#pragma once
#include <cstdint>

// Функция для установки всех сетевых хуков (Game Packet + HTTP)
void InitTrafficMonitor(uintptr_t base_addr);