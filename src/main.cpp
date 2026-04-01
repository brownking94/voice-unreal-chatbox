#include "server.h"
#include "transcriber_pool.h"
#include "filter.h"
#include "protocol.h"

#include <iostream>
#include <string>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-m <model>] [-p <port>] [-w <workers>]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  -m <path>      Whisper model (default: models/ggml-small.bin)" << std::endl;
    std::cerr << "  -p <port>      TCP port (default: 9090)" << std::endl;
    std::cerr << "  -w <workers>   Parallel transcription instances (default: 1)" << std::endl;
    std::cerr << "  -f <path>      Profanity word list (default: config/profanity.txt)" << std::endl;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::string model_path = "models/ggml-small.bin";
    std::string filter_path = "config/profanity.txt";
    uint16_t port = 9090;
    int workers = 1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-m" && i + 1 < argc) {
            model_path = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "-w" && i + 1 < argc) {
            workers = std::atoi(argv[++i]);
        } else if (arg == "-f" && i + 1 < argc) {
            filter_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (workers < 1) workers = 1;

    TranscriberPool pool(model_path, workers);
    ProfanityFilter filter(filter_path);

    // Translation handler for broadcast
    // Uses Whisper's built-in English translation for all cross-language listeners
    TranslateHandler translate_handler = [](
        const std::string& json_response,
        const std::string& source_lang,
        const std::string& target_lang) -> std::string {

        // Extract Whisper's English translation
        std::string key = "\"english\":\"";
        auto pos = json_response.find(key);
        if (pos == std::string::npos) return json_response;

        auto value_start = pos + key.size();
        auto value_end = json_response.find('"', value_start);
        if (value_end == std::string::npos) return json_response;

        std::string english_text = json_response.substr(value_start, value_end - value_start);

        // Strip "english" field, append "translated"
        std::string out = json_response;
        out.erase(pos - 1, value_end + 1 - (pos - 1)); // -1 to include the leading comma

        auto close = out.rfind('}');
        if (close != std::string::npos) {
            out.insert(close, ",\"translated\":\"" + english_text + "\"");
        }

        return out;
    };

    // Audio handler: transcribe → validate → filter → respond
    Server server(port, [&pool, &filter](int client_id, const std::string& locale, const std::vector<uint8_t>& audio_data) -> std::string {
        std::string speaker = "Player" + std::to_string(client_id);

        TranscribeResult result = pool.transcribe(audio_data, locale);
        if (result.text.empty()) {
            return protocol::make_error("No speech detected");
        }

        if (!result.detected_language.empty() && result.detected_language != locale && result.detected_language != "en") {
            std::cout << "[" << speaker << "] Language mismatch: client says '" << locale
                      << "' but detected '" << result.detected_language << "', dropping" << std::endl;
            return "";
        }

        FilterResult fr = filter.filter(result.text);

        std::cout << "[" << speaker << " (" << locale << ")] " << fr.original;
        if (!fr.flagged_words.empty()) {
            std::cout << "  (flagged: ";
            for (size_t i = 0; i < fr.flagged_words.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << fr.flagged_words[i];
            }
            std::cout << ")";
        }
        std::cout << std::endl;

        return protocol::make_response(speaker, locale, fr.original, fr.flagged_words, fr.redacted, result.english_translation);
    }, translate_handler);

    std::cout << "[main] Starting voice server on port " << port
              << " with " << workers << " worker(s) (Whisper translate for cross-language)"
              << std::endl;
    server.run();

    return 0;
}
