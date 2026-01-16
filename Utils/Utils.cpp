#include "Utils.h"
#include <link.h>
#include <string.h>
#include <stddef.h>

// Структура для передачи параметров в callback-функцию
struct CallbackData {
    const char* lib_name;
    uintptr_t addr;
};

// Функция обратного вызова, которую вызывает система для каждой загруженной библиотеки
static int find_lib_callback(struct dl_phdr_info* info, size_t size, void* data) {
    CallbackData* cb_data = (CallbackData*)data;

    // info->dlpi_name содержит полный путь к библиотеке (или имя).
    // Мы ищем вхождение искомого имени (например, "libil2cpp.so") в этом пути.
    if (info->dlpi_name && strstr(info->dlpi_name, cb_data->lib_name)) {
        cb_data->addr = (uintptr_t)info->dlpi_addr;
        return 1; // Возвращаем 1, чтобы прервать перебор (мы нашли то, что искали)
    }

    return 0; // Продолжаем поиск
}

uintptr_t get_lib_addr(const char* lib_name) {
    CallbackData data;
    data.lib_name = lib_name;
    data.addr = 0;

    // Запускаем системный итератор по заголовкам программ (Program Headers)
    dl_iterate_phdr(find_lib_callback, &data);

    return data.addr;
}