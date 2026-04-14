#pragma once

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>

// MCP サーバーと Vectorworks プラグイン間の Unix Domain Socket ブリッジ
// autocad-mcp と同じアーキテクチャ

using CommandHandler = std::function<nlohmann::json(const nlohmann::json&)>;

class SocketBridge {
public:
    static SocketBridge& Instance();

    bool Start(const std::string& socketPath);
    void Stop();
    void SetHandler(CommandHandler handler);

    bool IsRunning() const { return m_running; }
    const std::string& GetSocketPath() const { return m_socketPath; }

private:
    SocketBridge() = default;
    ~SocketBridge();

    void ServerLoop();
    void HandleClient(int clientFd);

    std::string m_socketPath;
    int m_serverFd = -1;
    std::atomic<bool> m_running{false};
    std::thread m_serverThread;
    CommandHandler m_handler;
};
