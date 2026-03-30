#include "server.h"
#include "transcriber_pool.h"
#include "filter.h"
#include "protocol.h"

#include <iostream>
#include <string>
#include <cstdlib>

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " -m <model_path> [-p <port>] [-w <workers>] [-f <filter_path>]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  -m <path>      Path to whisper model file (e.g. models/ggml-medium.en.bin)" << std::endl;
    std::cerr << "  -p <port>      TCP port to listen on (default: 9090)" << std::endl;
    std::cerr << "  -w <workers>   Number of model instances for parallel transcription (default: 2)" << std::endl;
    std::cerr << "  -f <path>      Path to profanity word list (default: config/profanity.txt)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string model_path;
    std::string filter_path = "config/profanity.txt";
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
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (model_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    if (workers < 1) workers = 1;

    // Load the transcriber pool and profanity filter
    TranscriberPool pool(model_path, workers);
    ProfanityFilter filter(filter_path);

    // Handler: transcribe → filter → respond
    Server server(port, [&pool, &filter](int client_id, const std::string& locale, const std::vector<uint8_t>& audio_data) -> std::string {
        std::string speaker = "Player" + std::to_string(client_id);
        std::string text = pool.transcribe(audio_data, locale);
        if (text.empty()) {
            return protocol::make_error("No speech detected");
        }

        FilterResult fr = filter.filter(text);

        std::cout << "[" << speaker << "] " << fr.original;
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
    });

    std::cout << "[main] Starting voice server on port " << port
              << " with " << workers << " worker(s)" << std::endl;
    server.run();

    return 0;
}
