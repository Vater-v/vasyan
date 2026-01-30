#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>
#include <functional>

using RecvCallback = std::function<void(const std::string&)>;

class NetworkSender {
public:
    static NetworkSender& Instance() {
        static NetworkSender instance;
        return instance;
    }

    void Start(const std::string& ip, int port);
    void Stop(); // Метод для полной остановки (если нужно)
    void SendLog(const std::string& type, int tableId, const std::string& packetDump);
    
    void SetCallback(RecvCallback cb) {
        recvCallback = cb;
    }

    bool IsRunning() const { 
        return isRunning; 
    }

    // Проверка, подключены ли мы прямо сейчас (для внешних проверок)
    bool IsConnected() const {
        return isConnected;
    }

private:
    NetworkSender() = default;
    ~NetworkSender() = default;

    void WorkerThread();
    void ReceiveLoop(int sock);
    
    std::string EscapeJson(const std::string& s);

    std::string serverIp;
    int serverPort;
    
    // Флаги состояния
    std::atomic<bool> isRunning{false};      // Глобальный флаг работы сервиса
    std::atomic<bool> isConnected{false};    // Флаг текущего подключения
    std::atomic<bool> connectionBroken{false}; // Сигнал о разрыве (для пробуждения потока)

    std::thread worker;

    std::queue<std::string> queue;
    std::mutex queueMutex;
    std::condition_variable cv;
    
    RecvCallback recvCallback = nullptr;

    // Счетчики трафика
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<uint64_t> bytesReceived{0};
};