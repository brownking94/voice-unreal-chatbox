#include "translator.h"

#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>

#include <iostream>
#include <sstream>

const std::unordered_map<std::string, std::string> Translator::locale_map_ = {
    {"en", "eng_Latn"}, {"zh", "zho_Hans"}, {"es", "spa_Latn"},
    {"hi", "hin_Deva"}, {"ar", "arb_Arab"}, {"pt", "por_Latn"},
    {"ja", "jpn_Jpan"}, {"ko", "kor_Hang"}, {"fr", "fra_Latn"},
    {"de", "deu_Latn"}, {"ru", "rus_Cyrl"}, {"it", "ita_Latn"},
};

std::string Translator::to_nllb_code(const std::string& whisper_code) {
    auto it = locale_map_.find(whisper_code);
    return it != locale_map_.end() ? it->second : "";
}

Translator::Translator(const std::string& model_dir,
                       const std::string& sp_model_path,
                       const std::string& device) {
    try {
        // Load SentencePiece tokenizer
        sp_processor_ = std::make_unique<sentencepiece::SentencePieceProcessor>();
        auto status = sp_processor_->Load(sp_model_path);
        if (!status.ok()) {
            std::cerr << "[translator] Failed to load SentencePiece model: "
                      << status.ToString() << std::endl;
            return;
        }
        std::cout << "[translator] SentencePiece model loaded: " << sp_model_path << std::endl;

        // Load CTranslate2 NLLB model
        ctranslate2::Device ct2_device = (device == "cuda")
            ? ctranslate2::Device::CUDA
            : ctranslate2::Device::CPU;

        ct2_translator_ = std::make_unique<ctranslate2::Translator>(
            model_dir, ct2_device);

        loaded_ = true;
        std::cout << "[translator] NLLB model loaded: " << model_dir
                  << " (device: " << device << ")" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[translator] Failed to initialize: " << e.what() << std::endl;
    }
}

Translator::~Translator() = default;

std::string Translator::translate(const std::string& text,
                                  const std::string& source_lang,
                                  const std::string& target_lang) {
    if (!loaded_ || source_lang == target_lang) return text;

    std::string src_nllb = to_nllb_code(source_lang);
    std::string tgt_nllb = to_nllb_code(target_lang);
    if (src_nllb.empty() || tgt_nllb.empty()) {
        std::cerr << "[translator] Unknown language pair: "
                  << source_lang << " -> " << target_lang << std::endl;
        return text;
    }

    try {
        // Tokenize with SentencePiece
        std::vector<std::string> tokens;
        sp_processor_->Encode(text, &tokens);

        // Prepend source language token (NLLB format)
        tokens.insert(tokens.begin(), src_nllb);

        // Target prefix: the target language token
        std::vector<std::string> target_prefix = {tgt_nllb};

        // Translate
        ctranslate2::TranslationOptions options;
        options.beam_size = 4;
        options.max_decoding_length = 256;

        std::vector<std::vector<std::string>> batch_input = {tokens};
        std::vector<std::vector<std::string>> batch_target = {target_prefix};

        std::vector<ctranslate2::TranslationResult> results;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            results = ct2_translator_->translate_batch(
                batch_input, batch_target, options);
        }

        if (results.empty() || results[0].output().empty()) {
            return text;
        }

        // Detokenize — skip the target language token
        const auto& output_tokens = results[0].output();
        std::vector<std::string> text_tokens;
        for (size_t i = 0; i < output_tokens.size(); i++) {
            if (output_tokens[i] != tgt_nllb && output_tokens[i] != "</s>") {
                text_tokens.push_back(output_tokens[i]);
            }
        }

        std::string translated;
        sp_processor_->Decode(text_tokens, &translated);

        std::cout << "[translator] " << source_lang << " -> " << target_lang
                  << ": \"" << text << "\" -> \"" << translated << "\"" << std::endl;

        return translated;

    } catch (const std::exception& e) {
        std::cerr << "[translator] Translation failed: " << e.what() << std::endl;
        return text;
    }
}
