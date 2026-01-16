#include "Utils.h"

uintptr_t get_lib_addr(const char* lib_name) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[1024];
    uintptr_t addr = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, lib_name)) {
            addr = strtoul(line, nullptr, 16);
            break;
        }
    }
    fclose(fp);
    return addr;
}