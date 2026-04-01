#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct whisper_context;

struct TranscribeResult {
    std::string text;
    std::string detected_language;  // whisper language code (e.g. "en", "ja")
};

class Transcriber {
public:
    // Load the whisper model from disk. Throws on failure.
    explicit Transcriber(const std::string& model_path);
    ~Transcriber();

    Transcriber(const Transcriber&) = delete;
    Transcriber& operator=(const Transcriber&) = delete;

    // Transcribe raw 16-bit PCM audio (16 kHz, mono).
    // Language should be a whisper language code (e.g. "en", "ja") or "auto".
    // Returns transcribed text and detected language.
    TranscribeResult transcribe(const std::vector<uint8_t>& pcm16_bytes, const std::string& language = "auto");

private:
    whisper_context* ctx_ = nullptr;
};
