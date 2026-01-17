#pragma once
#include <cstdint>
#include <string>

// Инициализация хуков и указателей
void InitTrafficMonitor(uintptr_t base_addr);

// Обработчик сообщений от сервера
void OnServerMessage(const std::string& json);