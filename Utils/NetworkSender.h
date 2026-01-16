#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>

class NetworkSender {
public:
    // Синглтон для удобного доступа из хуков
    static NetworkSender& Instance() {
        static NetworkSender instance;
        return instance;
    }

    // Запуск потока отправки
    void Start(const std::string& ip, int port);
    
    // Добавление лога в очередь
    void SendLog(const std::string& type, int tableId, const std::string& packetDump);

private:
    NetworkSender() = default;
    ~NetworkSender() = default;

    // Рабочая функция потока
    void WorkerThread();
    
    // Экранирование спецсимволов для JSON
    std::string EscapeJson(const std::string& s);

    std::string serverIp;
    int serverPort;
    
    std::atomic<bool> isRunning{false};
    std::thread worker;

    std::queue<std::string> queue;
    std::mutex queueMutex;
    std::condition_variable cv;
};