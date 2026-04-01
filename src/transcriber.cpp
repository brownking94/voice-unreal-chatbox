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

TranscribeResult Transcriber::transcribe(const std::vector<uint8_t>& pcm16_bytes, const std::string& language) {
    // Convert 16-bit signed PCM to float32 [-1.0, 1.0]
    size_t n_samples = pcm16_bytes.size() / 2;
    if (n_samples == 0) return {"", ""};

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
    params.language         = "auto";
    params.n_threads        = 4;

    int ret = whisper_full(ctx_, params, pcm_f32.data(), static_cast<int>(pcm_f32.size()));
    if (ret != 0) {
        std::cerr << "[transcriber] whisper_full failed with code " << ret << std::endl;
        return {"", ""};
    }

    // Get detected language
    int lang_id = whisper_full_lang_id(ctx_);
    const char* lang = whisper_lang_str(lang_id);
    std::string detected = lang ? lang : "";
    std::cout << "[transcriber] Detected language: " << detected << std::endl;

    // Collect all segment texts
    std::string result;
    int n_segments = whisper_full_n_segments(ctx_);
    for (int i = 0; i < n_segments; i++) {
        const char* text = whisper_full_get_segment_text(ctx_, i);
        if (text) {
            result += text;
        }
    }

    // If source language is not English, run a second pass with translate=true
    // to get a high-quality English translation directly from Whisper
    std::string english_translation;
    if (!detected.empty() && detected != "en" && !result.empty()) {
        whisper_full_params tparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        tparams.print_progress   = false;
        tparams.print_special    = false;
        tparams.print_realtime   = false;
        tparams.print_timestamps = false;
        tparams.single_segment   = false;
        tparams.language         = detected.c_str();
        tparams.translate        = true;
        tparams.n_threads        = 4;

        int tret = whisper_full(ctx_, tparams, pcm_f32.data(), static_cast<int>(pcm_f32.size()));
        if (tret == 0) {
            int tn_segments = whisper_full_n_segments(ctx_);
            for (int i = 0; i < tn_segments; i++) {
                const char* text = whisper_full_get_segment_text(ctx_, i);
                if (text) {
                    english_translation += text;
                }
            }
            std::cout << "[transcriber] Whisper translate: " << english_translation << std::endl;
        }
    }

    return {result, detected, english_translation};
}
