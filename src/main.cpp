#include "server.h"
#include "transcriber_pool.h"
#include "protocol.h"

#include <iostream>
#include <string>
#include <cstdlib>

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " -m <model_path> [-p <port>] [-w <workers>]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  -m <path>      Path to whisper model file (e.g. models/ggml-medium.en.bin)" << std::endl;
    std::cerr << "  -p <port>      TCP port to listen on (default: 9090)" << std::endl;
    std::cerr << "  -w <workers>   Number of model instances for parallel transcription (default: 2)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string model_path;
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

    // Load the transcriber pool (all model instances loaded upfront)
    TranscriberPool pool(model_path, workers);

    // Handler borrows a model instance from the pool, transcribes, returns it
    Server server(port, [&pool](const std::vector<uint8_t>& audio_data) -> std::string {
        std::string text = pool.transcribe(audio_data);
        if (text.empty()) {
            return protocol::make_error("No speech detected");
        }
        std::cout << "[main] Transcription: " << text << std::endl;
        return protocol::make_response("Player1", text);
    });

    std::cout << "[main] Starting voice server on port " << port
              << " with " << workers << " worker(s)" << std::endl;
    server.run();

    return 0;
}
