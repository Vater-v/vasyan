#pragma once
#include <cstdint>
#include <string>

// Функция для установки всех сетевых хуков
void InitTrafficMonitor(uintptr_t base_addr);

// Обработка сообщений от сервера
void OnServerMessage(const std::string& json);