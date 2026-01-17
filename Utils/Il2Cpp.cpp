#include "Il2Cpp.h"
#include "Logger.h"
#include <dlfcn.h>
#include <codecvt>
#include <locale>
#include <cstring>

// Инициализация переменных
void* (*il2cpp_domain_get)() = nullptr;
void** (*il2cpp_domain_get_assemblies)(void* domain, size_t* size) = nullptr;
void* (*il2cpp_assembly_get_image)(void* assembly) = nullptr;
const char* (*il2cpp_image_get_name)(void* image) = nullptr;
void* (*il2cpp_class_from_name)(void* image, const char* namespaze, const char* name) = nullptr;
void* (*il2cpp_class_get_method_from_name)(void* klass, const char* name, int argsCount) = nullptr;
void* (*il2cpp_object_get_class)(void* obj) = nullptr;
const char* (*il2cpp_class_get_name)(void* klass) = nullptr;
void* (*il2cpp_class_get_field_from_name)(void* klass, const char* name) = nullptr;
void  (*il2cpp_field_set_value)(void* obj, void* field, void* value) = nullptr;
void* (*il2cpp_string_new)(const char* str) = nullptr;
void* (*il2cpp_runtime_invoke)(void* method, void* obj, void** params, void** exc) = nullptr;
void* (*il2cpp_object_new)(void* klass) = nullptr;

// --- THREADING ---
void* (*il2cpp_thread_attach)(void* domain) = nullptr;
void  (*il2cpp_thread_detach)(void* thread) = nullptr;

bool InitIl2CppAPI(void* handle) {
    if (!handle) return false;
    
    il2cpp_domain_get = (void* (*)())dlsym(handle, "il2cpp_domain_get");
    il2cpp_domain_get_assemblies = (void** (*)(void*, size_t*))dlsym(handle, "il2cpp_domain_get_assemblies");
    il2cpp_assembly_get_image = (void* (*)(void*))dlsym(handle, "il2cpp_assembly_get_image");
    il2cpp_image_get_name = (const char* (*)(void*))dlsym(handle, "il2cpp_image_get_name");
    il2cpp_class_from_name = (void* (*)(void*, const char*, const char*))dlsym(handle, "il2cpp_class_from_name");
    
    il2cpp_object_get_class = (void* (*)(void*))dlsym(handle, "il2cpp_object_get_class");
    il2cpp_class_get_name = (const char* (*)(void*))dlsym(handle, "il2cpp_class_get_name");
    il2cpp_class_get_method_from_name = (void* (*)(void*, const char*, int))dlsym(handle, "il2cpp_class_get_method_from_name");
    il2cpp_runtime_invoke = (void* (*)(void*, void*, void**, void**))dlsym(handle, "il2cpp_runtime_invoke");
    il2cpp_class_get_field_from_name = (void* (*)(void*, const char*))dlsym(handle, "il2cpp_class_get_field_from_name");
    il2cpp_field_set_value = (void (*)(void*, void*, void*))dlsym(handle, "il2cpp_field_set_value");
    il2cpp_string_new = (void* (*)(const char*))dlsym(handle, "il2cpp_string_new");
    il2cpp_object_new = (void* (*)(void*))dlsym(handle, "il2cpp_object_new");
    
    // --- THREADING ---
    il2cpp_thread_attach = (void* (*)(void*))dlsym(handle, "il2cpp_thread_attach");
    il2cpp_thread_detach = (void (*)(void*))dlsym(handle, "il2cpp_thread_detach");
    
    return il2cpp_domain_get && il2cpp_class_from_name;
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

void* GetMethodAddress(const char* targetAssembly, const char* nameSpace, const char* className, const char* methodName, int argsCount) {
    if (!il2cpp_domain_get) return nullptr;

    void* domain = il2cpp_domain_get();
    size_t size = 0;
    void** assemblies = il2cpp_domain_get_assemblies(domain, &size);

    for (size_t i = 0; i < size; ++i) {
        void* image = il2cpp_assembly_get_image(assemblies[i]);
        const char* imgName = il2cpp_image_get_name ? il2cpp_image_get_name(image) : "unknown";
        
        if (targetAssembly && strstr(imgName, targetAssembly) == nullptr) {
            continue; 
        }

        void* klass = il2cpp_class_from_name(image, nameSpace, className);
        if (klass) {
            void* method = il2cpp_class_get_method_from_name(klass, methodName, argsCount);
            if (method) {
                LOGI("[+] Found Method: %s @ %p", methodName, *(void**)method);
                return *(void**)method; 
            }
        }
    }
    return nullptr;
}