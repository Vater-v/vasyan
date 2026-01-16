#pragma once
#include <cstdint>
#include <string>

// Структуры
struct Il2CppString {
    void* klass;
    void* monitor;
    int32_t length;
    uint16_t chars[1];
};

// Объявляем указатели как extern, чтобы они были видны везде, но определены в cpp
extern void* (*il2cpp_object_get_class)(void* obj);
extern const char* (*il2cpp_class_get_name)(void* klass);
extern void* (*il2cpp_class_get_method_from_name)(void* klass, const char* name, int argsCount);
extern void* (*il2cpp_runtime_invoke)(void* method, void* obj, void** params, void** exc);
extern void* (*il2cpp_class_get_field_from_name)(void* klass, const char* name);
extern void  (*il2cpp_field_set_value)(void* obj, void* field, void* value);
extern void* (*il2cpp_string_new)(const char* str);

// Функции-помощники
bool InitIl2CppAPI(void* handle); // Функция инициализации
std::string Utf16ToUtf8(Il2CppString* str);
std::string GetObjectDump(void* obj); // Твой GetPacketDump, но более универсальный