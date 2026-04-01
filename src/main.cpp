#include "server.h"
#include "transcriber_pool.h"
#include "translator.h"
#include "filter.h"
#include "protocol.h"

#include <iostream>
#include <string>
#include <cstdlib>
#include <memory>

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " -m <model_path> [-p <port>] [-w <workers>] [-f <filter_path>] [-t <translation_model_dir>] [-s <sp_model_path>]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  -m <path>      Path to whisper model file (e.g. models/ggml-medium.en.bin)" << std::endl;
    std::cerr << "  -p <port>      TCP port to listen on (default: 9090)" << std::endl;
    std::cerr << "  -w <workers>   Number of model instances for parallel transcription (default: 2)" << std::endl;
    std::cerr << "  -f <path>      Path to profanity word list (default: config/profanity.txt)" << std::endl;
    std::cerr << "  -t <path>      Path to CTranslate2 NLLB translation model directory (enables translation)" << std::endl;
    std::cerr << "  -s <path>      Path to SentencePiece .model file for NLLB (required with -t)" << std::endl;
}

// Parse the "locale" field from a JSON string (simple extraction, no full parser needed)
static std::string extract_json_field(const std::string& json, const std::string& field) {
    std::string key = "\"" + field + "\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Replace a JSON field's string value
static std::string replace_json_field(const std::string& json, const std::string& field, const std::string& new_value) {
    std::string key = "\"" + field + "\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return json;
    auto value_start = pos + key.size();
    auto value_end = json.find('"', value_start);
    if (value_end == std::string::npos) return json;
    return json.substr(0, value_start) + new_value + json.substr(value_end);
}

int main(int argc, char* argv[]) {
    std::string model_path = "models/ggml-small.bin";
    std::string filter_path = "config/profanity.txt";
    std::string translation_model_dir = "models/nllb-200-distilled-600M";
    std::string sp_model_path = "models/nllb-200-distilled-600M/sentencepiece.bpe.model";
    uint16_t port = 9090;
    int workers = 2;

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
        } else if (arg == "-t" && i + 1 < argc) {
            translation_model_dir = argv[++i];
        } else if (arg == "-s" && i + 1 < argc) {
            sp_model_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    // All paths have sensible defaults — no required flags

    if (workers < 1) workers = 1;

    // Load the transcriber pool and profanity filter
    TranscriberPool pool(model_path, workers);
    ProfanityFilter filter(filter_path);

    // Optionally load the translation model
    std::unique_ptr<Translator> translator;
    if (!translation_model_dir.empty() && !sp_model_path.empty()) {
        translator = std::make_unique<Translator>(translation_model_dir, sp_model_path, "cuda");
        if (!translator->is_loaded()) {
            std::cerr << "[main] Translation model failed to load, continuing without translation" << std::endl;
            translator.reset();
        }
    } else if (!translation_model_dir.empty() || !sp_model_path.empty()) {
        std::cerr << "[main] Both -t (model dir) and -s (sentencepiece model) are required for translation" << std::endl;
    }

    // Translate handler for broadcast: takes JSON response, translates the text fields
    TranslateHandler translate_handler = nullptr;
    if (translator) {
        translate_handler = [&translator](const std::string& json_response,
                                          const std::string& source_lang,
                                          const std::string& target_lang) -> std::string {
            // Extract the redacted text and translate it
            std::string redacted = extract_json_field(json_response, "redacted");
            if (redacted.empty()) return json_response;

            std::string translated = translator->translate(redacted, source_lang, target_lang);

            // Build new JSON with translated text, keeping original fields
            std::string result = json_response;
            result = replace_json_field(result, "redacted", translated);
            return result;
        };
    }

    // Handler: transcribe using client's language → validate → filter → respond
    Server server(port, [&pool, &filter](int client_id, const std::string& locale, const std::vector<uint8_t>& audio_data) -> std::string {
        std::string speaker = "Player" + std::to_string(client_id);

        TranscribeResult result = pool.transcribe(audio_data, locale);
        if (result.text.empty()) {
            return protocol::make_error("No speech detected");
        }

        // Drop if detected language doesn't match what the client claimed.
        // This catches cases like selecting Japanese but speaking English —
        // Whisper forced to Japanese would produce garbled output.
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

        return protocol::make_response(speaker, locale, fr.original, fr.flagged_words, fr.redacted);
    }, translate_handler);

    std::cout << "[main] Starting voice server on port " << port
              << " with " << workers << " worker(s)";
    if (translator) std::cout << " + NLLB translation";
    std::cout << std::endl;
    server.run();

    return 0;
}
