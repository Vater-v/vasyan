#include "NetworkSender.h"
#include "Logger.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>

void NetworkSender::Start(const std::string& ip, int port) {
    if (isRunning) return;
    serverIp = ip;
    serverPort = port;
    isRunning = true;
    worker = std::thread(&NetworkSender::WorkerThread, this);
}

void NetworkSender::SendLog(const std::string& type, int tableId, const std::string& packetDump) {
    if (!isRunning) return;

    // Формируем JSON вручную, чтобы не тянуть тяжелые библиотеки
    // Формат: {"type": "IN", "tableId": 123, "dump": "..."}
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
                if ('\x00' <= c && c <= '\x1f') {
                    // Игнорируем управляющие символы или можно кодировать в \uXXXX
                    continue; 
                }
                output += c;
        }
    }
    return output;
}

void NetworkSender::WorkerThread() {
    while (isRunning) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            LOGE("Socket creation failed");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(serverPort);
        
        if (inet_pton(AF_INET, serverIp.c_str(), &serv_addr.sin_addr) <= 0) {
            LOGE("Invalid address");
            close(sock);
            return; // Fatal error
        }

        LOGI("Connecting to %s:%d...", serverIp.c_str(), serverPort);
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            LOGE("Connection failed. Retrying in 1s...");
            close(sock);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        LOGI("Connected to server!");

        while (isRunning) {
            std::string payload;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                cv.wait(lock, [this]{ return !queue.empty() || !isRunning; });

                if (!isRunning) break;

                payload = queue.front();
                queue.pop();
            }

            // Добавляем перенос строки для JSONL
            payload += "\n";

            // Отправляем
            ssize_t sentBytes = send(sock, payload.c_str(), payload.size(), 0);
            if (sentBytes < 0) {
                LOGE("Send failed. Reconnecting...");
                break; // Выход во внешний цикл для переподключения
            }
        }

        close(sock);
    }
}