#include "TrafficMonitor.h"
#include "NetworkSender.h"
#include "And64InlineHook.hpp"
#include "Il2Cpp.h"
#include "Logger.h"
#include <queue>
#include <mutex>
#include <cstdlib>

// =================================================================
// HELPERS (JSON PARSING)
// =================================================================
std::string GetJsonString(const std::string& json, const std::string& key) {
    std::string qKey = "\"" + key + "\":";
    size_t pos = json.find(qKey);
    if (pos == std::string::npos) return "";
    
    pos += qKey.length();
    // Skip spaces
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\"')) pos++;
    
    size_t end = pos;
    while (end < json.length() && json[end] != '\"' && json[end] != ',' && json[end] != '}') end++;
    
    return json.substr(pos, end - pos);
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
struct ActionData {
    int seatId;
    std::string actionType;
    std::string chips;
};
std::queue<ActionData> g_actionQueue;
std::mutex g_actionMutex;

// =================================================================
// ACTION LOGIC
// =================================================================

void PerformAction(const ActionData& act) {
    if (!g_NetInstance || !orig_SendPacket) {
        LOGE("Cannot perform action: No NetInstance or OrigFunc");
        return;
    }
    
    // ВАЖНО: Тут должен быть код создания пакета.
    LOGI(">>> EXECUTING ACTION: %s (Chips: %s, Seat: %d)", act.actionType.c_str(), act.chips.c_str(), act.seatId);
    
    // Пока просто логируем, так как нет конструктора ActionREQ
}

void ProcessActionQueue() {
    std::lock_guard<std::mutex> lock(g_actionMutex);
    while (!g_actionQueue.empty()) {
        ActionData act = g_actionQueue.front();
        g_actionQueue.pop();
        PerformAction(act);
    }
}

// =================================================================
// HOOKS
// =================================================================

void H_SendPacket(void* instance, void* packet, int tableId) {
    if (g_NetInstance == nullptr) {
        g_NetInstance = instance;
        LOGI(">>> NetInstance captured: %p", instance);
    }
    
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

    std::string dump = GetObjectDump(packet);
    NetworkSender::Instance().SendLog("PACKET_IN", tableId, dump);
    
    // Выполняем действия бота здесь (в потоке игры)
    ProcessActionQueue();

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
    // ЛОГИРУЕМ ТО, ЧТО ПОПАЛО В ПАРСЕР
    LOGI(">>> OnServerMessage processing: %s", json.c_str());

    std::string msgType = GetJsonString(json, "message");
    if (msgType == "ActionREQ") {
        ActionData data;
        size_t payloadPos = json.find("\"payload\":");
        if (payloadPos != std::string::npos) {
            std::string payload = json.substr(payloadPos);
            data.seatId = std::atoi(GetJsonString(payload, "seatid").c_str());
            data.actionType = GetJsonString(payload, "actionType");
            data.chips = GetJsonString(payload, "chips");
            
            std::lock_guard<std::mutex> lock(g_actionMutex);
            g_actionQueue.push(data);
            LOGI(">>> Action Queued Successfully: %s", data.actionType.c_str());
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
    
    LOGI(">>> [TrafficMonitor] Done.");
}