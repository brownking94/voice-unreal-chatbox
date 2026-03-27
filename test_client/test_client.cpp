#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── Cross-platform socket includes (must precede miniaudio to avoid
//    winsock.h / winsock2.h redefinition conflicts on Windows) ────────────────
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

// ── miniaudio ───────────────────────────────────────────────────────────────
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

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

// Send audio data to server and print the transcription response
// Wire format: [1-byte locale length][locale string][4-byte audio length][audio data]
static bool send_and_receive(socket_t sock, const std::vector<uint8_t>& pcm_data, const std::string& locale) {
    // Send locale prefix
    uint8_t loc_len = static_cast<uint8_t>(locale.size());
    if (!send_all(sock, &loc_len, 1) || !send_all(sock, locale.data(), loc_len)) {
        std::cerr << "Failed to send locale" << std::endl;
        return false;
    }

    // Send audio
    uint32_t payload_len = static_cast<uint32_t>(pcm_data.size());
    uint8_t hdr[4];
    write_u32_be(hdr, payload_len);

    if (!send_all(sock, hdr, 4) || !send_all(sock, pcm_data.data(), payload_len)) {
        std::cerr << "Failed to send audio data" << std::endl;
        return false;
    }

    uint8_t resp_hdr[4];
    if (!recv_all(sock, resp_hdr, 4)) {
        std::cerr << "Failed to receive response header" << std::endl;
        return false;
    }

    uint32_t resp_len = read_u32_be(resp_hdr);
    std::vector<char> resp_buf(resp_len);
    if (!recv_all(sock, resp_buf.data(), resp_len)) {
        std::cerr << "Failed to receive response body" << std::endl;
        return false;
    }

    std::string response(resp_buf.begin(), resp_buf.end());
    std::cout << ">> " << response << std::endl;
    return true;
}

// ── Microphone capture with VAD ─────────────────────────────────────────────

static constexpr uint32_t MIC_SAMPLE_RATE     = 16000;
static constexpr uint32_t MIC_CHANNELS        = 1;
static constexpr float    VAD_ENERGY_THRESHOLD = 0.02f;    // RMS threshold to detect speech
static constexpr float    SILENCE_TIMEOUT_SEC  = 0.7f;     // Seconds of silence before auto-send
static constexpr float    MAX_CHUNK_SEC        = 10.0f;    // Hard cap per chunk
static constexpr uint32_t MAX_CHUNK_SAMPLES    = static_cast<uint32_t>(MIC_SAMPLE_RATE * MAX_CHUNK_SEC);

enum class VadState { IDLE, RECORDING, TRAILING_SILENCE };

struct MicState {
    std::mutex           mtx;

    // Double buffer: callback writes to speech_buf, main loop drains ready_buf
    std::vector<int16_t> speech_buf;       // Currently recording into this
    std::vector<int16_t> ready_buf;        // Completed chunk waiting to be sent

    VadState             state = VadState::IDLE;
    std::atomic<bool>    chunk_ready{false};  // Signals main loop a chunk is in ready_buf
    uint32_t             silence_samples = 0;
};

// Compute RMS energy of a block of int16 samples, normalized to [0, 1]
static float compute_rms(const int16_t* samples, uint32_t count) {
    if (count == 0) return 0.0f;
    double sum = 0.0;
    for (uint32_t i = 0; i < count; i++) {
        float s = static_cast<float>(samples[i]) / 32768.0f;
        sum += s * s;
    }
    return static_cast<float>(std::sqrt(sum / count));
}

static void mic_callback(ma_device* device, void* /*output*/, const void* input, ma_uint32 frame_count) {
    auto* state = static_cast<MicState*>(device->pUserData);
    const auto* samples = static_cast<const int16_t*>(input);

    float rms = compute_rms(samples, frame_count);
    bool is_speech = (rms > VAD_ENERGY_THRESHOLD);

    std::lock_guard<std::mutex> lock(state->mtx);

    // Helper: move speech_buf into ready_buf and reset for next chunk.
    // If the main loop hasn't consumed the previous ready_buf yet, we
    // append to it so no audio is lost (the server will just get a
    // bigger chunk next time).
    auto flush_chunk = [&](bool start_new_recording, const int16_t* carry_samples, uint32_t carry_count) {
        if (!state->ready_buf.empty()) {
            // Previous chunk not yet consumed — append to it
            state->ready_buf.insert(state->ready_buf.end(),
                                    state->speech_buf.begin(), state->speech_buf.end());
        } else {
            state->ready_buf.swap(state->speech_buf);
        }
        state->speech_buf.clear();
        state->chunk_ready.store(true);

        if (start_new_recording && carry_count > 0) {
            // Person is still talking — start the next chunk immediately
            state->state = VadState::RECORDING;
            state->silence_samples = 0;
            state->speech_buf.insert(state->speech_buf.end(), carry_samples, carry_samples + carry_count);
        } else {
            state->state = VadState::IDLE;
        }
    };

    switch (state->state) {
        case VadState::IDLE:
            if (is_speech) {
                state->state = VadState::RECORDING;
                state->speech_buf.clear();
                state->silence_samples = 0;
                state->speech_buf.insert(state->speech_buf.end(), samples, samples + frame_count);
            }
            break;

        case VadState::RECORDING:
            state->speech_buf.insert(state->speech_buf.end(), samples, samples + frame_count);

            if (!is_speech) {
                state->state = VadState::TRAILING_SILENCE;
                state->silence_samples = frame_count;
            }

            // Hard cap: flush but keep recording if still speaking
            if (state->speech_buf.size() >= MAX_CHUNK_SAMPLES) {
                flush_chunk(is_speech, samples, is_speech ? frame_count : 0);
            }
            break;

        case VadState::TRAILING_SILENCE:
            state->speech_buf.insert(state->speech_buf.end(), samples, samples + frame_count);

            if (is_speech) {
                state->state = VadState::RECORDING;
                state->silence_samples = 0;
            } else {
                state->silence_samples += frame_count;
                uint32_t silence_threshold = static_cast<uint32_t>(MIC_SAMPLE_RATE * SILENCE_TIMEOUT_SEC);
                if (state->silence_samples >= silence_threshold) {
                    flush_chunk(false, nullptr, 0);
                }
            }
            break;
    }
}

static int run_mic_mode(const std::string& host, uint16_t port, const std::string& locale) {
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

    std::cout << "Connected to server. Locale: " << locale << std::endl;

    MicState mic_state;

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format   = ma_format_s16;
    config.capture.channels = MIC_CHANNELS;
    config.sampleRate       = MIC_SAMPLE_RATE;
    config.dataCallback     = mic_callback;
    config.pUserData        = &mic_state;

    ma_device device;
    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
        std::cerr << "Failed to initialize microphone" << std::endl;
        CLOSE_SOCKET(sock);
        platform_cleanup();
        return 1;
    }

    if (ma_device_start(&device) != MA_SUCCESS) {
        std::cerr << "Failed to start microphone" << std::endl;
        ma_device_uninit(&device);
        CLOSE_SOCKET(sock);
        platform_cleanup();
        return 1;
    }

    std::cout << "Listening... (speak to transcribe, Ctrl+C to quit)" << std::endl;
    std::cout << "  VAD threshold: " << VAD_ENERGY_THRESHOLD
              << "  silence timeout: " << SILENCE_TIMEOUT_SEC << "s"
              << "  max chunk: " << MAX_CHUNK_SEC << "s" << std::endl;
    std::cout << std::endl;

    while (true) {
        // Poll every 50ms for a ready chunk
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (!mic_state.chunk_ready.load()) continue;

        // Grab the ready buffer (callback keeps recording into speech_buf)
        std::vector<int16_t> captured;
        {
            std::lock_guard<std::mutex> lock(mic_state.mtx);
            captured.swap(mic_state.ready_buf);
            mic_state.chunk_ready.store(false);
        }

        if (captured.empty()) continue;

        // Convert to bytes
        size_t byte_count = captured.size() * sizeof(int16_t);
        std::vector<uint8_t> pcm_bytes(byte_count);
        std::memcpy(pcm_bytes.data(), captured.data(), byte_count);

        float duration = static_cast<float>(captured.size()) / MIC_SAMPLE_RATE;
        std::cout << "[speech detected: " << duration << "s, sending...]" << std::endl;

        if (!send_and_receive(sock, pcm_bytes, locale)) {
            std::cerr << "Server communication failed" << std::endl;
            break;
        }
    }

    ma_device_stop(&device);
    ma_device_uninit(&device);
    CLOSE_SOCKET(sock);
    platform_cleanup();
    return 0;
}

// ── Main ────────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-s <host>] [-p <port>] [-l <locale>]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  -s <host>    Server host (default: 127.0.0.1)" << std::endl;
    std::cerr << "  -p <port>    Server port (default: 9090)" << std::endl;
    std::cerr << "  -l <locale>  Language code for STT, e.g. en, ja, auto (default: en)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 9090;
    std::string locale = "en";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-s" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "-l" && i + 1 < argc) {
            locale = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    return run_mic_mode(host, port, locale);
}
