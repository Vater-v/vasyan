#include "NetworkSender.h"
#include "Logger.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <cerrno>
#include <cstring>
#include <exception>
#include <vector>

void NetworkSender::Start(const std::string& ip, int port) {
    if (isRunning) return;
    serverIp = ip;
    serverPort = port;
    isRunning = true;
    worker = std::thread(&NetworkSender::WorkerThread, this);
}

void NetworkSender::Stop() {
    isRunning = false;
    connectionBroken = true; // Будим поток
    cv.notify_all();
    if (worker.joinable()) worker.join();
}

void NetworkSender::SendLog(const std::string& type, int tableId, const std::string& packetDump) {
    if (!isRunning) return;

    // Если нет соединения, можно либо дропать логи, либо копить.
    // Пока копим, но очередь может раздуться.
    if (queue.size() > 1000) return; // Защита от переполнения

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

// Поток чтения (Слушает сервер)
void NetworkSender::ReceiveLoop(int sock) {
    LOGI(">>> [NET] ReceiveLoop started on socket %d", sock);
    char buffer[4096];
    std::string pendingData;

    while (isRunning && !connectionBroken) {
        memset(buffer, 0, sizeof(buffer));
        // Читаем данные
        ssize_t bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0);

        if (bytesRead > 0) {
            bytesReceived += bytesRead; // Считаем трафик
            // LOGI(">>> [NET] IN: %zd bytes (Total: %llu)", bytesRead, (unsigned long long)bytesReceived);

            pendingData += buffer;
            size_t pos;
            while ((pos = pendingData.find('\n')) != std::string::npos) {
                std::string msg = pendingData.substr(0, pos);
                pendingData.erase(0, pos + 1);

                if (recvCallback) {
                    recvCallback(msg);
                }
            }
        } else {
            if (bytesRead == 0) {
                LOGW(">>> [NET] Server closed connection.");
            } else {
                LOGE(">>> [NET] Recv error: %s", strerror(errno));
            }
            // Сигнализируем о разрыве
            connectionBroken = true;
            cv.notify_all(); // Будим WorkerThread, чтобы он пошел на реконнект
            break;
        }
    }
    LOGI(">>> [NET] ReceiveLoop stopped");
}

// Главный поток (Управляет соединением и отправкой)
void NetworkSender::WorkerThread() {
    LOGI(">>> [NET] WorkerThread started");

    while (isRunning) {
        // Сброс состояния перед попыткой
        connectionBroken = false;
        isConnected = false;

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            LOGE(">>> [NET] Socket creation failed: %s", strerror(errno));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Настройка таймаутов (опционально)
        struct timeval tv;
        tv.tv_sec = 5; tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(serverPort);
        inet_pton(AF_INET, serverIp.c_str(), &serv_addr.sin_addr);

        LOGI(">>> [NET] Connecting to %s:%d...", serverIp.c_str(), serverPort);
        
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            LOGE(">>> [NET] Connect failed: %s. Retry in 2s...", strerror(errno));
            close(sock);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue; // Повторяем цикл while(isRunning)
        }

        LOGI(">>> [NET] Connected! Session Start.");
        isConnected = true;

        // Запускаем чтение в отдельном потоке
        std::thread receiver(&NetworkSender::ReceiveLoop, this, sock);
        receiver.detach(); 

        // Цикл отправки
        try {
            while (isRunning && !connectionBroken) {
                std::string payload;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    // Ждем, пока что-то появится В ОЧЕРЕДИ или СЛОМАЕТСЯ СОЕДИНЕНИЕ
                    cv.wait(lock, [this]{ 
                        return !queue.empty() || !isRunning || connectionBroken; 
                    });

                    if (!isRunning || connectionBroken) break;
                    if (queue.empty()) continue;

                    payload = queue.front();
                    queue.pop();
                }

                payload += "\n";
                ssize_t sentBytes = send(sock, payload.c_str(), payload.size(), MSG_NOSIGNAL);
                
                if (sentBytes < 0) {
                    LOGE(">>> [NET] Send error: %s", strerror(errno));
                    connectionBroken = true; // Выходим из цикла отправки
                    break; 
                } else {
                    bytesSent += sentBytes;
                    // LOGI(">>> [NET] Sent %zd bytes. (Total Out: %llu)", sentBytes, (unsigned long long)bytesSent);
                }
            }
        } catch (const std::exception& e) {
            LOGE(">>> [NET] Exception in sender loop: %s", e.what());
        }

        // Завершение сессии (очистка)
        isConnected = false;
        LOGW(">>> [NET] Session lost. Closing socket and retrying...");
        
        shutdown(sock, SHUT_RDWR); // Это разбудит ReceiveLoop, если он висит в recv
        close(sock);
        
        // Небольшая пауза перед реконнектом, чтобы не спамить
        if (isRunning) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    LOGI(">>> [NET] WorkerThread finished completely.");
}