#include <jni.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sstream>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <android/log.h>

// Подключаем ваши модули
#include "And64InlineHook.hpp"
#include "Logger.h"
#include "Utils.h" // Убедитесь, что тут есть Utf16ToUtf8 и GetObjectDump
#include "Il2Cpp.h"

// =================================================================
// 1. КЛАСС NETWORK SENDER (JSONL SENDER)
// =================================================================
class NetworkSender {
public:
    static NetworkSender& Instance() {
        static NetworkSender instance;
        return instance;
    }

    // Запуск сетевого потока
    void Start(const std::string& ip, int port) {
        if (isRunning) return;
        serverIp = ip;
        serverPort = port;
        isRunning = true;
        worker = std::thread(&NetworkSender::WorkerThread, this);
    }

    // Основной метод отправки логов
    void SendLog(const std::string& type, int tableId, const std::string& content) {
        if (!isRunning) return;

        // Формируем JSON вручную
        std::stringstream ss;
        ss << "{\"type\":\"" << type << "\",";
        ss << "\"tableId\":" << tableId << ",";
        ss << "\"data\":\"" << EscapeJson(content) << "\"}";
        
        std::string json = ss.str();

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            queue.push(json);
        }
        cv.notify_one();
    }

private:
    NetworkSender() : isRunning(false) {}
    ~NetworkSender() {
        isRunning = false;
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    std::string serverIp;
    int serverPort;
    bool isRunning;
    std::thread worker;
    std::queue<std::string> queue;
    std::mutex queueMutex;
    std::condition_variable cv;

    // Экранирование спецсимволов для валидного JSON
    std::string EscapeJson(const std::string& s) {
        std::string output;
        output.reserve(s.length());
        for (char c : s) {
            switch (c) {
                case '"': output += "\\\""; break;
                case '\\': output += "\\\\"; break;
                case '\b': output += "\\b"; break;
                case '\f': output += "\\f"; break;
                case '\n': output += "\\n"; break;
                case '\r': output += "\\r"; break;
                case '\t': output += "\\t"; break;
                default:
                    if ('\x00' <= c && c <= '\x1f') continue;
                    output += c;
            }
        }
        return output;
    }

    void WorkerThread() {
        LOGI("[Network] WorkerThread started");
        while (isRunning) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            struct sockaddr_in serv_addr;
            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(serverPort);
            inet_pton(AF_INET, serverIp.c_str(), &serv_addr.sin_addr);

            LOGI("[Network] Connecting to %s:%d...", serverIp.c_str(), serverPort);
            if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
                LOGE("[Network] Connect failed. Retry in 2s...");
                close(sock);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            LOGI("[Network] Connected!");

            try {
                while (isRunning) {
                    std::string payload;
                    {
                        std::unique_lock<std::mutex> lock(queueMutex);
                        cv.wait(lock, [this]{ return !queue.empty() || !isRunning; });
                        if (!isRunning) break;
                        if (queue.empty()) continue;

                        payload = queue.front();
                        queue.pop();
                    }
                    // Добавляем перенос строки для формата JSONL
                    payload += "\n";

                    ssize_t sentBytes = send(sock, payload.c_str(), payload.size(), MSG_NOSIGNAL);
                    if (sentBytes < 0) {
                        LOGE("[Network] Send error, reconnecting...");
                        break; 
                    }
                }
            } catch (...) {
                LOGE("[Network] Exception in loop");
            }
            close(sock);
        }
    }
};

// =================================================================
// 2. КОНФИГУРАЦИЯ ОФФСЕТОВ (GAME & UNITY)
// =================================================================

// --- GAME PACKET OFFSETS ---
const uintptr_t OFFSET_SEND_PACKET      = 0x6D2BC60; 
const uintptr_t OFFSET_DISPATCH_PACKET  = 0x6D2D14C; 

// --- UNITY HTTP OFFSETS (RVA) ---
const uintptr_t RVA_SendWebRequest = 0x91CE4E0;
const uintptr_t RVA_GetUrl         = 0x91CED88;
const uintptr_t RVA_GetMethod      = 0x91CE800;

// =================================================================
// 3. ФУНКЦИОНАЛЬНЫЕ УКАЗАТЕЛИ (ORIGINALS)
// =================================================================

// Packet Originals
void (*orig_SendPacket)(void* instance, void* packet, int tableId);
void (*orig_OnDispatchPacket)(void* instance, void* packet, int tableId);

// HTTP Originals
void* (*orig_SendWebRequest)(void* instance);
void* (*call_GetUrl)(void* instance); 
int   (*call_GetMethod)(void* instance);

// Spoofer Originals
void* (*orig_get_deviceModel)();
void* (*orig_get_deviceName)();
void* (*orig_get_processorType)();
void* (*orig_get_systemMemorySize)(); 
void* (*orig_get_installerName)();

// =================================================================
// 4. SPOOFERS (ANTI-EMULATOR / ANTI-CHEAT)
// =================================================================
void* H_get_deviceModel() {
    return il2cpp_string_new("Samsung SM-G991B"); // S21 Ultra
}

void* H_get_deviceName() {
    return il2cpp_string_new("Galaxy S21 5G");
}

void* H_get_processorType() {
    return il2cpp_string_new("ARM64-v8a Hexa-core Processor");
}

int H_get_systemMemorySize() {
    return 8192; // 8 GB RAM
}

void* H_get_installerName() {
    return il2cpp_string_new("com.android.vending"); // Play Store
}

// =================================================================
// 5. PACKET HOOKS (WITH MODIFICATION LOGIC)
// =================================================================

// Исходящие пакеты (Клиент -> Сервер)
void H_SendPacket(void* instance, void* packet, int tableId) {
    
    // === ЛОГИКА ПОДМЕНЫ ПОЛЯ (ИЗ СТАРОЙ ВЕРСИИ) ===
    if (packet && il2cpp_object_get_class) {
        void* klass = il2cpp_object_get_class(packet);
        if (klass) {
            // Ищем поле "system"
            void* field = il2cpp_class_get_field_from_name(klass, "system");
            if (field) {
                // Подменяем значение на "android"
                void* androidString = il2cpp_string_new("android");
                il2cpp_field_set_value(packet, field, androidString);
                
                // const char* name = il2cpp_class_get_name(klass);
                // LOGW("PATCHED: system -> android inside %s", name ? name : "Unknown");
            }
        }
    }
    // ==============================================

    // Снимаем дамп (Utils.h должен содержать GetObjectDump)
    std::string dump = GetObjectDump(packet);
    
    // Отправляем JSONL
    NetworkSender::Instance().SendLog("PACKET_OUT", tableId, dump);

    if (orig_SendPacket) orig_SendPacket(instance, packet, tableId);
}

// Входящие пакеты (Сервер -> Клиент)
void H_OnDispatchPacket(void* instance, void* packet, int tableId) {
    std::string dump = GetObjectDump(packet);
    
    // Отправляем JSONL
    NetworkSender::Instance().SendLog("PACKET_IN", tableId, dump);

    if (orig_OnDispatchPacket) orig_OnDispatchPacket(instance, packet, tableId);
}

// =================================================================
// 6. HTTP HOOK
// =================================================================
void* Hook_SendWebRequest(void* instance) {
    if (instance != nullptr) {
        int methodType = -1;
        if (call_GetMethod) methodType = call_GetMethod(instance);

        const char* methodStr = "UNKNOWN";
        switch (methodType) {
            case 0: methodStr = "GET"; break;
            case 1: methodStr = "POST"; break;
            case 2: methodStr = "PUT"; break;
            case 3: methodStr = "HEAD"; break;
            default: methodStr = "OTHER"; break;
        }

        std::string urlStr = "null";
        if (call_GetUrl) {
            void* urlObj = call_GetUrl(instance);
            if (urlObj) urlStr = Utf16ToUtf8((Il2CppString*)urlObj);
        }

        // Локальный лог
        LOGW(">>> [HttpHook] %s %s", methodStr, urlStr.c_str());

        // Сетевой лог: "GET http://..."
        std::string dump = std::string(methodStr) + " " + urlStr;
        NetworkSender::Instance().SendLog("HTTP_REQ", 100, dump);
    }
    return orig_SendWebRequest(instance);
}

// =================================================================
// 7. MAIN THREAD / INIT
// =================================================================
void* hack_thread(void*) {
    // 1. ЗАПУСК СЕТИ
    // Адрес сервера: 192.168.0.132, порт 5006
    LOGI(">>> Starting Network Sender...");
    NetworkSender::Instance().Start("192.168.0.132", 5006); 

    // 2. ОЖИДАНИЕ БИБЛИОТЕКИ
    LOGI(">>> Waiting for libil2cpp.so...");
    uintptr_t base_addr = 0;
    while ((base_addr = get_lib_addr("libil2cpp.so")) == 0) {
        usleep(100000);
    }
    sleep(3); // Пауза для надежности

    // 3. INIT API
    LOGI(">>> libil2cpp loaded. Init API...");
    void* handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (!InitIl2CppAPI(handle)) {
        LOGE("!!! Failed to load IL2CPP API !!!");
        return nullptr;
    }

    // 4. УСТАНОВКА ХУКОВ НА ПАКЕТЫ (OFFSET BASED)
    LOGI(">>> Hooking Packets...");
    A64HookFunction((void*)(base_addr + OFFSET_SEND_PACKET), (void*)H_SendPacket, (void**)&orig_SendPacket);
    A64HookFunction((void*)(base_addr + OFFSET_DISPATCH_PACKET), (void*)H_OnDispatchPacket, (void**)&orig_OnDispatchPacket);

    // 5. УСТАНОВКА ХУКОВ НА HTTP (RVA BASED)
    // Сначала резолвим вспомогательные функции Unity
    call_GetUrl    = (void* (*)(void*)) (base_addr + RVA_GetUrl);
    call_GetMethod = (int (*)(void*))   (base_addr + RVA_GetMethod);
    
    LOGI(">>> Hooking HTTP...");
    A64HookFunction((void*)(base_addr + RVA_SendWebRequest), (void*)Hook_SendWebRequest, (void**)&orig_SendWebRequest);

    // 6. УСТАНОВКА SPOOFERS (SYMBOL BASED)
    LOGI(">>> Applying Spoofers...");
    
    void* addr_Model = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_deviceModel", 0);
    if (addr_Model) A64HookFunction(addr_Model, (void*)H_get_deviceModel, (void**)&orig_get_deviceModel);

    void* addr_Name = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_deviceName", 0);
    if (addr_Name) A64HookFunction(addr_Name, (void*)H_get_deviceName, (void**)&orig_get_deviceName);

    void* addr_Cpu = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_processorType", 0);
    if (addr_Cpu) A64HookFunction(addr_Cpu, (void*)H_get_processorType, (void**)&orig_get_processorType);

    void* addr_Mem = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_systemMemorySize", 0);
    if (addr_Mem) A64HookFunction(addr_Mem, (void*)H_get_systemMemorySize, (void**)&orig_get_systemMemorySize);

    void* addr_Install = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "Application", "get_installerName", 0);
    if (addr_Install) A64HookFunction(addr_Install, (void*)H_get_installerName, (void**)&orig_get_installerName);

    LOGI("=== ULTIMATE SNIFFER (PACKETS + HTTP + SPOOF) READY ===");
    return nullptr;
}

void __attribute__((constructor)) init() {
    pthread_t t; pthread_create(&t, nullptr, hack_thread, nullptr);
}