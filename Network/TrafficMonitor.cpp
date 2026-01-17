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

// Ищем Update динамически, чтобы не гадать смещение
// const uintptr_t RVA_Debug_Log = ...; // УБИРАЕМ, ЭТО МЕРТВЫЙ НОМЕР

// HoldemActionButtons (Жизненный цикл)
const uintptr_t RVA_Buttons_Initial     = 0x741B7D0; 
const uintptr_t RVA_Buttons_OnDispose   = 0x741D144; 

// ГЛАВНЫЙ МЕТОД ДЛЯ ДЕЙСТВИЙ (SendRequestAction)
// public void SendRequestAction(ActionType actType, long actChips = 0L)
const uintptr_t RVA_SendRequest         = 0x741ACB8; 

// Managers
const uintptr_t RVA_Manager_GetTid      = 0x7163438;

// Networking
const uintptr_t OFFSET_SEND_PACKET      = 0x6D2BC60; 
const uintptr_t OFFSET_DISPATCH_PACKET  = 0x6D2D14C;

// =================================================================
// GLOBALS & TYPEDEFS
// =================================================================

// ВАЖНО: Добавляем MethodInfo* (void* method) в конец сигнатур!
void (*orig_Buttons_Initial)(void* instance, void* manager, void* method);
void (*orig_Buttons_Dispose)(void* instance, void* method);
void (*orig_Update)(void* instance, void* method); // Хук для Update
void (*orig_SendPacket)(void*, void*, int, void*);
void (*orig_OnDispatchPacket)(void*, void*, int, void*);

// Функции игры
int  (*call_GetTid)(void* manager, void* method);
// SendRequestAction(ActionType type, long chips)
void (*call_SendRequest)(void* instance, int type, int64_t chips, void* method);

// Состояние
std::map<int, void*> g_TableUI; // TableID -> HoldemActionButtons Instance
std::mutex g_tableMutex;
void* g_CurrentUpdateInstance = nullptr; // Экземпляр, на котором крутится Update

struct ActionData {
    int tableId;
    std::string actionType;
    int64_t chips;
};

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

// Конвертация строк в ActionType (проверьте значения enum в DnSpy/Il2CppDumper!)
int GetActionTypeValue(const std::string& typeStr) {
    if (typeStr == "ACTION_FOLD")  return 1; // Pb.ActionType.Fold
    if (typeStr == "ACTION_CHECK") return 2; // Pb.ActionType.Check
    if (typeStr == "ACTION_CALL")  return 3; // Pb.ActionType.Call
    if (typeStr == "ACTION_RAISE") return 4; // Pb.ActionType.Raise
    if (typeStr == "ACTION_BET")   return 7; // Pb.ActionType.Bet
    if (typeStr == "ACTION_ALLIN") return 201; // Pb.ActionType.AllIn (Пример)
    return 0;
}

// =================================================================
// 🟢 EXECUTION (MAIN THREAD)
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
        // Если ID стола не найден, пробуем использовать тот, чей Update сейчас работает
        // Это костыль, но спасет если играете 1 стол
        if (g_CurrentUpdateInstance) {
             uiInstance = g_CurrentUpdateInstance;
             LOGW(">>> [BOT] TableID %d not found, using Active Instance!", act.tableId);
        } else {
             LOGE(">>> [BOT] No UI instance found for table %d", act.tableId);
             return;
        }
    }

    int typeVal = GetActionTypeValue(act.actionType);
    LOGI(">>> [EXEC] %s (Enum: %d, Chips: %lld) -> Instance: %p", act.actionType.c_str(), typeVal, (long long)act.chips, uiInstance);
    if (call_SendRequest) {
        // Передаем nullptr вместо MethodInfo, это безопасно для большинства методов
        call_SendRequest(uiInstance, typeVal, act.chips, nullptr);
    } else {
        LOGE("!!! call_SendRequest is NULL !!!");
    }
}

// =================================================================
// 🟢 HOOKS
// =================================================================

// 1. UPDATE HOOK (Вместо Debug.Log)
// Этот метод вызывается каждый кадр на экземпляре HoldemActionButtons
void H_HoldemActionButtons_Update(void* instance, void* method) {
    // Сохраняем активный инстанс (на случай если GetTid подвел)
    g_CurrentUpdateInstance = instance;

    // 1. Вызываем оригинал (чтобы игра работала)
    if (orig_Update) orig_Update(instance, method);

    // 2. Обрабатываем очередь
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

// 2. UI CAPTURE
void H_Buttons_Initial(void* instance, void* manager, void* method) {
    if (orig_Buttons_Initial) orig_Buttons_Initial(instance, manager, method);

    if (manager && call_GetTid) {
        int tid = call_GetTid(manager, nullptr);
        std::lock_guard<std::mutex> lock(g_tableMutex);
        g_TableUI[tid] = instance;
        LOGI(">>> [HOOK] Captured UI for Table %d @ %p", tid, instance);
    }
}

void H_Buttons_Dispose(void* instance, void* method) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    for (auto it = g_TableUI.begin(); it != g_TableUI.end(); ) {
        if (it->second == instance) {
            it = g_TableUI.erase(it);
        } else {
            ++it;
        }
    }
    if (instance == g_CurrentUpdateInstance) g_CurrentUpdateInstance = nullptr;
    if (orig_Buttons_Dispose) orig_Buttons_Dispose(instance, method);
}

// 3. NET LOGS
void H_SendPacket(void* instance, void* packet, int tableId, void* method) {
    if (NetworkSender::Instance().IsRunning()) {
        std::string dump = GetObjectDump(packet);
        NetworkSender::Instance().SendLog("PACKET_OUT", tableId, dump);
    }
    if (orig_SendPacket) orig_SendPacket(instance, packet, tableId, method);
}

void H_OnDispatchPacket(void* instance, void* packet, int tableId, void* method) {
    if (NetworkSender::Instance().IsRunning()) {
        std::string dump = GetObjectDump(packet);
        NetworkSender::Instance().SendLog("PACKET_IN", tableId, dump);
    }
    if (orig_OnDispatchPacket) orig_OnDispatchPacket(instance, packet, tableId, method);
}

// =================================================================
// SERVER CALLBACK
// =================================================================

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

            LOGI(">>> [NET] Recv Action: %s. Queueing...", data.actionType.c_str());

            {
                std::lock_guard<std::mutex> lock(g_actionMutex);
                g_actionQueue.push(data);
            }
        }
    }
}

// =================================================================
// INITIALIZATION
// =================================================================

void InitTrafficMonitor(uintptr_t base_addr) {
    LOGI(">>> [Init] TrafficMonitor: Starting...");

    // 1. Указатели на методы (С учетом MethodInfo*)
    call_GetTid      = (int(*)(void*, void*))          (base_addr + RVA_Manager_GetTid);
    call_SendRequest = (void(*)(void*,int,int64_t,void*))(base_addr + RVA_SendRequest);

    // 2. Ищем Update динамически
    // Так как HoldemActionButtons наследуется, метод Update может быть в нем, либо в родителе
    // Используем GetMethodAddress (nullptr в первом аргументе ищет во всех сборках)
    void* addr_Update = GetMethodAddress(nullptr, "PP.PPPoker", "HoldemActionButtons", "Update", 0);
    
    if (!addr_Update) {
        LOGW("HoldemActionButtons.Update not found! Trying LateUpdate...");
        addr_Update = GetMethodAddress(nullptr, "PP.PPPoker", "HoldemActionButtons", "LateUpdate", 0);
    }
    
    if (!addr_Update) {
        LOGE("!!! FATAL: Could not find Update loop hook. Bot will NOT work. !!!");
    } else {
        LOGI(">>> Hooking Update Loop at: %p", addr_Update);
        A64HookFunction(addr_Update, (void*)H_HoldemActionButtons_Update, (void**)&orig_Update);
    }

    // 3. Статические хуки
    A64HookFunction((void*)(base_addr + RVA_Buttons_Initial), (void*)H_Buttons_Initial, (void**)&orig_Buttons_Initial);
    A64HookFunction((void*)(base_addr + RVA_Buttons_OnDispose), (void*)H_Buttons_Dispose, (void**)&orig_Buttons_Dispose);
    
    // Сеть
    A64HookFunction((void*)(base_addr + OFFSET_SEND_PACKET), (void*)H_SendPacket, (void**)&orig_SendPacket);
    A64HookFunction((void*)(base_addr + OFFSET_DISPATCH_PACKET), (void*)H_OnDispatchPacket, (void**)&orig_OnDispatchPacket);

    LOGI(">>> [Init] Done.");
}