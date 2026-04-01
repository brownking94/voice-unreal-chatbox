#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ctranslate2 { class Translator; }
namespace sentencepiece { class SentencePieceProcessor; }

/**
 * Translates text between language pairs using Opus-MT models (via CTranslate2).
 * Each language pair (e.g. en-ja, ja-en) has its own dedicated model for high quality.
 * Thread-safe: multiple threads can call translate() concurrently.
 */
class Translator {
public:
    // models_dir: directory containing opus-mt-<pair>/ subdirectories
    // device: "cuda" or "cpu"
    Translator(const std::string& models_dir, const std::string& device = "cuda");
    ~Translator();

    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    // Translate text from source_lang to target_lang (whisper-style codes: "en", "ja").
    // Returns translated text, or original if no model available for the pair.
    std::string translate(const std::string& text,
                          const std::string& source_lang,
                          const std::string& target_lang);

    bool is_loaded() const { return !pairs_.empty(); }
    int pair_count() const { return static_cast<int>(pairs_.size()); }

private:
    struct TranslationPair {
        std::unique_ptr<ctranslate2::Translator> translator;
        std::unique_ptr<sentencepiece::SentencePieceProcessor> source_tokenizer;
        std::unique_ptr<sentencepiece::SentencePieceProcessor> target_tokenizer;
        std::mutex mtx;
    };

    std::unordered_map<std::string, std::unique_ptr<TranslationPair>> pairs_;
    std::string device_;
};
