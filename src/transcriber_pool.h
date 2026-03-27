#pragma once

#include "transcriber.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

class TranscriberPool {
public:
    // Load `pool_size` whisper model instances from the same model file.
    TranscriberPool(const std::string& model_path, int pool_size);

    // Borrow a transcriber, transcribe, and return it automatically.
    // Blocks if all instances are in use.
    std::string transcribe(const std::vector<uint8_t>& pcm16_bytes, const std::string& language = "auto");

private:
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<Transcriber>> all_;      // Owns all instances
    std::queue<Transcriber*>                  available_; // Free instances
};
