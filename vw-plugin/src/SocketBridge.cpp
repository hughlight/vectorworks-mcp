//
// SocketBridge.cpp — TCP localhost socket server bridging the MCP server to
// the VW plugin. Cross-platform: Winsock2 on Windows, POSIX sockets elsewhere.
//

#include "SocketBridge.h"

#include <cstring>
#include <cstdio>
#include <sstream>

#ifdef _WIN32
  #include <ws2tcpip.h>
  #include <io.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "kernel32.lib")

  using ssize_t = SSIZE_T;

  #define SOCK_INVALID   INVALID_SOCKET
  #define SOCK_FAIL      SOCKET_ERROR
  #define SHUT_RW_FLAG   SD_BOTH

  static inline int socket_close(SocketFd fd) { return ::closesocket(fd); }
  static inline ssize_t socket_read(SocketFd fd, void* buf, size_t len) {
      return ::recv(fd, static_cast<char*>(buf), static_cast<int>(len), 0);
  }
  static inline ssize_t socket_write(SocketFd fd, const void* buf, size_t len) {
      return ::send(fd, static_cast<const char*>(buf), static_cast<int>(len), 0);
  }

  // Winsock initialization is reference-counted; calling WSAStartup multiple
  // times is cheap. We match each WSAStartup with a WSACleanup in Stop().
  static bool winsock_startup() { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }
  static void winsock_shutdown() { WSACleanup(); }

#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/select.h>
  #include <unistd.h>

  #define SOCK_INVALID   (-1)
  #define SOCK_FAIL      (-1)
  #define SHUT_RW_FLAG   SHUT_RDWR

  static inline int socket_close(SocketFd fd) { return ::close(fd); }
  static inline ssize_t socket_read(SocketFd fd, void* buf, size_t len) {
      return ::read(fd, buf, len);
  }
  static inline ssize_t socket_write(SocketFd fd, const void* buf, size_t len) {
      return ::write(fd, buf, len);
  }

  static bool winsock_startup() { return true; }
  static void winsock_shutdown() {}
#endif


SocketBridge& SocketBridge::Instance() {
    // Singleton. NEVER reset members here — each call must return the same
    // live state. (Earlier version reset m_serverFd, which silently destroyed
    // any listening socket as soon as anyone re-queried the singleton.)
    static SocketBridge s_instance;
    return s_instance;
}

SocketBridge::~SocketBridge() {
    Stop();
}

unsigned short SocketBridge::GetDefaultPort(int sdkVersion) {
    // 27200 + (sdkVersion/100). SDK 3000 (VW2025) -> 27230, SDK 3100 (VW2026) -> 27231.
    unsigned short base = 27200;
    if (sdkVersion > 0) {
        base += static_cast<unsigned short>(sdkVersion / 100);
    }
    return base;
}

std::string SocketBridge::GetDefaultEndpoint(int sdkVersion) {
    std::ostringstream oss;
    oss << "127.0.0.1:" << GetDefaultPort(sdkVersion);
    return oss.str();
}

bool SocketBridge::Start(unsigned short port) {
    if (m_running) return false;

    m_port = port;

    if (!winsock_startup()) return false;

    m_serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverFd == SOCK_INVALID) {
        winsock_shutdown();
        return false;
    }

    // SO_REUSEADDR so we can rebind quickly after a stop/start cycle.
#ifdef _WIN32
    BOOL one = TRUE;
    ::setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));
#else
    int one = 1;
    ::setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // Bind to 127.0.0.1 only — never expose this socket to the network.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(m_serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCK_FAIL) {
        socket_close(m_serverFd);
        m_serverFd = SOCK_INVALID;
        winsock_shutdown();
        return false;
    }

    if (::listen(m_serverFd, 5) == SOCK_FAIL) {
        socket_close(m_serverFd);
        m_serverFd = SOCK_INVALID;
        winsock_shutdown();
        return false;
    }

    m_running = true;
    m_serverThread = std::thread(&SocketBridge::ServerLoop, this);
    return true;
}

void SocketBridge::Stop() {
    if (!m_running) return;

    m_running = false;

    if (m_serverFd != SOCK_INVALID) {
        ::shutdown(m_serverFd, SHUT_RW_FLAG);
        socket_close(m_serverFd);
        m_serverFd = SOCK_INVALID;
    }

    // Wake up select()/accept() with a dummy connection so the server thread
    // notices m_running == false and exits cleanly.
    SocketFd dummy = ::socket(AF_INET, SOCK_STREAM, 0);
    if (dummy != SOCK_INVALID) {
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::connect(dummy, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        socket_close(dummy);
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }

    winsock_shutdown();
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

        int ret = ::select(static_cast<int>(m_serverFd) + 1,
                           &readfds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;
        if (!m_running) break;

        SocketFd clientFd = ::accept(m_serverFd, nullptr, nullptr);
        if (clientFd == SOCK_INVALID) {
            if (!m_running) break;
            continue;
        }

#ifdef _WIN32
        DWORD timeoutMs = 30000;
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
        struct timeval clientTimeout;
        clientTimeout.tv_sec = 30;
        clientTimeout.tv_usec = 0;
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO,
                     &clientTimeout, sizeof(clientTimeout));
#endif

        HandleClient(clientFd);
        socket_close(clientFd);
    }
}

void SocketBridge::HandleClient(SocketFd clientFd) {
    char buf[65536];
    std::string buffer;

    while (m_running) {
        ssize_t n = socket_read(clientFd, buf, sizeof(buf) - 1);
        if (n <= 0) break;

        buf[n] = '\0';
        buffer.append(buf, static_cast<size_t>(n));

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            nlohmann::json result;
            try {
                nlohmann::json cmd = nlohmann::json::parse(line);

                if (m_handler) {
                    result = m_handler(cmd);
                } else {
                    result["id"] = cmd.value("id", "");
                    result["success"] = false;
                    result["data"] = "no handler registered";
                }
            }
            catch (const std::exception& e) {
                result["success"] = false;
                result["data"] = std::string("Parse error: ") + e.what();
            }

            std::string response = result.dump() + "\n";
            socket_write(clientFd, response.c_str(), response.size());
        }
    }
}
