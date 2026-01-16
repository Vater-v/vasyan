#include <jni.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h> // usleep
#include "And64InlineHook.hpp"

// Подключаем наши модули
#include "Logger.h"
#include "Utils.h"
#include "Il2Cpp.h"
#include "NetworkSender.h" // <--- Подключили

// =============================================================
// CONFIG (Оффсеты игры)
// =============================================================
const uintptr_t OFFSET_SEND_PACKET      = 0x6D2BC60; 
const uintptr_t OFFSET_DISPATCH_PACKET  = 0x6D2D14C; 

// =============================================================
// HOOKS
// =============================================================

void (*orig_SendPacket)(void* instance, void* packet, int tableId);
void (*orig_OnDispatchPacket)(void* instance, void* packet, int tableId);

// Исходящие
void H_SendPacket(void* instance, void* packet, int tableId) {
    // Получаем дамп ПЕРЕД отправкой в очередь, т.к. в другом потоке packet может быть уже удален
    std::string dump = GetObjectDump(packet);
    
    // Логируем в Logcat для отладки
    // LOGW(">>> [OUT] (T:%d) %s", tableId, dump.c_str());

    // Отправляем на сервер
    NetworkSender::Instance().SendLog("OUT", tableId, dump);

    if (orig_SendPacket) orig_SendPacket(instance, packet, tableId);
}

// Входящие
void H_OnDispatchPacket(void* instance, void* packet, int tableId) {
    std::string dump = GetObjectDump(packet);
    
    // LOGI("<<< [IN]  (T:%d) %s", tableId, dump.c_str());
    
    NetworkSender::Instance().SendLog("IN", tableId, dump);

    if (orig_OnDispatchPacket) orig_OnDispatchPacket(instance, packet, tableId);
}

// =============================================================
// MAIN THREAD
// =============================================================

void* hack_thread(void*) {
    // 1. Запускаем сетевой клиент
    // Адрес сервера: 192.168.0.132, порт 5006
    NetworkSender::Instance().Start("192.168.0.132", 5006);

    // 2. Ждем библиотеку игры
    uintptr_t base = 0;
    while (!(base = get_lib_addr("libil2cpp.so"))) usleep(100000);
    
    // 3. Инициализируем Il2Cpp API
    void* handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (InitIl2CppAPI(handle)) {
        LOGI("Il2Cpp API Loaded");
    } else {
        LOGE("Failed to load Il2Cpp API");
    }

    // 4. Ставим хуки
    A64HookFunction((void*)(base + OFFSET_SEND_PACKET), (void*)H_SendPacket, (void**)&orig_SendPacket);
    A64HookFunction((void*)(base + OFFSET_DISPATCH_PACKET), (void*)H_OnDispatchPacket, (void**)&orig_OnDispatchPacket);

    LOGI("=== NETWORK SNIFFER STARTED ===");
    return nullptr;
}

void __attribute__((constructor)) init() {
    pthread_t t; pthread_create(&t, nullptr, hack_thread, nullptr);
}