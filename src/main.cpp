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

// Internal response from the audio handler — not sent to clients directly.
// The server picks the right text per-listener before sending.
struct InternalResult {
    std::string speaker;
    std::string detected_lang;   // what Whisper actually detected
    std::string original_text;   // profanity-filtered text in speaker's language
    std::string english_text;    // Whisper translate output (empty if already English)
};

// Extract a JSON string field value (simple, no nested quotes)
static std::string json_get(const std::string& json, const std::string& field) {
    std::string key = "\"" + field + "\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return "";
    auto start = pos + key.size();
    auto end = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
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
    // Server picks the right text: same language → original, different → English
    TranslateHandler translate_handler = [](
        const std::string& json_response,
        const std::string& source_lang,
        const std::string& target_lang) -> std::string {

        // The internal JSON has both "text" (original) and "_english" (translation)
        // Replace "text" with the English translation for cross-language listeners
        std::string english = json_get(json_response, "_english");
        if (english.empty()) return json_response;

        // Build a clean message with English text, strip _english field
        std::string speaker = json_get(json_response, "speaker");
        std::string locale = json_get(json_response, "locale");
        return protocol::make_message(speaker, locale, english);
    };

    // Audio handler: transcribe → filter → respond
    Server server(port, [&pool, &filter](int client_id, const std::string& locale, const std::vector<uint8_t>& audio_data) -> std::string {
        std::string speaker = "Player" + std::to_string(client_id);

        TranscribeResult result = pool.transcribe(audio_data, locale);
        if (result.text.empty()) {
            return "";  // silently ignore — no speech detected
        }

        // Use detected language for routing
        std::string source_lang = result.detected_language.empty() ? locale : result.detected_language;

        if (source_lang != locale) {
            std::cout << "[" << speaker << "] Detected '" << source_lang
                      << "' (client locale: " << locale << ")" << std::endl;
        }

        FilterResult fr = filter.filter(result.text);

        std::cout << "[" << speaker << " (" << source_lang << ")] " << fr.redacted;
        if (!fr.flagged_words.empty()) {
            std::cout << "  (flagged: ";
            for (size_t i = 0; i < fr.flagged_words.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << fr.flagged_words[i];
            }
            std::cout << ")";
        }
        std::cout << std::endl;

        // Build response — attach _english internally for broadcast routing
        std::string json = protocol::make_message(speaker, source_lang, fr.redacted);

        if (!result.english_translation.empty()) {
            std::string english_msg = protocol::make_message(speaker, source_lang, result.english_translation);
            std::string ekey = ",\"_english\":\"" ;
            // Borrow json_escape logic: make_message already escapes, so extract text back
            std::string escaped;
            for (char c : result.english_translation) {
                switch (c) {
                    case '"':  escaped += "\\\""; break;
                    case '\\': escaped += "\\\\"; break;
                    case '\n': escaped += "\\n";  break;
                    default:   escaped += c;      break;
                }
            }
            json.insert(json.rfind('}'), ",\"_english\":\"" + escaped + "\"");
        }

        return json;
    }, translate_handler);

    std::cout << "[main] Starting voice server on port " << port
              << " with " << workers << " worker(s) (Whisper translate for cross-language)"
              << std::endl;
    server.run();

    return 0;
}
