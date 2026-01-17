#include "TrafficMonitor.h"
#include "NetworkSender.h"
#include "And64InlineHook.hpp"
#include "Il2Cpp.h"
#include "Logger.h"
#include <queue>
#include <mutex>
#include <cstdlib>
#include <string>
#include <map>

// =================================================================
// 🟢 OFFSETS (RVAs)
// =================================================================

// Main Thread "Pulse" (Хук для выполнения кода в главном потоке)
const uintptr_t RVA_Debug_Log           = 0x8E614EC; // UnityEngine.Debug.Log(object)

// HoldemActionButtons (Жизненный цикл)
const uintptr_t RVA_Buttons_Initial     = 0x741B7D0; 
const uintptr_t RVA_Buttons_OnDispose   = 0x741D144; 

// Button Actions (Конкретные методы нажатий из старого кода/дампа)
const uintptr_t RVA_Check_IsVisible     = 0x741D6B4;
const uintptr_t RVA_OnFold              = 0x741D3F8;
const uintptr_t RVA_OnCheck             = 0x741DBD8;
const uintptr_t RVA_OnCall              = 0x741DC90;
// Для ставок используем универсальный метод, так как он принимает сумму
const uintptr_t RVA_SendRequest         = 0x741ACB8; 

// Managers
const uintptr_t RVA_Manager_GetTid      = 0x7163438;

// Networking (Для логов)
const uintptr_t OFFSET_SEND_PACKET      = 0x6D2BC60; 
const uintptr_t OFFSET_DISPATCH_PACKET  = 0x6D2D14C;

// =================================================================
// GLOBALS
// =================================================================

// Оригиналы хуков
void (*orig_DebugLog)(void* msg);
void (*orig_Buttons_Initial)(void* instance, void* manager);
void (*orig_Buttons_Dispose)(void* instance);
void (*orig_SendPacket)(void*, void*, int);
void (*orig_OnDispatchPacket)(void*, void*, int);

// Указатели на функции игры
int  (*call_GetTid)(void* manager);
bool (*call_Check_IsVisible)(void* instance);
void (*call_OnFold)(void* instance);
void (*call_OnCheck)(void* instance);
void (*call_OnCall)(void* instance);
void (*call_SendRequest)(void* instance, int type, int64_t chips);

// Состояние
std::map<int, void*> g_TableUI;
std::mutex g_tableMutex;

struct ActionData {
    int tableId;
    std::string actionType;
    int64_t chips;
};

// Очередь действий (Network Thread -> Main Thread)
std::queue<ActionData> g_actionQueue;
std::mutex g_actionMutex;

// =================================================================
// HELPERS
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

int GetActionTypeValue(const std::string& typeStr) {
    if (typeStr == "ACTION_FOLD") return 1;
    if (typeStr == "ACTION_CHECK") return 2;
    if (typeStr == "ACTION_CALL") return 3;
    if (typeStr == "ACTION_RAISE") return 4;
    if (typeStr == "ACTION_BET") return 7;
    if (typeStr == "ACTION_ALLIN") return 201;
    return 0;
}

// =================================================================
// 🟢 ИСПОЛНЕНИЕ (MAIN THREAD)
// =================================================================

void PerformActionSafe(const ActionData& act) {
    void* uiInstance = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_tableMutex);
        if (g_TableUI.find(act.tableId) != g_TableUI.end()) {
            uiInstance = g_TableUI[act.tableId];
        }
    }

    if (!uiInstance) {
        // Не нашли UI для стола
        return;
    }

    LOGI(">>> [MAIN THREAD] Executing %s (Chips: %lld) for Table %d", act.actionType.c_str(), act.chips, act.tableId);

    // 1. FOLD
    if (act.actionType == "ACTION_FOLD") {
        if (call_OnFold) call_OnFold(uiInstance);
    }
    // 2. CHECK (с проверкой, как в старой версии)
    else if (act.actionType == "ACTION_CHECK") {
        bool canCheck = false;
        if (call_Check_IsVisible) canCheck = call_Check_IsVisible(uiInstance);

        if (canCheck) {
            if (call_OnCheck) call_OnCheck(uiInstance);
        } else {
            LOGW(">>> [BOT] Check not visible! Fallback to FOLD.");
            if (call_OnFold) call_OnFold(uiInstance);
        }
    }
    // 3. CALL
    else if (act.actionType == "ACTION_CALL") {
        if (call_OnCall) call_OnCall(uiInstance);
    }
    // 4. BET / RAISE / ALLIN (Тут нужны аргументы суммы, используем SendRequest)
    else {
        int typeVal = GetActionTypeValue(act.actionType);
        if (call_SendRequest) call_SendRequest(uiInstance, typeVal, act.chips);
    }
}

// =================================================================
// 🟢 HOOKS
// =================================================================

// ГЛАВНЫЙ ПОТОК: Паразитируем на Debug.Log
void H_DebugLog(void* msg) {
    // 1. Вызываем оригинал
    if (orig_DebugLog) orig_DebugLog(msg);

    // 2. Разгребаем очередь действий
    while (true) {
        ActionData act;
        bool hasAction = false;
        {
            std::lock_guard<std::mutex> lock(g_actionMutex);
            if (!g_actionQueue.empty()) {
                act = g_actionQueue.front();
                g_actionQueue.pop();
                hasAction = true;
            }
        }

        if (hasAction) {
            PerformActionSafe(act);
        } else {
            break;
        }
    }
}

// ЛОВЛЯ UI СТОЛА
void H_Buttons_Initial(void* instance, void* manager) {
    if (orig_Buttons_Initial) orig_Buttons_Initial(instance, manager);

    if (manager && call_GetTid) {
        int tid = call_GetTid(manager);
        std::lock_guard<std::mutex> lock(g_tableMutex);
        g_TableUI[tid] = instance;
        LOGI(">>> [HOOK] UI Captured for TableID: %d | Addr: %p", tid, instance);
    }
}

// ОЧИСТКА UI
void H_Buttons_Dispose(void* instance) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    for (auto it = g_TableUI.begin(); it != g_TableUI.end(); ) {
        if (it->second == instance) {
            it = g_TableUI.erase(it);
        } else {
            ++it;
        }
    }
    if (orig_Buttons_Dispose) orig_Buttons_Dispose(instance);
}

// СЕТЕВЫЕ ХУКИ (Только логи)
void H_SendPacket(void* instance, void* packet, int tableId) {
    std::string dump = GetObjectDump(packet);
    NetworkSender::Instance().SendLog("PACKET_OUT", tableId, dump);
    if (orig_SendPacket) orig_SendPacket(instance, packet, tableId);
}

void H_OnDispatchPacket(void* instance, void* packet, int tableId) {
    std::string dump = GetObjectDump(packet);
    NetworkSender::Instance().SendLog("PACKET_IN", tableId, dump);
    if (orig_OnDispatchPacket) orig_OnDispatchPacket(instance, packet, tableId);
}

// =================================================================
// INIT & NET HANDLER
// =================================================================

// Вызывается из NetworkSender (из другого потока!)
void OnServerMessage(const std::string& json) {
    std::string msgType = GetJsonString(json, "message");
    
    if (msgType == "ActionREQ") {
        ActionData data;
        size_t pPos = json.find("\"payload\":");
        if (pPos != std::string::npos) {
            std::string p = json.substr(pPos);
            
            data.actionType = GetJsonString(p, "actionType");
            std::string chipsStr = GetJsonString(p, "chips");
            data.chips = chipsStr.empty() ? 0 : std::atoll(chipsStr.c_str());
            
            std::string tStr = GetJsonString(p, "tableId");
            data.tableId = tStr.empty() ? 0 : std::atoi(tStr.c_str());

            LOGI(">>> [NET] Recv Action: %s (Queueing for Main Thread)", data.actionType.c_str());

            // Просто кладем в очередь, исполнит MainThread (H_DebugLog)
            {
                std::lock_guard<std::mutex> lock(g_actionMutex);
                g_actionQueue.push(data);
            }
        }
    }
}

void InitTrafficMonitor(uintptr_t base_addr) {
    LOGI(">>> [Init] TrafficMonitor: Setting up Main Thread Hooks...");

    // 1. Резолвим указатели (Offsets)
    call_GetTid          = (int(*)(void*))          (base_addr + RVA_Manager_GetTid);
    call_Check_IsVisible = (bool(*)(void*))         (base_addr + RVA_Check_IsVisible);
    call_OnFold          = (void(*)(void*))         (base_addr + RVA_OnFold);
    call_OnCheck         = (void(*)(void*))         (base_addr + RVA_OnCheck);
    call_OnCall          = (void(*)(void*))         (base_addr + RVA_OnCall);
    call_SendRequest     = (void(*)(void*,int,int64_t))(base_addr + RVA_SendRequest);

    // 2. Ставим хуки
    // Главный цикл (Main Thread Pump)
    A64HookFunction((void*)(base_addr + RVA_Debug_Log), (void*)H_DebugLog, (void**)&orig_DebugLog);

    // UI Capture
    A64HookFunction((void*)(base_addr + RVA_Buttons_Initial), (void*)H_Buttons_Initial, (void**)&orig_Buttons_Initial);
    A64HookFunction((void*)(base_addr + RVA_Buttons_OnDispose), (void*)H_Buttons_Dispose, (void**)&orig_Buttons_Dispose);

    // Network Logs
    A64HookFunction((void*)(base_addr + OFFSET_SEND_PACKET), (void*)H_SendPacket, (void**)&orig_SendPacket);
    A64HookFunction((void*)(base_addr + OFFSET_DISPATCH_PACKET), (void*)H_OnDispatchPacket, (void**)&orig_OnDispatchPacket);

    LOGI(">>> [Init] Hooks Installed. Waiting for Debug.Log pulse...");
}