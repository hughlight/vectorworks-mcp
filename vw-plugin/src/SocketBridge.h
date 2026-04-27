#pragma once

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>

// MCP server <-> Vectorworks plugin TCP localhost bridge.
//
// Cross-platform: Winsock2 on Windows, POSIX sockets on macOS/Linux.
// We bind to 127.0.0.1:<port> where port is derived from the VW version so
// VW2025 and VW2026 plugins coexist without colliding (VW2025 -> 27230,
// VW2026 -> 27231, computed as 27200 + SDK_VERSION/100).
//
// TCP localhost was chosen over AF_UNIX because Rust tokio's UnixStream is
// POSIX-only, and using it would require either Windows-specific named-pipe
// branches or low-level windows-sys glue. TCP is universally supported on
// both sides at negligible perf cost for our message rate.

#ifdef _WIN32
  #ifndef _WINSOCKAPI_
    #define _WINSOCKAPI_
  #endif
  #include <winsock2.h>
  using SocketFd = SOCKET;
#else
  using SocketFd = int;
#endif

using CommandHandler = std::function<nlohmann::json(const nlohmann::json&)>;

class SocketBridge {
public:
    static SocketBridge& Instance();

    /// Returns 27200 + (sdkVersion/100), e.g. 27230 for VW2025 (SDK 3000) or
    /// 27231 for VW2026 (SDK 3100). Pass 0 for the legacy untagged port 27200.
    static unsigned short GetDefaultPort(int sdkVersion = 0);

    /// Convenience: returns "127.0.0.1:<port>" for display in alert dialogs.
    static std::string GetDefaultEndpoint(int sdkVersion = 0);

    bool Start(unsigned short port);
    void Stop();
    void SetHandler(CommandHandler handler);

    bool IsRunning() const { return m_running; }
    unsigned short GetPort() const { return m_port; }

private:
    SocketBridge() = default;
    ~SocketBridge();

    void ServerLoop();
    void HandleClient(SocketFd clientFd);

    unsigned short m_port = 0;
    // Initialize to "invalid" so the singleton is safe to query before first
    // Start(). SOCK_INVALID is INVALID_SOCKET on Windows (= ~0) and -1 on POSIX;
    // both are achievable by casting -1 to SocketFd.
    SocketFd m_serverFd = static_cast<SocketFd>(-1);
    std::atomic<bool> m_running{false};
    std::thread m_serverThread;
    CommandHandler m_handler;
};
