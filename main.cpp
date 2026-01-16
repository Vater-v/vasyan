#include <jni.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include "And64InlineHook.hpp"

// Подключаем наши модули
#include "Logger.h"
#include "Utils.h"
#include "Il2Cpp.h"
#include "NetworkSender.h"

// Определяем RTLD_NOLOAD, если его нет (для Android)
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 4
#endif

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
    if (packet && il2cpp_object_get_class) {
        void* klass = il2cpp_object_get_class(packet);
        if (klass) {
            void* field = il2cpp_class_get_field_from_name(klass, "system");
            if (field) {
                void* androidString = il2cpp_string_new("android");
                il2cpp_field_set_value(packet, field, androidString);
                // const char* name = il2cpp_class_get_name(klass);
                // LOGW("PATCHED: system -> android inside %s", name ? name : "Unknown");
            }
        }
    }
    std::string dump = GetObjectDump(packet);
    NetworkSender::Instance().SendLog("OUT", tableId, dump);
    if (orig_SendPacket) orig_SendPacket(instance, packet, tableId);
}

// Входящие
void H_OnDispatchPacket(void* instance, void* packet, int tableId) {
    std::string dump = GetObjectDump(packet);
    NetworkSender::Instance().SendLog("IN", tableId, dump);
    if (orig_OnDispatchPacket) orig_OnDispatchPacket(instance, packet, tableId);
}

// =============================================================
// MAIN THREAD
// =============================================================

void* hack_thread(void*) {
    // Игнорируем вылеты сети
    signal(SIGPIPE, SIG_IGN); 

    LOGI(">>> Hack Thread Started <<<");

    // 1. Запуск сети
    LOGI("Starting NetworkSender...");
    NetworkSender::Instance().Start("192.168.0.132", 5006);

    // 2. Ожидание библиотеки
    LOGI("Waiting for libil2cpp.so...");
    uintptr_t base = 0;
    int wait_attempts = 0;
    
    // Ждем, пока библиотека появится в памяти
    while (!(base = get_lib_addr("libil2cpp.so"))) {
        usleep(100000); // 0.1 сек
        wait_attempts++;
        if (wait_attempts % 50 == 0) LOGI("Still waiting... (%d)", wait_attempts);
    }
    
    LOGI("Found libil2cpp at: %p", (void*)base);

    // === ВАЖНОЕ ИСПРАВЛЕНИЕ ===
    // Библиотека только что появилась. Дадим системе время завершить её инициализацию (JNI_OnLoad и т.д.)
    // Иначе dlopen может вызвать краш из-за гонки потоков.
    LOGI("Sleeping 3s to let libil2cpp initialize...");
    sleep(3); 
    // ===========================

    LOGI("Attempting to get handle via dlopen...");
    
    // Сначала пробуем RTLD_NOLOAD (получить хендл без повторной загрузки)
    void* handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    
    if (!handle) {
        LOGW("dlopen(NOLOAD) failed. Trying regular dlopen...");
        handle = dlopen("libil2cpp.so", RTLD_NOW);
    }

    if (handle) {
        LOGI("dlopen success! Handle: %p", handle);
        
        if (InitIl2CppAPI(handle)) {
            LOGI("Il2Cpp API Loaded successfully");
        } else {
            LOGE("Failed to load Il2Cpp API (symbols not found?)");
        }
    } else {
        LOGE("dlopen FAILED completely. API calls won't work!");
        // Здесь мы не выходим, так как хуки всё равно можно поставить по оффсетам,
        // но функции типа GetObjectDump работать не будут.
    }

    // 4. Ставим хуки
    LOGI("Applying hooks at base: %p", (void*)base);
    A64HookFunction((void*)(base + OFFSET_SEND_PACKET), (void*)H_SendPacket, (void**)&orig_SendPacket);
    A64HookFunction((void*)(base + OFFSET_DISPATCH_PACKET), (void*)H_OnDispatchPacket, (void**)&orig_OnDispatchPacket);

    LOGI("=== NETWORK SNIFFER FULLY READY ===");
    return nullptr;
}

void __attribute__((constructor)) init() {
    pthread_t t; pthread_create(&t, nullptr, hack_thread, nullptr);
}