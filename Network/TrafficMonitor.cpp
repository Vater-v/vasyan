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
#include <map>

// =================================================================
// 🟢 1. ЖЕСТКО ЗАДАННЫЕ АДРЕСА (ИЗ ТВОИХ ФАЙЛОВ)
// =================================================================

// Из HoldemActionButtons.cs
const uintptr_t RVA_Buttons_Initial     = 0x741B7D0; // public void Initial(HoldemManager manager)
const uintptr_t RVA_Buttons_SendAction  = 0x741ACB8; // public void SendRequestAction(ActionType actType, long actChips)
const uintptr_t RVA_Buttons_OnDispose   = 0x741D144; // protected override void OnDispose()

// Из HoldemManager.cs
const uintptr_t RVA_Manager_GetTid      = 0x7163438; // public int GetTid()

// Сетевые (оставляем для чтения логов)
const uintptr_t OFFSET_SEND_PACKET      = 0x6D2BC60; 
const uintptr_t OFFSET_DISPATCH_PACKET  = 0x6D2D14C;

// =================================================================
// GLOBALS
// =================================================================

// Карта: ID Стола -> Экземпляр кнопок (HoldemActionButtons)
std::map<int, void*> g_TableUI;
std::mutex g_tableMutex;

// Оригиналы функций для хуков
static void (*orig_Buttons_Initial)(void* instance, void* manager);
static void (*orig_Buttons_Dispose)(void* instance);
static void (*orig_SendPacket)(void*, void*, int);
static void (*orig_OnDispatchPacket)(void*, void*, int);
static int  (*call_Manager_GetTid)(void* instance); // Функция игры GetTid()

// Очередь действий
struct ActionData {
    int tableId;
    std::string actionType;
    std::string chips;
};

std::queue<ActionData> g_actionQueue;
std::mutex g_actionMutex;
std::condition_variable g_actionCv;

std::atomic<bool> g_botRunning{true};
std::thread g_botThread;

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

// PP.PPPoker.GameActionType (из твоего Enum)
int GetActionTypeValue(const std::string& typeStr) {
    if (typeStr == "ACTION_FOLD") return 1;
    if (typeStr == "ACTION_CHECK") return 2;
    if (typeStr == "ACTION_CALL") return 3;
    if (typeStr == "ACTION_RAISE") return 4;
    if (typeStr == "ACTION_BET") return 7;   // <-- ВНИМАНИЕ: 7
    if (typeStr == "ACTION_ALLIN") return 201;
    return 0;
}

// =================================================================
// 🟢 2. ЛОГИКА НАЖАТИЯ КНОПОК
// =================================================================

void PerformAction(const ActionData& act) {
    void* uiInstance = nullptr;

    // 1. Ищем кнопки для этого стола
    {
        std::lock_guard<std::mutex> lock(g_tableMutex);
        if (g_TableUI.find(act.tableId) != g_TableUI.end()) {
            uiInstance = g_TableUI[act.tableId];
        }
    }

    if (!uiInstance) {
        LOGE(">>> [BOT] Fail: Buttons not found for Table %d. (Try reopening table?)", act.tableId);
        return;
    }

    // 2. Получаем метод SendRequestAction
    // Мы знаем RVA, поэтому можем не искать его по имени, а вызвать напрямую? 
    // Нет, безопаснее через il2cpp_runtime_invoke, чтобы Unity сама настроила контекст.
    
    void* klass = il2cpp_object_get_class(uiInstance);
    if (!klass) return;

    // Ищем метод по имени, так надежнее
    void* method = il2cpp_class_get_method_from_name(klass, "SendRequestAction", 2);
    if (!method) {
        LOGE(">>> [BOT] Fail: Method SendRequestAction not found!");
        return;
    }

    // 3. Аргументы: (ActionType type, long chips)
    // ActionType - это enum (int32), chips - long (int64)
    int32_t argType = GetActionTypeValue(act.actionType);
    int64_t argChips = std::atoll(act.chips.c_str());

    void* args[2];
    args[0] = &argType;
    args[1] = &argChips;

    LOGI(">>> [BOT] 🟢 CLICKING BUTTON: Table=%d Type=%d Chips=%lld", act.tableId, argType, argChips);
    
    // 4. ВЫЗОВ!
    il2cpp_runtime_invoke(method, uiInstance, args, nullptr);
}

// =================================================================
// WORKER THREAD
// =================================================================

void BotWorkerLoop() {
    // Привязываем поток к Unity (IL2CPP)
    void* il2cppThread = nullptr;
    if (il2cpp_thread_attach && il2cpp_domain_get) {
        il2cppThread = il2cpp_thread_attach(il2cpp_domain_get());
        LOGI(">>> Bot Thread Attached to IL2CPP");
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(500, 2500); // Задержка

    while (g_botRunning) {
        ActionData act;
        {
            std::unique_lock<std::mutex> lock(g_actionMutex);
            g_actionCv.wait(lock, []{ return !g_actionQueue.empty() || !g_botRunning; });

            if (!g_botRunning) break;
            act = g_actionQueue.front();
            g_actionQueue.pop();
        }

        int delay = distrib(gen);
        LOGI(">>> [BOT] Thinking %d ms...", delay);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        PerformAction(act);
    }

    if (il2cppThread && il2cpp_thread_detach) {
        il2cpp_thread_detach(il2cppThread);
    }
}

// =================================================================
// 🟢 3. ХУКИ (СВЯЗЫВАЕМ UI И TABLE_ID)
// =================================================================

// Хук: HoldemActionButtons.Initial(HoldemManager manager)
void H_Buttons_Initial(void* instance, void* manager) {
    // Сначала вызываем оригинал, чтобы manager инициализировался
    if (orig_Buttons_Initial) orig_Buttons_Initial(instance, manager);

    if (manager && call_Manager_GetTid) {
        // Вызываем GetTid() у менеджера
        int tid = call_Manager_GetTid(manager);
        
        std::lock_guard<std::mutex> lock(g_tableMutex);
        g_TableUI[tid] = instance;
        
        LOGI(">>> [HOOK] ✅ UI CAPTURED! TableID: %d | Buttons: %p | Manager: %p", tid, instance, manager);
    } else {
        LOGW(">>> [HOOK] Buttons_Initial called but manager is null or GetTid missing");
    }
}

// Хук: HoldemActionButtons.OnDispose()
void H_Buttons_Dispose(void* instance) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    for (auto it = g_TableUI.begin(); it != g_TableUI.end(); ) {
        if (it->second == instance) {
            LOGI(">>> [HOOK] UI Disposed for Table %d", it->first);
            it = g_TableUI.erase(it);
        } else {
            ++it;
        }
    }
    if (orig_Buttons_Dispose) orig_Buttons_Dispose(instance);
}

// Сетевые хуки (только для логов)
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
// INIT
// =================================================================

void OnServerMessage(const std::string& json) {
    std::string msgType = GetJsonString(json, "message");
    if (msgType == "ActionREQ") {
        ActionData data;
        // ... парсинг ...
        // (упростил для краткости, логика та же)
        size_t pPos = json.find("\"payload\":");
        if (pPos != std::string::npos) {
            std::string p = json.substr(pPos);
            data.actionType = GetJsonString(p, "actionType");
            data.chips = GetJsonString(p, "chips");
            std::string tStr = GetJsonString(p, "tableId");
            data.tableId = tStr.empty() ? 0 : std::atoi(tStr.c_str());
            
            {
                std::lock_guard<std::mutex> lock(g_actionMutex);
                g_actionQueue.push(data);
            }
            g_actionCv.notify_one();
            LOGI(">>> [NET] Recv Action: %s for Table %d", data.actionType.c_str(), data.tableId);
        }
    }
}

void InitTrafficMonitor(uintptr_t base_addr) {
    LOGI(">>> [Init] Setting up Hooks...");

    // 1. Подготавливаем вызов GetTid
    call_Manager_GetTid = (int (*)(void*))(base_addr + RVA_Manager_GetTid);

    // 2. Хуки UI (Кнопки)
    A64HookFunction((void*)(base_addr + RVA_Buttons_Initial), (void*)H_Buttons_Initial, (void**)&orig_Buttons_Initial);
    A64HookFunction((void*)(base_addr + RVA_Buttons_OnDispose), (void*)H_Buttons_Dispose, (void**)&orig_Buttons_Dispose);

    // 3. Хуки Сети (Логирование)
    A64HookFunction((void*)(base_addr + OFFSET_SEND_PACKET), (void*)H_SendPacket, (void**)&orig_SendPacket);
    A64HookFunction((void*)(base_addr + OFFSET_DISPATCH_PACKET), (void*)H_OnDispatchPacket, (void**)&orig_OnDispatchPacket);

    // 4. Запуск бота
    g_botThread = std::thread(BotWorkerLoop);
    g_botThread.detach();

    LOGI(">>> [Init] TrafficMonitor Ready. Waiting for Table Initial...");
}