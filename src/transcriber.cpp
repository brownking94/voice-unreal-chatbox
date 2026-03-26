#include "transcriber.h"
#include "whisper.h"

#include <iostream>
#include <stdexcept>
#include <cstring>

Transcriber::Transcriber(const std::string& model_path) {
    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;
    ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx_) {
        throw std::runtime_error("Failed to load whisper model: " + model_path);
    }
    std::cout << "[transcriber] Model loaded: " << model_path << std::endl;
}

Transcriber::~Transcriber() {
    if (ctx_) {
        whisper_free(ctx_);
    }
}

std::string Transcriber::transcribe(const std::vector<uint8_t>& pcm16_bytes) {
    // Convert 16-bit signed PCM to float32 [-1.0, 1.0]
    size_t n_samples = pcm16_bytes.size() / 2;
    if (n_samples == 0) return "";

    std::vector<float> pcm_f32(n_samples);
    const int16_t* samples = reinterpret_cast<const int16_t*>(pcm16_bytes.data());
    for (size_t i = 0; i < n_samples; i++) {
        pcm_f32[i] = static_cast<float>(samples[i]) / 32768.0f;
    }

    // Configure whisper inference
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress   = false;
    params.print_special    = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.single_segment   = false;
    params.language         = "en";
    params.n_threads        = 4;

    int ret = whisper_full(ctx_, params, pcm_f32.data(), static_cast<int>(pcm_f32.size()));
    if (ret != 0) {
        std::cerr << "[transcriber] whisper_full failed with code " << ret << std::endl;
        return "";
    }

    // Collect all segment texts
    std::string result;
    int n_segments = whisper_full_n_segments(ctx_);
    for (int i = 0; i < n_segments; i++) {
        const char* text = whisper_full_get_segment_text(ctx_, i);
        if (text) {
            result += text;
        }
    }

    return result;
}
