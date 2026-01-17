#include <jni.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>

#include "Logger.h"
#include "Utils.h"
#include "Il2Cpp.h"
#include "Spoofer.h"
#include "NetworkSender.h"
#include "TrafficMonitor.h"

// =================================================================
// MAIN THREAD
// =================================================================
void* hack_thread(void*) {
    // 1. ЗАПУСК СЕТЕВОГО КЛИЕНТА
    LOGI(">>> Starting Network Sender...");
    
    // Подключаем callback приема сообщений
    NetworkSender::Instance().SetCallback(OnServerMessage);
    
    // IP и порт
    NetworkSender::Instance().Start("192.168.0.132", 5006); 

    // 2. ОЖИДАНИЕ БИБЛИОТЕКИ ИГРЫ
    LOGI(">>> Waiting for libil2cpp.so...");
    uintptr_t base_addr = 0;
    while ((base_addr = get_lib_addr("libil2cpp.so")) == 0) {
        usleep(100000);
    }
    sleep(3); 

    // 3. INIT API
    LOGI(">>> libil2cpp loaded. Init API...");
    void* handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (!InitIl2CppAPI(handle)) {
        LOGE("!!! Failed to load IL2CPP API !!!");
        return nullptr;
    }

    // 4. УСТАНОВКА ХУКОВ
    InitSpoofers();
    InitTrafficMonitor(base_addr);

    LOGI("=== ULTIMATE SNIFFER & BOT READY ===");
    return nullptr;
}

void __attribute__((constructor)) init() {
    pthread_t t; 
    pthread_create(&t, nullptr, hack_thread, nullptr);
}