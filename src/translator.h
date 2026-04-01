#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Forward declarations — avoid pulling in heavy CTranslate2/SentencePiece headers
namespace ctranslate2 { class Translator; }
namespace sentencepiece { class SentencePieceProcessor; }

/**
 * Translates text between languages using NLLB-200 (via CTranslate2).
 * Thread-safe: multiple threads can call translate() concurrently.
 */
class Translator {
public:
    // model_dir: path to CTranslate2-converted NLLB model directory
    // sp_model_path: path to the SentencePiece .model file
    // device: "cuda" or "cpu"
    Translator(const std::string& model_dir,
               const std::string& sp_model_path,
               const std::string& device = "cuda");
    ~Translator();

    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    // Translate text from one language to another.
    // source_lang / target_lang are whisper-style codes (e.g. "en", "ja").
    // Returns translated text, or the original if translation fails.
    std::string translate(const std::string& text,
                          const std::string& source_lang,
                          const std::string& target_lang);

    // Returns true if the translator was loaded successfully.
    bool is_loaded() const { return loaded_; }

    // Convert whisper locale code (e.g. "en") to NLLB code (e.g. "eng_Latn").
    // Returns empty string if unknown.
    static std::string to_nllb_code(const std::string& whisper_code);

private:
    std::unique_ptr<ctranslate2::Translator> ct2_translator_;
    std::unique_ptr<sentencepiece::SentencePieceProcessor> sp_processor_;
    std::mutex mtx_;  // CTranslate2 Translator is not fully thread-safe with shared state
    bool loaded_ = false;

    // Whisper code → NLLB-200 code mapping
    static const std::unordered_map<std::string, std::string> locale_map_;
};
