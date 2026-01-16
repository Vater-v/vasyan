#include "Spoofer.h"
#include "Il2Cpp.h"           // Для il2cpp_string_new, GetMethodAddress
#include "And64InlineHook.hpp" // Для A64HookFunction
#include "Logger.h"

// =================================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ (ОРИГИНАЛЫ)
// =================================================================
void* (*orig_get_deviceModel)();
void* (*orig_get_deviceName)();
void* (*orig_get_processorType)();
int   (*orig_get_systemMemorySize)(); 
void* (*orig_get_installerName)();

// =================================================================
// ФУНКЦИИ-ПОДМЕНЫ (HOOKS)
// =================================================================
void* H_get_deviceModel() {
    return il2cpp_string_new("Samsung SM-G991B"); // S21 Ultra
}

void* H_get_deviceName() {
    return il2cpp_string_new("Galaxy S21 5G");
}

void* H_get_processorType() {
    return il2cpp_string_new("ARM64-v8a Hexa-core Processor");
}

int H_get_systemMemorySize() {
    return 8192; // 8 GB RAM
}

void* H_get_installerName() {
    return il2cpp_string_new("com.android.vending"); // Play Store
}

// =================================================================
// ИНИЦИАЛИЗАЦИЯ
// =================================================================
void InitSpoofers() {
    LOGI(">>> [Spoofer] Searching for methods...");

    // SystemInfo.deviceModel
    void* addr_Model = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_deviceModel", 0);
    if (addr_Model) A64HookFunction(addr_Model, (void*)H_get_deviceModel, (void**)&orig_get_deviceModel);

    // SystemInfo.deviceName
    void* addr_Name = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_deviceName", 0);
    if (addr_Name) A64HookFunction(addr_Name, (void*)H_get_deviceName, (void**)&orig_get_deviceName);

    // SystemInfo.processorType
    void* addr_Cpu = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_processorType", 0);
    if (addr_Cpu) A64HookFunction(addr_Cpu, (void*)H_get_processorType, (void**)&orig_get_processorType);

    // SystemInfo.systemMemorySize
    void* addr_Mem = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "SystemInfo", "get_systemMemorySize", 0);
    if (addr_Mem) A64HookFunction(addr_Mem, (void*)H_get_systemMemorySize, (void**)&orig_get_systemMemorySize);

    // Application.installerName
    void* addr_Install = GetMethodAddress("UnityEngine.CoreModule", "UnityEngine", "Application", "get_installerName", 0);
    if (addr_Install) A64HookFunction(addr_Install, (void*)H_get_installerName, (void**)&orig_get_installerName);

    LOGI(">>> [Spoofer] Initialization done.");
}