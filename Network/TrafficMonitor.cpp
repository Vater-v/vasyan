#include "TrafficMonitor.h"
#include "NetworkSender.h"
#include "And64InlineHook.hpp"
#include "Il2Cpp.h"
#include "Logger.h"
#include <queue>
#include <mutex>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <random>
#include <atomic>

// =================================================================
// HELPERS (JSON PARSING & MAPPING)
// =================================================================
std::string GetJsonString(const std::string& json, const std::string& key) {
    std::string qKey = "\"" + key + "\":";
    size_t pos = json.find(qKey);
    if (pos == std::string::npos) return "";
    
    pos += qKey.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\"')) pos++;
    
    size_t end = pos;
    while (end < json.length() && json[end] != '\"' && json[end] != ',' && json[end] != '}') end++;
    
    return json.substr(pos, end - pos);
}

// Маппинг строковых действий от Python к Enum игры (PP.PPPoker.GameActionType / Pb.ActionType)
int GetActionTypeValue(const std::string& typeStr) {
    if (typeStr == "ACTION_FOLD") return 1;
    if (typeStr == "ACTION_CHECK") return 2;
    if (typeStr == "ACTION_CALL") return 3;
    if (typeStr == "ACTION_RAISE") return 4;
    if (typeStr == "ACTION_BET") return 6; // Внимание: проверьте реальный ID, в дампе было BET=6? или используем RAISE=4
    if (typeStr == "ACTION_ALLIN") return 201;
    return 0; // NONE
}

// =================================================================
// GLOBALS & CONFIG
// =================================================================

// --- OFFSETS ---
const uintptr_t OFFSET_SEND_PACKET      = 0x6D2BC60; 
const uintptr_t OFFSET_DISPATCH_PACKET  = 0x6D2D14C; 
const uintptr_t RVA_SendWebRequest = 0x91CE4E0;
const uintptr_t RVA_GetUrl         = 0x91CED88;
const uintptr_t RVA_GetMethod      = 0x91CE800;

// --- ORIGINALS ---
static void (*orig_SendPacket)(void* instance, void* packet, int tableId);
static void (*orig_OnDispatchPacket)(void* instance, void* packet, int tableId);
static void* (*orig_SendWebRequest)(void* instance);
static void* (*call_GetUrl)(void* instance); 
static int   (*call_GetMethod)(void* instance);

// --- STATE ---
void* g_NetInstance = nullptr; 
int   g_LastTableId = -1; // Сохраняем ID стола для отправки

struct ActionData {
    int seatId;
    std::string actionType;
    std::string chips;
};

std::queue<ActionData> g_actionQueue;
std::mutex g_actionMutex;
std::condition_variable g_actionCv;
std::atomic<bool> g_botRunning{true};
std::thread g_botThread;

// =================================================================
// ACTION LOGIC (EXECUTED IN BOT THREAD)
// =================================================================

// Поиск класса Pb.ActionREQ
void* GetActionReqClass() {
    static void* klass = nullptr;
    if (klass) return klass;

    if (!il2cpp_domain_get) return nullptr;
    void* domain = il2cpp_domain_get();
    size_t size = 0;
    void** assemblies = il2cpp_domain_get_assemblies(domain, &size);

    for (size_t i = 0; i < size; ++i) {
        void* image = il2cpp_assembly_get_image(assemblies[i]);
        // Ищем класс в сборке, где находятся Protobuf сообщения. Обычно это Assembly-CSharp или отдельная dll.
        // Пробуем найти Pb.ActionREQ
        void* cls = il2cpp_class_from_name(image, "Pb", "ActionREQ");
        if (cls) {
            klass = cls;
            LOGI(">>> Found Pb.ActionREQ class!");
            return klass;
        }
    }
    LOGE("!!! Pb.ActionREQ class NOT FOUND !!!");
    return nullptr;
}

void PerformAction(const ActionData& act) {
    if (!g_NetInstance || !orig_SendPacket) {
        LOGE("Cannot perform action: No NetInstance or OrigFunc");
        return;
    }
    
    if (g_LastTableId == -1) {
        LOGE("Cannot perform action: Unknown TableID");
        return;
    }

    void* reqClass = GetActionReqClass();
    if (!reqClass) return;

    // 1. Создаем объект ActionREQ
    void* actionPacket = il2cpp_object_new(reqClass);
    if (!actionPacket) {
        LOGE("Failed to instantiate ActionREQ");
        return;
    }

    // 2. Устанавливаем поля
    // Поля в Protobuf обычно приватные с подчеркиванием: actionType_, chips_
    void* fAction = il2cpp_class_get_field_from_name(reqClass, "actionType_");
    void* fChips  = il2cpp_class_get_field_from_name(reqClass, "chips_");

    if (fAction) {
        int typeVal = GetActionTypeValue(act.actionType);
        il2cpp_field_set_value(actionPacket, fAction, &typeVal);
    } else {
        LOGE("Field actionType_ not found");
    }

    if (fChips) {
        // chips_ это int64 (long)
        long long chipsVal = std::atoll(act.chips.c_str());
        il2cpp_field_set_value(actionPacket, fChips, &chipsVal);
    } else {
        LOGE("Field chips_ not found");
    }

    // 3. Отправляем
    LOGI(">>> [BOT] Sending Action: %s (Val: %d), Chips: %s", act.actionType.c_str(), GetActionTypeValue(act.actionType), act.chips.c_str());
    
    // Вызов оригинальной функции отправки
    orig_SendPacket(g_NetInstance, actionPacket, g_LastTableId);
}

void BotWorkerLoop() {
    LOGI(">>> Bot Worker Thread Started");
    
    // ВАЖНО: Прикрепляем поток к IL2CPP, иначе il2cpp_object_new крашнет игру
    void* il2cppThread = nullptr;
    if (il2cpp_thread_attach && il2cpp_domain_get) {
        il2cppThread = il2cpp_thread_attach(il2cpp_domain_get());
        LOGI(">>> Bot thread attached to IL2CPP");
    } else {
        LOGE("!!! IL2CPP Thread Attach API missing. Bot may crash !!!");
    }

    // Генератор случайных чисел
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(500, 5000); // 0.5s - 5s

    while (g_botRunning) {
        ActionData act;
        {
            std::unique_lock<std::mutex> lock(g_actionMutex);
            g_actionCv.wait(lock, []{ return !g_actionQueue.empty() || !g_botRunning; });

            if (!g_botRunning) break;
            
            act = g_actionQueue.front();
            g_actionQueue.pop();
        }

        // Рандомная задержка перед ходом
        int delay = distrib(gen);
        LOGI(">>> [BOT] Thinking for %d ms...", delay);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        PerformAction(act);
    }

    if (il2cppThread && il2cpp_thread_detach) {
        il2cpp_thread_detach(il2cppThread);
    }
    LOGI(">>> Bot Worker Thread Stopped");
}

// =================================================================
// HOOKS
// =================================================================

void H_SendPacket(void* instance, void* packet, int tableId) {
    if (g_NetInstance == nullptr) {
        g_NetInstance = instance;
        LOGI(">>> NetInstance captured: %p", instance);
    }
    // Запоминаем последний активный стол
    g_LastTableId = tableId;
    
    // Анти-чит спуфер (оставляем как было)
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

void H_OnDispatchPacket(void* instance, void* packet, int tableId) {
    if (g_NetInstance == nullptr) {
        g_NetInstance = instance;
    }
    g_LastTableId = tableId;

    std::string dump = GetObjectDump(packet);
    NetworkSender::Instance().SendLog("PACKET_IN", tableId, dump);
    
    // Здесь больше не вызываем ProcessActionQueue, это делает BotWorkerLoop

    if (orig_OnDispatchPacket) orig_OnDispatchPacket(instance, packet, tableId);
}

void* Hook_SendWebRequest(void* instance) {
    if (instance != nullptr) {
        int methodType = -1;
        if (call_GetMethod) methodType = call_GetMethod(instance);
        std::string urlStr = "null";
        if (call_GetUrl) {
            void* urlObj = call_GetUrl(instance);
            if (urlObj) urlStr = Utf16ToUtf8((Il2CppString*)urlObj);
        }
        std::string dump = std::to_string(methodType) + " " + urlStr;
        NetworkSender::Instance().SendLog("HTTP_REQ", 100, dump);
    }
    return orig_SendWebRequest(instance);
}

// =================================================================
// EXTERNAL INTERFACE
// =================================================================

void OnServerMessage(const std::string& json) {
    LOGI(">>> OnServerMessage processing: %s", json.c_str());

    std::string msgType = GetJsonString(json, "message");
    if (msgType == "ActionREQ") {
        ActionData data;
        size_t payloadPos = json.find("\"payload\":");
        if (payloadPos != std::string::npos) {
            std::string payload = json.substr(payloadPos);
            
            // Парсим SeatID (может пригодиться для проверки, но отправляем мы всегда от себя)
            std::string seatStr = GetJsonString(payload, "seatid");
            data.seatId = seatStr.empty() ? 0 : std::atoi(seatStr.c_str());
            
            data.actionType = GetJsonString(payload, "actionType");
            data.chips = GetJsonString(payload, "chips");
            
            {
                std::lock_guard<std::mutex> lock(g_actionMutex);
                g_actionQueue.push(data);
            }
            g_actionCv.notify_one(); // Будим поток бота
            
            LOGI(">>> Action Queued: %s %s", data.actionType.c_str(), data.chips.c_str());
        } else {
            LOGW(">>> ActionREQ received but no payload found!");
        }
    }
}

void InitTrafficMonitor(uintptr_t base_addr) {
    LOGI(">>> [TrafficMonitor] Hooking Packets...");
    A64HookFunction((void*)(base_addr + OFFSET_SEND_PACKET), (void*)H_SendPacket, (void**)&orig_SendPacket);
    A64HookFunction((void*)(base_addr + OFFSET_DISPATCH_PACKET), (void*)H_OnDispatchPacket, (void**)&orig_OnDispatchPacket);

    LOGI(">>> [TrafficMonitor] Hooking HTTP...");
    call_GetUrl    = (void* (*)(void*)) (base_addr + RVA_GetUrl);
    call_GetMethod = (int (*)(void*))   (base_addr + RVA_GetMethod);
    A64HookFunction((void*)(base_addr + RVA_SendWebRequest), (void*)Hook_SendWebRequest, (void**)&orig_SendWebRequest);
    
    // Запускаем поток бота
    g_botThread = std::thread(BotWorkerLoop);
    g_botThread.detach();

    LOGI(">>> [TrafficMonitor] Done.");
}