#include "TrafficMonitor.h"
#include "NetworkSender.h"
#include "And64InlineHook.hpp"
#include "Il2Cpp.h"
#include "Logger.h"
#include <queue>
#include <mutex>
#include <cstdlib>
#include <string>
#include <unistd.h> 

// =================================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// =================================================================

struct ActionData {
    int tableId;
    int actionEnum;
    int64_t chips;
};

std::queue<ActionData> g_actionQueue;
std::mutex g_actionMutex;

// Наш пульт управления (экземпляр стола)
void* g_UIInstance = nullptr;

// --- ОРИГИНАЛЫ МЕТОДОВ ---
void (*orig_SendPacket)(void* packet, int tableId, bool mask, void* method);
void (*orig_ReceviePacket)(void* packet, int tableId, void* method);
void (*orig_DebugLog)(void* message, void* method);

// Хуки для захвата инстанса (чтобы сохранить оригинал и вызвать его)
void (*orig_OnEnable)(void* instance, void* method) = nullptr;
void (*orig_Start)(void* instance, void* method) = nullptr;
void (*orig_OnCall)(void* instance, void* method) = nullptr;
void (*orig_OnCheck)(void* instance, void* method) = nullptr;

// --- МЕТОДЫ ДЛЯ ВЫЗОВА ---
void* method_SendRequestAction = nullptr;

// =================================================================
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// =================================================================

std::string GetJsonValue(const std::string& json, const std::string& key) {
    std::string qKey = "\"" + key + "\":";
    size_t pos = json.find(qKey);
    if (pos == std::string::npos) return "";
    pos += qKey.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\"')) pos++;
    size_t end = pos;
    while (end < json.length() && json[end] != '\"' && json[end] != ',' && json[end] != '}') end++;
    return json.substr(pos, end - pos);
}

int MapActionTypeToEnum(const std::string& typeStr) {
    if (typeStr == "ACTION_FOLD")  return 1;
    if (typeStr == "ACTION_CHECK") return 2;
    if (typeStr == "ACTION_CALL")  return 3;
    if (typeStr == "ACTION_RAISE") return 4;
    if (typeStr == "ACTION_BET")   return 7;
    if (typeStr == "ACTION_ALLIN") return 4; 
    if (typeStr == "ACTION_SB")    return 8;
    if (typeStr == "ACTION_BB")    return 9;
    return 0;
}

// =================================================================
// 🟢 ИСПОЛНЕНИЕ ДЕЙСТВИЯ (ЧЕРЕЗ МЕТОД ИГРЫ)
// =================================================================

void ExecuteGameAction(const ActionData& act) {
    if (!g_UIInstance) {
        // Если инстанс еще не пойман, бот не может ходить
        // Это может случиться, если бот запущен, но стол еще не открыт
        static bool warned = false;
        if (!warned) {
            LOGW(">>> [BOT-WAIT] UI Instance missing! Waiting for OnEnable or Button Click...");
            warned = true;
        }
        return;
    }

    if (!method_SendRequestAction) {
        LOGE(">>> [BOT-FAIL] SendRequestAction method not found!");
        return;
    }

    // Подготовка аргументов: SendRequestAction(int actionType, long chips)
    int actionType = act.actionEnum;
    int64_t chips = act.chips;

    void* args[2];
    args[0] = &actionType;
    args[1] = &chips;

    LOGI(">>> [BOT-EXEC] Invoking SendRequestAction(%d, %lld) on Obj %p", actionType, (long long)chips, g_UIInstance);

    // Прямой вызов метода игры. Игра сама создаст пакет и отправит его.
    // Это на 100% безопасно, так как мы используем "родной" код.
    il2cpp_runtime_invoke(method_SendRequestAction, g_UIInstance, args, nullptr);
}

// =================================================================
// 🟢 ХУКИ ЗАХВАТА ИНСТАНСА
// =================================================================

// Общая функция для захвата "this"
void CaptureInstance(void* instance) {
    if (instance && g_UIInstance != instance) {
        g_UIInstance = instance;
        LOGI(">>> [HOOK] Captured UI Instance: %p. Bot is Ready!", instance);
    }
}

void H_OnEnable(void* instance, void* method) {
    CaptureInstance(instance);
    if (orig_OnEnable) orig_OnEnable(instance, method);
}

void H_Start(void* instance, void* method) {
    CaptureInstance(instance);
    if (orig_Start) orig_Start(instance, method);
}

void H_OnCall(void* instance, void* method) {
    CaptureInstance(instance); // Если авто-захват не сработал, сработает при нажатии
    if (orig_OnCall) orig_OnCall(instance, method);
}

void H_OnCheck(void* instance, void* method) {
    CaptureInstance(instance);
    if (orig_OnCheck) orig_OnCheck(instance, method);
}

// =================================================================
// 🟢 ГЛАВНЫЙ ЦИКЛ (Debug.Log) + СНИФФЕР
// =================================================================

void H_SendPacket(void* packet, int tableId, bool mask, void* method) {
    if (NetworkSender::Instance().IsRunning()) {
        std::string dump = GetObjectDump(packet);
        NetworkSender::Instance().SendLog("PACKET_OUT", tableId, dump);
    }
    if (orig_SendPacket) orig_SendPacket(packet, tableId, mask, method);
}

void H_ReceviePacket(void* packet, int tableId, void* method) {
    if (NetworkSender::Instance().IsRunning()) {
        std::string dump = GetObjectDump(packet);
        NetworkSender::Instance().SendLog("PACKET_IN", tableId, dump);
    }
    if (orig_ReceviePacket) orig_ReceviePacket(packet, tableId, method);
}

void H_DebugLog(void* message, void* method) {
    // 1. Оригинал
    if (orig_DebugLog) orig_DebugLog(message, method);

    // 2. Проверка очереди
    bool shouldAct = false;
    ActionData act;

    // Быстрая проверка без блокировки, если очередь пуста
    if (g_actionMutex.try_lock()) {
        if (!g_actionQueue.empty()) {
            act = g_actionQueue.front();
            g_actionQueue.pop();
            shouldAct = true;
        }
        g_actionMutex.unlock();
    }

    if (shouldAct) {
        ExecuteGameAction(act);
    }
}

// =================================================================
// ИНИЦИАЛИЗАЦИЯ
// =================================================================

void InitTrafficMonitor(uintptr_t base_addr) {
    LOGI(">>> [Init] TrafficMonitor: Starting... Searching for hooks...");

    int attempts = 0;
    while (true) {
        // 1. Ищем сетевые методы (Сниффер)
        void* addr_SendPacket = GetMethodAddress(nullptr, "PP.PPPoker", "Protocol", "SendPacket", 3);
        void* addr_RecvPacket = GetMethodAddress(nullptr, "PP.PPPoker", "Protocol", "ReceviePacket", 2);
        if (!addr_RecvPacket) addr_RecvPacket = GetMethodAddress(nullptr, "PP.PPPoker", "Protocol", "ReceivePacket", 2);

        // 2. Ищем Debug.Log (Цикл исполнения)
        void* addr_DebugLog = GetMethodAddress("UnityEngine", "UnityEngine", "Debug", "Log", 1);

        // 3. Ищем методы для захвата инстанса (OnEnable, Start, Кнопки)
        void* addr_OnEnable = GetMethodAddress(nullptr, "PP.PPPoker", "HoldemActionButtons", "OnEnable", 0);
        void* addr_Start    = GetMethodAddress(nullptr, "PP.PPPoker", "HoldemActionButtons", "Start", 0);
        void* addr_OnCall   = GetMethodAddress(nullptr, "PP.PPPoker", "HoldemActionButtons", "OnCallButtonClick", 0);
        void* addr_OnCheck  = GetMethodAddress(nullptr, "PP.PPPoker", "HoldemActionButtons", "OnCheckButtonClick", 0);

        // 4. Ищем метод ДЕЙСТВИЯ (SendRequestAction)
        if (!method_SendRequestAction) {
            size_t size = 0;
            void** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &size);
            void* klass_Buttons = nullptr;
            for (size_t i = 0; i < size; ++i) {
                void* image = il2cpp_assembly_get_image(assemblies[i]);
                klass_Buttons = il2cpp_class_from_name(image, "PP.PPPoker", "HoldemActionButtons");
                if (klass_Buttons) break;
            }
            if (klass_Buttons) {
                // Пытаемся найти с 2 аргументами (ActionType, chips)
                method_SendRequestAction = il2cpp_class_get_method_from_name(klass_Buttons, "SendRequestAction", 2);
                if (method_SendRequestAction) LOGI(">>> [Reflect] Found SendRequestAction!");
            }
        }

        // ПРОВЕРКА: Нам нужны Сеть, Лог и Метод Действия. 
        // Хуки захвата (OnEnable/Start) опциональны (хотя бы один должен быть).
        bool captureMethodFound = (addr_OnEnable || addr_Start || addr_OnCall);

        if (addr_SendPacket && addr_RecvPacket && addr_DebugLog && method_SendRequestAction && captureMethodFound) {
            LOGI(">>> [Init] Components found. Installing Hooks...");

            // Сеть
            A64HookFunction(addr_SendPacket, (void*)H_SendPacket, (void**)&orig_SendPacket);
            A64HookFunction(addr_RecvPacket, (void*)H_ReceviePacket, (void**)&orig_ReceviePacket);
            
            // Цикл
            A64HookFunction(addr_DebugLog, (void*)H_DebugLog, (void**)&orig_DebugLog);

            // Захват инстанса (ставим хуки на всё, что нашли, для надежности)
            if (addr_OnEnable) A64HookFunction(addr_OnEnable, (void*)H_OnEnable, (void**)&orig_OnEnable);
            if (addr_Start)    A64HookFunction(addr_Start,    (void*)H_Start,    (void**)&orig_Start);
            if (addr_OnCall)   A64HookFunction(addr_OnCall,   (void*)H_OnCall,   (void**)&orig_OnCall);
            if (addr_OnCheck)  A64HookFunction(addr_OnCheck,  (void*)H_OnCheck,  (void**)&orig_OnCheck);

            LOGI(">>> [Init] ALL SYSTEMS GO. Ready for Action.");
            break;
        }

        attempts++;
        if (attempts % 5 == 0) {
            LOGW(">>> [Init] Waiting... (Attempt %d)", attempts);
            if (!addr_SendPacket) LOGW("    - Missing: Protocol.SendPacket");
            if (!addr_DebugLog) LOGW("    - Missing: Debug.Log");
            if (!method_SendRequestAction) LOGW("    - Missing: SendRequestAction (Check Class/Method Name)");
            if (!captureMethodFound) LOGW("    - Missing: Any Capture Method (OnEnable/Start/OnCall)");
        }
        
        sleep(1);
    }
}

// Callback от сервера
void OnServerMessage(const std::string& json) {
    std::string msgType = GetJsonValue(json, "message");
    if (msgType == "ActionREQ") {
        size_t pPos = json.find("\"payload\":");
        if (pPos != std::string::npos) {
            std::string p = json.substr(pPos);
            ActionData data;
            
            std::string typeStr = GetJsonValue(p, "actionType");
            data.actionEnum = MapActionTypeToEnum(typeStr);
            
            std::string chipsStr = GetJsonValue(p, "chips");
            data.chips = chipsStr.empty() ? 0 : std::atoll(chipsStr.c_str());
            
            std::string tStr = GetJsonValue(p, "tableId");
            data.tableId = tStr.empty() ? -1 : std::atoi(tStr.c_str());

            if (data.actionEnum != 0) {
                LOGI(">>> [NET-CMD] Recv Action: %s. Queued.", typeStr.c_str());
                std::lock_guard<std::mutex> lock(g_actionMutex);
                g_actionQueue.push(data);
            }
        }
    }
}