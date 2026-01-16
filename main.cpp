#include <jni.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string>

// Подключаем модули
#include "And64InlineHook.hpp"
#include "Logger.h"
#include "Utils.h"
#include "Il2Cpp.h"
#include "NetworkSender.h" // Используем вынесенный класс
#include "Spoofer.h"       // Наш новый модуль

// =================================================================
// 1. КОНФИГУРАЦИЯ ОФФСЕТОВ (GAME & UNITY)
// =================================================================

// --- GAME PACKET OFFSETS ---
const uintptr_t OFFSET_SEND_PACKET      = 0x6D2BC60; 
const uintptr_t OFFSET_DISPATCH_PACKET  = 0x6D2D14C; 

// --- UNITY HTTP OFFSETS (RVA) ---
const uintptr_t RVA_SendWebRequest = 0x91CE4E0;
const uintptr_t RVA_GetUrl         = 0x91CED88;
const uintptr_t RVA_GetMethod      = 0x91CE800;

// =================================================================
// 2. ФУНКЦИОНАЛЬНЫЕ УКАЗАТЕЛИ (ORIGINALS)
// =================================================================

// Packet Originals
void (*orig_SendPacket)(void* instance, void* packet, int tableId);
void (*orig_OnDispatchPacket)(void* instance, void* packet, int tableId);

// HTTP Originals
void* (*orig_SendWebRequest)(void* instance);
void* (*call_GetUrl)(void* instance); 
int   (*call_GetMethod)(void* instance);

// =================================================================
// 3. PACKET HOOKS
// =================================================================

// Исходящие пакеты (Клиент -> Сервер)
void H_SendPacket(void* instance, void* packet, int tableId) {
    // Подмена значения system -> android (пример)
    if (packet && il2cpp_object_get_class) {
        void* klass = il2cpp_object_get_class(packet);
        if (klass) {
            void* field = il2cpp_class_get_field_from_name(klass, "system");
            if (field) {
                void* androidString = il2cpp_string_new("android");
                il2cpp_field_set_value(packet, field, androidString);
            }
        }
    }

    std::string dump = GetObjectDump(packet);
    NetworkSender::Instance().SendLog("PACKET_OUT", tableId, dump);

    if (orig_SendPacket) orig_SendPacket(instance, packet, tableId);
}

// Входящие пакеты (Сервер -> Клиент)
void H_OnDispatchPacket(void* instance, void* packet, int tableId) {
    std::string dump = GetObjectDump(packet);
    NetworkSender::Instance().SendLog("PACKET_IN", tableId, dump);

    if (orig_OnDispatchPacket) orig_OnDispatchPacket(instance, packet, tableId);
}

// =================================================================
// 4. HTTP HOOK
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

        LOGW(">>> [HttpHook] %s %s", methodStr, urlStr.c_str());
        std::string dump = std::string(methodStr) + " " + urlStr;
        NetworkSender::Instance().SendLog("HTTP_REQ", 100, dump);
    }
    return orig_SendWebRequest(instance);
}

// =================================================================
// 5. MAIN THREAD / INIT
// =================================================================
void* hack_thread(void*) {
    // 1. ЗАПУСК СЕТИ
    LOGI(">>> Starting Network Sender...");
    NetworkSender::Instance().Start("192.168.0.132", 5006); 

    // 2. ОЖИДАНИЕ БИБЛИОТЕКИ
    LOGI(">>> Waiting for libil2cpp.so...");
    uintptr_t base_addr = 0;
    while ((base_addr = get_lib_addr("libil2cpp.so")) == 0) {
        usleep(100000);
    }
    sleep(3); 

    // 3. INIT API
    LOGI(">>> libil2cpp loaded. Init API...");
    void* handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (!InitIl2CppAPI(handle)) {
        LOGE("!!! Failed to load IL2CPP API !!!");
        return nullptr;
    }

    // 4. УСТАНОВКА ХУКОВ НА ПАКЕТЫ
    LOGI(">>> Hooking Packets...");
    A64HookFunction((void*)(base_addr + OFFSET_SEND_PACKET), (void*)H_SendPacket, (void**)&orig_SendPacket);
    A64HookFunction((void*)(base_addr + OFFSET_DISPATCH_PACKET), (void*)H_OnDispatchPacket, (void**)&orig_OnDispatchPacket);

    // 5. УСТАНОВКА ХУКОВ НА HTTP
    call_GetUrl    = (void* (*)(void*)) (base_addr + RVA_GetUrl);
    call_GetMethod = (int (*)(void*))   (base_addr + RVA_GetMethod);
    LOGI(">>> Hooking HTTP...");
    A64HookFunction((void*)(base_addr + RVA_SendWebRequest), (void*)Hook_SendWebRequest, (void**)&orig_SendWebRequest);

    // 6. ЗАПУСК SPOOFER (ТЕПЕРЬ ОДНОЙ СТРОКОЙ)
    InitSpoofers();

    LOGI("=== ULTIMATE SNIFFER READY ===");
    return nullptr;
}

void __attribute__((constructor)) init() {
    pthread_t t; pthread_create(&t, nullptr, hack_thread, nullptr);
}