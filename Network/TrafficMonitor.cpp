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
void* g_NetInstance = nullptr; // Сохраняем экземпляр сетевого менеджера
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

// Карта типов действий (Примерная! Проверьте реальные значения Enum в игре)
int MapActionTypeToInt(const std::string& type) {
    if (type == "ACTION_FOLD") return 0; // Пример
    if (type == "ACTION_CHECK") return 1;
    if (type == "ACTION_CALL") return 2;
    if (type == "ACTION_BET") return 3;
    if (type == "ACTION_RAISE") return 4;
    if (type == "ACTION_ALLIN") return 5;
    return 0; // Default Fold
}

void PerformAction(const ActionData& act) {
    if (!g_NetInstance || !orig_SendPacket) {
        LOGE("Cannot perform action: No NetInstance or OrigFunc");
        return;
    }
    
    // 1. Ищем класс ActionREQ (Предположительное имя, проверьте в дампе!)
    // Обычно классы пакетов находятся в Assembly-CSharp
    static void* klass_ActionREQ = nullptr;
    if (!klass_ActionREQ) {
        // Попытка найти класс. Если не находит, попробуйте указать namespace
        klass_ActionREQ = GetMethodAddress("Assembly-CSharp", "", "ActionREQ", ".ctor", 0);
        if (klass_ActionREQ) {
            // GetMethodAddress возвращает адрес метода, нам нужен класс.
            // Это хак: мы знаем что GetMethodAddress находит класс внутри.
            // Лучше использовать il2cpp_class_from_name напрямую, но image нам неизвестен.
            // Поэтому сделаем перебор, если GetMethodAddress не вернет именно класс.
            // В данном коде GetMethodAddress возвращает void*, который есть адрес функции.
            // Исправим: используем глобальный поиск или сохраним image из хуков.
        }
    }
    
    // Упрощенный поиск: используем сохраненный Image из любого другого хука или поиска
    // В InitIl2CppAPI мы получаем domains.
    // ДЛЯ ПРИМЕРА: Создадим объект через Reflection, если класс не найден напрямую.
    
    // (!) ВАЖНО: Ниже код предполагает, что мы нашли класс. 
    // Если его нет, бот не сходит. 
    LOGI("Performing Action: %s, Chips: %s", act.actionType.c_str(), act.chips.c_str());
    
    // TODO: Реализовать создание объекта ActionREQ
    // void* packetObj = il2cpp_object_new(klass_ActionREQ);
    
    // ПРИМЕЧАНИЕ: Т.к. мы не знаем точного имени класса ActionREQ в игре,
    // мы можем попробовать изменить ТЕКУЩИЙ пакет, если бы мы были в SendPacket.
    // Но мы хотим создать НОВЫЙ.
    
    LOGW("Action Injection logic requires 'ActionREQ' class pointer. Implement lookup!");
}

// Вызывается из главного потока (внутри хука)
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
    // 1. Захватываем instance
    if (g_NetInstance == nullptr) {
        g_NetInstance = instance;
        LOGI(">>> NetInstance captured: %p", instance);
    }
    
    // Спуфинг (оставлен из оригинала)
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
    // 1. Также захватываем instance (на случай если SendPacket еще не вызывался)
    if (g_NetInstance == nullptr) {
        g_NetInstance = instance;
    }

    // 2. Логирование
    std::string dump = GetObjectDump(packet);
    NetworkSender::Instance().SendLog("PACKET_IN", tableId, dump);
    
    // 3. ПРОВЕРКА ОЧЕРЕДИ ДЕЙСТВИЙ (Выполняем действия бота здесь, в MainThread)
    ProcessActionQueue();

    if (orig_OnDispatchPacket) orig_OnDispatchPacket(instance, packet, tableId);
}

// HTTP Hook (оставлен без изменений)
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
    // Парсинг JSON от brain.py
    // Ожидаем: {"message": "ActionREQ", "payload": {"seatid": X, "actionType": "...", "chips": "..."}}
    
    std::string msgType = GetJsonString(json, "message");
    if (msgType == "ActionREQ") {
        ActionData data;
        // Грубый парсинг payload
        size_t payloadPos = json.find("\"payload\":");
        if (payloadPos != std::string::npos) {
            std::string payload = json.substr(payloadPos);
            data.seatId = std::atoi(GetJsonString(payload, "seatid").c_str());
            data.actionType = GetJsonString(payload, "actionType");
            data.chips = GetJsonString(payload, "chips"); // Строка
            
            std::lock_guard<std::mutex> lock(g_actionMutex);
            g_actionQueue.push(data);
            LOGI("Queued Action: %s", data.actionType.c_str());
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