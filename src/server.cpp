#include "server.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
    #define CLOSE_SOCKET closesocket
    static void platform_init() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }
    static void platform_cleanup() { WSACleanup(); }
#else
    #include <signal.h>
    #define CLOSE_SOCKET ::close
    static void platform_init() {
        // Ignore SIGPIPE so writing to a closed socket doesn't kill us
        signal(SIGPIPE, SIG_IGN);
    }
    static void platform_cleanup() {}
#endif

// ── Wire protocol ───────────────────────────────────────────────────────────
// Client -> Server:  [4-byte big-endian length][raw PCM payload]
// Server -> Client:  [4-byte big-endian length][JSON payload]

static uint32_t read_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}

static void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF;
    p[3] =  v        & 0xFF;
}

// ── Server implementation ───────────────────────────────────────────────────

Server::Server(uint16_t port, AudioHandler handler, TranslateHandler translate_handler)
    : port_(port), handler_(std::move(handler)), translate_handler_(std::move(translate_handler)) {
    platform_init();
}

Server::~Server() {
    stop();
    platform_cleanup();
}

void Server::run() {
    listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock_ == SOCKET_INVALID) {
        throw std::runtime_error("Failed to create socket");
    }

    // Allow quick restart after crash
    int opt = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        CLOSE_SOCKET(listen_sock_);
        throw std::runtime_error("Failed to bind on port " + std::to_string(port_));
    }

    if (listen(listen_sock_, 4) < 0) {
        CLOSE_SOCKET(listen_sock_);
        throw std::runtime_error("Failed to listen");
    }

    running_ = true;
    std::cout << "[server] Listening on port " << port_ << std::endl;

    while (running_) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        socket_t    client_sock = accept(listen_sock_,
                                         reinterpret_cast<sockaddr*>(&client_addr),
                                         &client_len);
        if (client_sock == SOCKET_INVALID) {
            if (!running_) break;
            std::cerr << "[server] Accept failed, continuing..." << std::endl;
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        int client_id = client_count_.fetch_add(1) + 1;
        std::cout << "[server] Client #" << client_id << " connected from " << ip << std::endl;

        std::thread([this, client_sock, client_id]() {
            handle_client(client_sock, client_id);
            CLOSE_SOCKET(client_sock);
            std::cout << "[server] Client #" << client_id << " disconnected" << std::endl;
        }).detach();
    }
}

void Server::stop() {
    running_ = false;
    if (listen_sock_ != SOCKET_INVALID) {
        CLOSE_SOCKET(listen_sock_);
        listen_sock_ = SOCKET_INVALID;
    }
}

void Server::register_client(int client_id, socket_t sock, const std::string& locale) {
    std::lock_guard<std::mutex> lock(clients_mtx_);
    clients_[client_id] = {sock, locale};
    std::cout << "[server] Registered Client #" << client_id << " (locale: " << locale << ")" << std::endl;
}

void Server::unregister_client(int client_id) {
    std::lock_guard<std::mutex> lock(clients_mtx_);
    clients_.erase(client_id);
    std::cout << "[server] Unregistered Client #" << client_id << std::endl;
}

void Server::broadcast(int sender_id, const std::string& message, const std::string& source_lang) {
    std::lock_guard<std::mutex> lock(clients_mtx_);
    for (auto& [id, info] : clients_) {
        if (id == sender_id) continue;  // Don't echo back to sender

        // Translate if source and receiver languages differ
        std::string msg_to_send = message;
        if (translate_handler_ && !info.locale.empty() && info.locale != source_lang) {
            msg_to_send = translate_handler_(message, source_lang, info.locale);
        }

        uint32_t msg_len = static_cast<uint32_t>(msg_to_send.size());
        uint8_t hdr[4];
        write_u32_be(hdr, msg_len);

        if (!send_all(info.sock, hdr, 4) ||
            !send_all(info.sock, msg_to_send.data(), msg_len)) {
            std::cerr << "[server] Failed to broadcast to Client #" << id << std::endl;
        }
    }
}

void Server::handle_client(socket_t client_sock, int client_id) {
    // Register with a default locale; updated with each message
    register_client(client_id, client_sock, "en");

    while (running_) {
        // Read 1-byte locale length + locale string
        uint8_t locale_len;
        if (!recv_all(client_sock, &locale_len, 1)) {
            break;  // Client disconnected
        }
        if (locale_len == 0 || locale_len > 16) {
            std::cerr << "[server] Invalid locale length (" << (int)locale_len << "), dropping" << std::endl;
            break;
        }
        std::vector<char> locale_buf(locale_len);
        if (!recv_all(client_sock, locale_buf.data(), locale_len)) {
            break;
        }
        std::string locale(locale_buf.begin(), locale_buf.end());

        // Update locale in registry (in case client changes language mid-session)
        {
            std::lock_guard<std::mutex> lock(clients_mtx_);
            std::string& stored_locale = clients_[client_id].locale;
            if (stored_locale != locale) {
                std::cout << "[server] Client #" << client_id << " updated locale: " << stored_locale << " -> " << locale << std::endl;
                stored_locale = locale;
            }
        }

        // Read 4-byte audio length header
        uint8_t len_buf[4];
        if (!recv_all(client_sock, len_buf, 4)) {
            break;
        }

        uint32_t payload_len = read_u32_be(len_buf);

        // Zero-length audio = registration/locale update only
        if (payload_len == 0) {
            std::cout << "[server] Client #" << client_id << " (" << locale << ") sent locale update" << std::endl;
            continue;
        }

        // Sanity check: reject payloads > 50 MB
        if (payload_len > 50 * 1024 * 1024) {
            std::cerr << "[server] Payload too large (" << payload_len << " bytes), dropping" << std::endl;
            break;
        }

        // Read audio payload
        std::vector<uint8_t> audio(payload_len);
        if (!recv_all(client_sock, audio.data(), payload_len)) {
            std::cerr << "[server] Incomplete audio payload" << std::endl;
            break;
        }

        std::cout << "[server] Client #" << client_id << " (" << locale << ") sent " << payload_len << " bytes of audio" << std::endl;

        // Run transcription via the pool (thread-safe, borrows a model instance)
        std::string response = handler_(client_id, locale, audio);

        // Send response back to sender: [4-byte length][JSON]
        uint32_t resp_len = static_cast<uint32_t>(response.size());
        uint8_t resp_hdr[4];
        write_u32_be(resp_hdr, resp_len);

        if (!send_all(client_sock, resp_hdr, 4) ||
            !send_all(client_sock, response.data(), resp_len)) {
            std::cerr << "[server] Failed to send response" << std::endl;
            break;
        }

        // Broadcast to all other connected clients (with per-client translation)
        broadcast(client_id, response, locale);
    }

    unregister_client(client_id);
}

bool Server::recv_all(socket_t sock, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        auto n = recv(sock, reinterpret_cast<char*>(p), static_cast<int>(remaining), 0);
        if (n <= 0) return false;
        p += n;
        remaining -= n;
    }
    return true;
}

bool Server::send_all(socket_t sock, const void* buf, size_t len) {
    auto* p = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        auto n = send(sock, reinterpret_cast<const char*>(p), static_cast<int>(remaining), 0);
        if (n <= 0) return false;
        p += n;
        remaining -= n;
    }
    return true;
}
