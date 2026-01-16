#include <jni.h>
#include <pthread.h>
#include <dlfcn.h>
#include "And64InlineHook.hpp"

// Подключаем наши новые модули
#include "Logger.h"
#include "Utils.h"
#include "Il2Cpp.h"

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
    // Теперь используем удобную функцию из Il2Cpp.h
    LOGW(">>> [OUT] (T:%d) %s", tableId, GetObjectDump(packet).c_str());
    if (orig_SendPacket) orig_SendPacket(instance, packet, tableId);
}

// Входящие
void H_OnDispatchPacket(void* instance, void* packet, int tableId) {
    LOGI("<<< [IN]  (T:%d) %s", tableId, GetObjectDump(packet).c_str());
    if (orig_OnDispatchPacket) orig_OnDispatchPacket(instance, packet, tableId);
}

// =============================================================
// MAIN THREAD
// =============================================================

void* hack_thread(void*) {
    // 1. Ждем библиотеку
    uintptr_t base = 0;
    while (!(base = get_lib_addr("libil2cpp.so"))) usleep(100000);
    
    // 2. Инициализируем Il2Cpp API
    void* handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (InitIl2CppAPI(handle)) {
        LOGI("Il2Cpp API Loaded");
    } else {
        LOGE("Failed to load Il2Cpp API");
    }

    // 3. Ставим хуки
    A64HookFunction((void*)(base + OFFSET_SEND_PACKET), (void*)H_SendPacket, (void**)&orig_SendPacket);
    A64HookFunction((void*)(base + OFFSET_DISPATCH_PACKET), (void*)H_OnDispatchPacket, (void**)&orig_OnDispatchPacket);

    LOGI("=== SIMPLE SNIFFER STARTED ===");
    return nullptr;
}

void __attribute__((constructor)) init() {
    pthread_t t; pthread_create(&t, nullptr, hack_thread, nullptr);
}