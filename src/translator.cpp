#include "translator.h"

#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

Translator::Translator(const std::string& models_dir, const std::string& device)
    : device_(device) {

    // Scan for opus-mt-<src>-<tgt>/ directories
    if (!fs::exists(models_dir)) {
        std::cerr << "[translator] Models directory not found: " << models_dir << std::endl;
        return;
    }

    for (const auto& entry : fs::directory_iterator(models_dir)) {
        if (!entry.is_directory()) continue;

        std::string dirname = entry.path().filename().string();
        if (dirname.substr(0, 8) != "opus-mt-") continue;

        std::string pair_key = dirname.substr(8); // e.g. "en-ja"
        std::string model_path = entry.path().string();
        std::string model_bin = model_path + "/model.bin";

        if (!fs::exists(model_bin)) continue;

        // Find SentencePiece tokenizers (source + target)
        std::string source_sp = model_path + "/source.spm";
        std::string target_sp = model_path + "/target.spm";
        if (!fs::exists(source_sp) || !fs::exists(target_sp)) {
            std::cerr << "[translator] Missing source.spm or target.spm for " << pair_key << std::endl;
            continue;
        }

        try {
            auto pair = std::make_unique<TranslationPair>();

            pair->source_tokenizer = std::make_unique<sentencepiece::SentencePieceProcessor>();
            auto status = pair->source_tokenizer->Load(source_sp);
            if (!status.ok()) {
                std::cerr << "[translator] Failed to load source tokenizer for " << pair_key
                          << ": " << status.ToString() << std::endl;
                continue;
            }

            pair->target_tokenizer = std::make_unique<sentencepiece::SentencePieceProcessor>();
            status = pair->target_tokenizer->Load(target_sp);
            if (!status.ok()) {
                std::cerr << "[translator] Failed to load target tokenizer for " << pair_key
                          << ": " << status.ToString() << std::endl;
                continue;
            }

            ctranslate2::Device ct2_device = (device == "cuda")
                ? ctranslate2::Device::CUDA
                : ctranslate2::Device::CPU;

            pair->translator = std::make_unique<ctranslate2::Translator>(model_path, ct2_device);

            pairs_[pair_key] = std::move(pair);
            std::cout << "[translator] Loaded " << pair_key << " (" << device << ")" << std::endl;

        } catch (const std::exception& e) {
            std::cerr << "[translator] Failed to load " << pair_key << ": " << e.what() << std::endl;
        }
    }

    std::cout << "[translator] " << pairs_.size() << " translation pair(s) ready" << std::endl;
}

Translator::~Translator() = default;

std::string Translator::translate(const std::string& text,
                                  const std::string& source_lang,
                                  const std::string& target_lang) {
    if (source_lang == target_lang) return text;

    std::string pair_key = source_lang + "-" + target_lang;
    auto it = pairs_.find(pair_key);
    if (it == pairs_.end()) {
        return text; // No model for this pair, return as-is
    }

    auto& pair = *it->second;

    try {
        std::vector<std::string> tokens;
        pair.source_tokenizer->Encode(text, &tokens);

        ctranslate2::TranslationOptions options;
        options.beam_size = 5;
        options.max_decoding_length = static_cast<size_t>(tokens.size() * 3);
        options.repetition_penalty = 1.2f;
        options.no_repeat_ngram_size = 3;

        std::vector<ctranslate2::TranslationResult> results;
        {
            std::lock_guard<std::mutex> lock(pair.mtx);
            results = pair.translator->translate_batch({tokens}, {}, options);
        }

        if (results.empty() || results[0].output().empty()) {
            return text;
        }

        std::string translated;
        pair.target_tokenizer->Decode(results[0].output(), &translated);

        std::cout << "[translator] " << pair_key << ": \"" << text
                  << "\" -> \"" << translated << "\"" << std::endl;

        return translated;

    } catch (const std::exception& e) {
        std::cerr << "[translator] Translation failed: " << e.what() << std::endl;
        return text;
    }
}
