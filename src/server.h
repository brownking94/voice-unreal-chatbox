#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ── Cross-platform socket types ─────────────────────────────────────────────
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    constexpr socket_t SOCKET_INVALID = INVALID_SOCKET;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using socket_t = int;
    constexpr socket_t SOCKET_INVALID = -1;
#endif

// Callback: receives raw PCM audio bytes, returns transcribed JSON response.
// Must be thread-safe — called from multiple client threads concurrently.
using AudioHandler = std::function<std::string(const std::vector<uint8_t>& audio_data)>;

class Server {
public:
    Server(uint16_t port, AudioHandler handler);
    ~Server();

    // Blocks, listening for connections until stop() is called
    void run();
    void stop();

private:
    void handle_client(socket_t client_sock, int client_id);
    bool recv_all(socket_t sock, void* buf, size_t len);
    bool send_all(socket_t sock, const void* buf, size_t len);

    uint16_t          port_;
    AudioHandler      handler_;
    socket_t          listen_sock_ = SOCKET_INVALID;
    std::atomic<bool> running_{false};
    std::atomic<int>  client_count_{0};
};
