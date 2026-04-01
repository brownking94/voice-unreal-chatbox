#include "server.h"
#include "transcriber_pool.h"
#include "translator.h"
#include "filter.h"
#include "protocol.h"

#include <iostream>
#include <string>
#include <cstdlib>
#include <memory>

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
    std::cerr << "  --models-dir   Directory with opus-mt-* translation models (default: models)" << std::endl;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::string model_path = "models/ggml-small.bin";
    std::string filter_path = "config/profanity.txt";
    std::string models_dir = "models";
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
        } else if (arg == "--models-dir" && i + 1 < argc) {
            models_dir = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (workers < 1) workers = 1;

    TranscriberPool pool(model_path, workers);
    ProfanityFilter filter(filter_path);

    // Load translation models (scans models/ for opus-mt-* directories)
    auto translator = std::make_unique<Translator>(models_dir, "cuda");

    // Translation handler for broadcast
    // Adds a "translated" field — never touches "redacted"
    TranslateHandler translate_handler = [&translator](
        const std::string& json_response,
        const std::string& source_lang,
        const std::string& target_lang) -> std::string {

        std::string translated_text;

        if (target_lang == "en") {
            // Use Whisper's built-in translate if available (high quality)
            std::string key = "\"english\":\"";
            auto pos = json_response.find(key);
            if (pos != std::string::npos) {
                auto value_start = pos + key.size();
                auto value_end = json_response.find('"', value_start);
                if (value_end != std::string::npos) {
                    translated_text = json_response.substr(value_start, value_end - value_start);
                }
            }
        }

        // Fall back to Opus-MT for non-English targets or if no Whisper translation
        if (translated_text.empty() && translator && translator->is_loaded()) {
            std::string key = "\"redacted\":\"";
            auto pos = json_response.find(key);
            if (pos != std::string::npos) {
                auto value_start = pos + key.size();
                auto value_end = json_response.find('"', value_start);
                if (value_end != std::string::npos) {
                    std::string text = json_response.substr(value_start, value_end - value_start);
                    translated_text = translator->translate(text, source_lang, target_lang);
                }
            }
        }

        if (translated_text.empty()) return json_response;

        // Strip the internal "english" field and append "translated"
        std::string out = json_response;

        // Remove "english" field if present
        std::string ekey = ",\"english\":\"";
        auto epos = out.find(ekey);
        if (epos != std::string::npos) {
            auto evalue_start = epos + ekey.size();
            auto evalue_end = out.find('"', evalue_start);
            if (evalue_end != std::string::npos) {
                out.erase(epos, evalue_end + 1 - epos);
            }
        }

        // Insert "translated" before the closing }
        auto close = out.rfind('}');
        if (close != std::string::npos) {
            out.insert(close, ",\"translated\":\"" + translated_text + "\"");
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

        if (!result.detected_language.empty() && result.detected_language != locale) {
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
              << " with " << workers << " worker(s)";
    if (translator->is_loaded()) std::cout << " + " << translator->pair_count() << " translation pair(s)";
    std::cout << std::endl;
    server.run();

    return 0;
}
