#include "Il2Cpp.h"
#include <dlfcn.h>
#include <codecvt>
#include <locale>

// Определение переменных (выделение памяти под указатели)
void* (*il2cpp_object_get_class)(void* obj) = nullptr;
const char* (*il2cpp_class_get_name)(void* klass) = nullptr;
void* (*il2cpp_class_get_method_from_name)(void* klass, const char* name, int argsCount) = nullptr;
void* (*il2cpp_runtime_invoke)(void* method, void* obj, void** params, void** exc) = nullptr;

bool InitIl2CppAPI(void* handle) {
    if (!handle) return false;
    il2cpp_object_get_class = (void* (*)(void*))dlsym(handle, "il2cpp_object_get_class");
    il2cpp_class_get_name = (const char* (*)(void*))dlsym(handle, "il2cpp_class_get_name");
    il2cpp_class_get_method_from_name = (void* (*)(void*, const char*, int))dlsym(handle, "il2cpp_class_get_method_from_name");
    il2cpp_runtime_invoke = (void* (*)(void*, void*, void**, void**))dlsym(handle, "il2cpp_runtime_invoke");
    return true;
}

std::string Utf16ToUtf8(Il2CppString* str) {
    if (!str || str->length <= 0) return "";
    std::u16string u16_str(reinterpret_cast<char16_t*>(str->chars), str->length);
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
    return convert.to_bytes(u16_str);
}

std::string GetObjectDump(void* obj) {
    if (!obj || !il2cpp_object_get_class) return "null";

    void* klass = il2cpp_object_get_class(obj);
    if (!klass) return "UnknownClass";
    const char* cName = il2cpp_class_get_name(klass);
    std::string name = cName ? cName : "NoName";

    std::string content = "{}";
    void* method = il2cpp_class_get_method_from_name(klass, "ToString", 0);
    if (method) {
        Il2CppString* res = (Il2CppString*)il2cpp_runtime_invoke(method, obj, nullptr, nullptr);
        if (res) content = Utf16ToUtf8(res);
    }
    return name + " " + content;
}