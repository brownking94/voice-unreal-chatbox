#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct whisper_context;

class Transcriber {
public:
    // Load the whisper model from disk. Throws on failure.
    explicit Transcriber(const std::string& model_path);
    ~Transcriber();

    Transcriber(const Transcriber&) = delete;
    Transcriber& operator=(const Transcriber&) = delete;

    // Transcribe raw 16-bit PCM audio (16 kHz, mono).
    // Returns the transcribed text (empty string if nothing detected).
    std::string transcribe(const std::vector<uint8_t>& pcm16_bytes);

private:
    whisper_context* ctx_ = nullptr;
};
