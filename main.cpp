#include <jni.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include "And64InlineHook.hpp"

#include "Logger.h"
#include "Utils.h"
#include "Il2Cpp.h"
#include "NetworkSender.h"

// ГЛОБАЛЬНЫЕ ОРИГИНАЛЫ
void* (*orig_get_deviceModel)();
void* (*orig_get_deviceName)();
void* (*orig_get_operatingSystem)();

// --- ХУКИ (ПОДМЕНА ДАННЫХ) ---

// 1. Модель устройства
void* H_get_deviceModel() {
    // Возвращаем реальный флагман, чтобы игра думала, что мы на телефоне
    // LOGW("SPOOF: SystemInfo.deviceModel requested -> Samsung SM-G991B");
    return il2cpp_string_new("Samsung SM-G991B"); 
}

// 2. Имя устройства
void* H_get_deviceName() {
    // LOGW("SPOOF: SystemInfo.deviceName requested -> Galaxy S21");
    return il2cpp_string_new("Galaxy S21 5G");
}

// 3. ОС (иногда палят по строке "nox" или "vbox" в версии ядра)
void* H_get_operatingSystem() {
    // LOGW("SPOOF: SystemInfo.operatingSystem requested -> Android 12");
    return il2cpp_string_new("Android OS 12 / API-31 (SP1A.210812.016/G991BXXU5CVH7)");
}

// --- ПОТОК ---

void* hack_thread(void*) {
    signal(SIGPIPE, SIG_IGN); 
    LOGI(">>> Hmuriy SystemInfo Spoofer Started <<<");

    // Ждем библиотеку
    uintptr_t base = 0;
    while (!(base = get_lib_addr("libil2cpp.so"))) {
        usleep(100000);
    }
    sleep(2); // Даем прогрузиться

    void* handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (!InitIl2CppAPI(handle)) {
        LOGE("Failed to load Il2Cpp API!");
        return nullptr;
    }
    LOGI("Il2Cpp API Loaded. Searching for SystemInfo...");

    // Цикл поиска (Retry Loop)
    bool hooked = false;
    int attempts = 0;

    while (!hooked) {
        attempts++;
        if (attempts % 5 == 0) LOGI("Scanning attempt %d...", attempts);

        // Ищем класс SystemInfo в сборке UnityEngine.CoreModule
        // Имя сборки может быть без расширения или с ним, проверяем по частичному совпадению в GetMethodAddress
        void* addr_Model = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_deviceModel", 0);
        
        if (addr_Model) {
            LOGW(">>> FOUND SystemInfo! Applying Hooks... <<<");
            
            // Хукаем Model
            A64HookFunction(addr_Model, (void*)H_get_deviceModel, (void**)&orig_get_deviceModel);
            
            // Хукаем Name
            void* addr_Name = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_deviceName", 0);
            if (addr_Name) A64HookFunction(addr_Name, (void*)H_get_deviceName, (void**)&orig_get_deviceName);

            // Хукаем OS
            void* addr_OS = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_operatingSystem", 0);
            if (addr_OS) A64HookFunction(addr_OS, (void*)H_get_operatingSystem, (void**)&orig_get_operatingSystem);

            hooked = true;
            LOGW(">>> SystemInfo Spoofed Successfully! <<<");
        }

        if (hooked) break;
        sleep(1);
    }

    return nullptr;
}

void __attribute__((constructor)) init() {
    pthread_t t; pthread_create(&t, nullptr, hack_thread, nullptr);
}