#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Структуры
struct Il2CppString {
    void* klass;
    void* monitor;
    int32_t length;
    uint16_t chars[1];
};

// Объявляем указатели API
extern void* (*il2cpp_domain_get)();
extern void** (*il2cpp_domain_get_assemblies)(void* domain, size_t* size);
extern void* (*il2cpp_assembly_get_image)(void* assembly);
extern const char* (*il2cpp_image_get_name)(void* image); // <--- НОВОЕ
extern void* (*il2cpp_class_from_name)(void* image, const char* namespaze, const char* name);
extern void* (*il2cpp_class_get_method_from_name)(void* klass, const char* name, int argsCount);
extern void* (*il2cpp_object_get_class)(void* obj);
extern const char* (*il2cpp_class_get_name)(void* klass);
extern void* (*il2cpp_class_get_field_from_name)(void* klass, const char* name);
extern void  (*il2cpp_field_set_value)(void* obj, void* field, void* value);
extern void* (*il2cpp_string_new)(const char* str);
extern void* (*il2cpp_runtime_invoke)(void* method, void* obj, void** params, void** exc);

// Функции-помощники
bool InitIl2CppAPI(void* handle);
std::string Utf16ToUtf8(Il2CppString* str);
std::string GetObjectDump(void* obj);

// Функция поиска
void* GetMethodAddress(const char* targetAssembly, const char* nameSpace, const char* className, const char* methodName, int argsCount);