#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>

class NetworkSender {
public:
    static NetworkSender& Instance() {
        static NetworkSender instance;
        return instance;
    }

    void Start(const std::string& ip, int port);
    void SendLog(const std::string& type, int tableId, const std::string& packetDump);

private:
    NetworkSender() = default;
    ~NetworkSender() = default;

    void WorkerThread();
    std::string EscapeJson(const std::string& s);

    std::string serverIp;
    int serverPort;
    
    std::atomic<bool> isRunning{false};
    std::thread worker;

    std::queue<std::string> queue;
    std::mutex queueMutex;
    std::condition_variable cv;
};