#pragma once
#include <cstdint>
#include <string>

// Функция для установки всех сетевых хуков (Game Packet + HTTP)
void InitTrafficMonitor(uintptr_t base_addr);

// Обработка сообщений от сервера (вызывается из NetworkSender thread)
void OnServerMessage(const std::string& json);