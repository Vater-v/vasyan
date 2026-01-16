#include "NetworkSender.h"
#include "Logger.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <cerrno>
#include <cstring>
#include <exception>

void NetworkSender::Start(const std::string& ip, int port) {
    if (isRunning) return;
    serverIp = ip;
    serverPort = port;
    isRunning = true;
    worker = std::thread(&NetworkSender::WorkerThread, this);
}

void NetworkSender::SendLog(const std::string& type, int tableId, const std::string& packetDump) {
    if (!isRunning) return;

    std::stringstream ss;
    ss << "{\"type\":\"" << type << "\",";
    ss << "\"tableId\":" << tableId << ",";
    ss << "\"dump\":\"" << EscapeJson(packetDump) << "\"}";
    
    std::string json = ss.str();

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push(json);
    }
    cv.notify_one();
}

std::string NetworkSender::EscapeJson(const std::string& s) {
    std::string output;
    output.reserve(s.length());
    for (char c : s) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f') continue;
                output += c;
        }
    }
    return output;
}

void NetworkSender::WorkerThread() {
    LOGI("WorkerThread started");

    while (isRunning) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            LOGE("Socket failed: %s", strerror(errno));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(serverPort);
        inet_pton(AF_INET, serverIp.c_str(), &serv_addr.sin_addr);

        LOGI("Connecting to %s:%d...", serverIp.c_str(), serverPort);
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            LOGE("Connect failed: %s. Retry in 1s...", strerror(errno));
            close(sock);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        LOGI("Connected! Loop starting...");

        try {
            while (isRunning) {
                std::string payload;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    cv.wait(lock, [this]{ return !queue.empty() || !isRunning; });

                    if (!isRunning) break;
                    if (queue.empty()) {
                        LOGW("Wakeup with empty queue! Spurious wakeup?");
                        continue;
                    }

                    payload = queue.front();
                    queue.pop();
                }

                payload += "\n";
                ssize_t sentBytes = send(sock, payload.c_str(), payload.size(), MSG_NOSIGNAL);
                if (sentBytes < 0) {
                    LOGE("Send error: %s", strerror(errno));
                    break; 
                }
            }
        } catch (const std::exception& e) {
            LOGE("CRASH AVOIDED in WorkerThread: %s", e.what());
        } catch (...) {
            LOGE("CRASH AVOIDED in WorkerThread: Unknown exception");
        }

        LOGW("Closing socket and reconnecting...");
        close(sock);
    }
}