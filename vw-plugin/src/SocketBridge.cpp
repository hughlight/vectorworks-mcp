//
// SocketBridge.cpp — Unix Domain Socket サーバー
// autocad-mcp の socket_bridge.cpp と同じアーキテクチャ
// Vectorworks SDK に nlohmann JSON が内蔵されているため、自前パーサー不要
//

#include "SocketBridge.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>
#include <cstring>

SocketBridge& SocketBridge::Instance() {
    static SocketBridge s_instance;
    return s_instance;
}

SocketBridge::~SocketBridge() {
    Stop();
}

bool SocketBridge::Start(const std::string& socketPath) {
    if (m_running) return false;

    m_socketPath = socketPath;
    unlink(socketPath.c_str());

    m_serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_serverFd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(m_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    if (listen(m_serverFd, 5) < 0) {
        close(m_serverFd);
        m_serverFd = -1;
        unlink(socketPath.c_str());
        return false;
    }

    m_running = true;
    m_serverThread = std::thread(&SocketBridge::ServerLoop, this);
    return true;
}

void SocketBridge::Stop() {
    if (!m_running) return;

    m_running = false;

    if (m_serverFd >= 0) {
        shutdown(m_serverFd, SHUT_RDWR);
        close(m_serverFd);
        m_serverFd = -1;
    }

    // accept() を起こすダミー接続
    int dummy = socket(AF_UNIX, SOCK_STREAM, 0);
    if (dummy >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);
        connect(dummy, (struct sockaddr*)&addr, sizeof(addr));
        close(dummy);
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }

    unlink(m_socketPath.c_str());
}

void SocketBridge::SetHandler(CommandHandler handler) {
    m_handler = handler;
}

void SocketBridge::ServerLoop() {
    while (m_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(m_serverFd, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(m_serverFd + 1, &readfds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;
        if (!m_running) break;

        int clientFd = accept(m_serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (!m_running) break;
            continue;
        }

        struct timeval clientTimeout;
        clientTimeout.tv_sec = 30;
        clientTimeout.tv_usec = 0;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &clientTimeout, sizeof(clientTimeout));

        HandleClient(clientFd);
        close(clientFd);
    }
}

void SocketBridge::HandleClient(int clientFd) {
    char buf[65536];
    std::string buffer;

    while (m_running) {
        ssize_t n = read(clientFd, buf, sizeof(buf) - 1);
        if (n <= 0) break;

        buf[n] = '\0';
        buffer += buf;

        // 改行区切りでコマンド処理
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer = buffer.substr(pos + 1);

            if (line.empty()) continue;

            try {
                nlohmann::json cmd = nlohmann::json::parse(line);
                nlohmann::json result;

                if (m_handler) {
                    result = m_handler(cmd);
                } else {
                    result["id"] = cmd.value("id", "");
                    result["success"] = false;
                    result["data"] = "no handler";
                }

                std::string response = result.dump() + "\n";
                write(clientFd, response.c_str(), response.size());
            }
            catch (const std::exception& e) {
                nlohmann::json err;
                err["success"] = false;
                err["data"] = std::string("Parse error: ") + e.what();
                std::string response = err.dump() + "\n";
                write(clientFd, response.c_str(), response.size());
            }
        }
    }
}
