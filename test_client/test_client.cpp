#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// ── Cross-platform socket includes ──────────────────────────────────────────
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    constexpr socket_t SOCKET_INVALID = INVALID_SOCKET;
    #define CLOSE_SOCKET closesocket
    static void platform_init() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::cerr << "WSAStartup failed" << std::endl;
            exit(1);
        }
    }
    static void platform_cleanup() { WSACleanup(); }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using socket_t = int;
    constexpr socket_t SOCKET_INVALID = -1;
    #define CLOSE_SOCKET ::close
    static void platform_init() {}
    static void platform_cleanup() {}
#endif

// ── Helpers ─────────────────────────────────────────────────────────────────

static void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF;
    p[3] =  v        & 0xFF;
}

static uint32_t read_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}

static bool recv_all(socket_t sock, void* buf, size_t len) {
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

static bool send_all(socket_t sock, const void* buf, size_t len) {
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

// ── WAV file reader ─────────────────────────────────────────────────────────
// Reads a standard PCM WAV file and returns the raw audio samples.
// Validates format: must be 16-bit PCM, 16 kHz, mono.

struct WavData {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    std::vector<uint8_t> pcm_data;
};

static bool read_wav(const std::string& path, WavData& wav) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Cannot open file: " << path << std::endl;
        return false;
    }

    // Read RIFF header
    char riff[4];
    file.read(riff, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0) {
        std::cerr << "Not a RIFF file" << std::endl;
        return false;
    }

    uint32_t file_size;
    file.read(reinterpret_cast<char*>(&file_size), 4);

    char wave[4];
    file.read(wave, 4);
    if (std::memcmp(wave, "WAVE", 4) != 0) {
        std::cerr << "Not a WAVE file" << std::endl;
        return false;
    }

    // Read chunks until we find "fmt " and "data"
    bool found_fmt = false, found_data = false;
    while (file && !(found_fmt && found_data)) {
        char chunk_id[4];
        uint32_t chunk_size;
        file.read(chunk_id, 4);
        file.read(reinterpret_cast<char*>(&chunk_size), 4);

        if (!file) break;

        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t audio_format;
            file.read(reinterpret_cast<char*>(&audio_format), 2);
            file.read(reinterpret_cast<char*>(&wav.channels), 2);
            file.read(reinterpret_cast<char*>(&wav.sample_rate), 4);
            uint32_t byte_rate;
            file.read(reinterpret_cast<char*>(&byte_rate), 4);
            uint16_t block_align;
            file.read(reinterpret_cast<char*>(&block_align), 2);
            file.read(reinterpret_cast<char*>(&wav.bits_per_sample), 2);

            if (audio_format != 1) {
                std::cerr << "Not PCM format (format=" << audio_format << ")" << std::endl;
                return false;
            }

            // Skip any extra fmt bytes
            if (chunk_size > 16) {
                file.seekg(chunk_size - 16, std::ios::cur);
            }
            found_fmt = true;

        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            wav.pcm_data.resize(chunk_size);
            file.read(reinterpret_cast<char*>(wav.pcm_data.data()), chunk_size);
            found_data = true;

        } else {
            // Skip unknown chunk
            file.seekg(chunk_size, std::ios::cur);
        }
    }

    if (!found_fmt || !found_data) {
        std::cerr << "Missing fmt or data chunk" << std::endl;
        return false;
    }

    return true;
}

// ── Main ────────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <wav_file> [-h <host>] [-p <port>]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  <wav_file>   Path to a 16-bit PCM WAV file (16 kHz mono recommended)" << std::endl;
    std::cerr << "  -h <host>    Server host (default: 127.0.0.1)" << std::endl;
    std::cerr << "  -p <port>    Server port (default: 9090)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string wav_path;
    std::string host = "127.0.0.1";
    uint16_t port = 9090;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (wav_path.empty()) {
            wav_path = arg;
        }
    }

    if (wav_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Read WAV file
    WavData wav;
    if (!read_wav(wav_path, wav)) {
        return 1;
    }

    std::cout << "WAV: " << wav.channels << " ch, "
              << wav.sample_rate << " Hz, "
              << wav.bits_per_sample << " bit, "
              << wav.pcm_data.size() << " bytes" << std::endl;

    if (wav.bits_per_sample != 16) {
        std::cerr << "Warning: expected 16-bit audio, got " << wav.bits_per_sample << "-bit" << std::endl;
    }
    if (wav.sample_rate != 16000) {
        std::cerr << "Warning: expected 16000 Hz, got " << wav.sample_rate << " Hz" << std::endl;
        std::cerr << "Whisper expects 16 kHz mono — transcription quality may suffer." << std::endl;
    }

    // Connect to server
    platform_init();

    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == SOCKET_INVALID) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to server" << std::endl;
        CLOSE_SOCKET(sock);
        platform_cleanup();
        return 1;
    }

    std::cout << "Connected. Sending " << wav.pcm_data.size() << " bytes of audio..." << std::endl;

    // Send: [4-byte length][pcm data]
    uint32_t payload_len = static_cast<uint32_t>(wav.pcm_data.size());
    uint8_t hdr[4];
    write_u32_be(hdr, payload_len);

    if (!send_all(sock, hdr, 4) || !send_all(sock, wav.pcm_data.data(), payload_len)) {
        std::cerr << "Failed to send audio data" << std::endl;
        CLOSE_SOCKET(sock);
        platform_cleanup();
        return 1;
    }

    std::cout << "Waiting for transcription..." << std::endl;

    // Receive: [4-byte length][json response]
    uint8_t resp_hdr[4];
    if (!recv_all(sock, resp_hdr, 4)) {
        std::cerr << "Failed to receive response header" << std::endl;
        CLOSE_SOCKET(sock);
        platform_cleanup();
        return 1;
    }

    uint32_t resp_len = read_u32_be(resp_hdr);
    std::vector<char> resp_buf(resp_len);
    if (!recv_all(sock, resp_buf.data(), resp_len)) {
        std::cerr << "Failed to receive response body" << std::endl;
        CLOSE_SOCKET(sock);
        platform_cleanup();
        return 1;
    }

    std::string response(resp_buf.begin(), resp_buf.end());
    std::cout << "Response: " << response << std::endl;

    CLOSE_SOCKET(sock);
    platform_cleanup();
    return 0;
}
