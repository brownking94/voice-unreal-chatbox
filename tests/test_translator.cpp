#include "translator.h"

#include <cassert>
#include <iostream>
#include <string>

static void test_locale_map_english() {
    assert(Translator::to_nllb_code("en") == "eng_Latn");
    std::cout << "  PASS: test_locale_map_english" << std::endl;
}

static void test_locale_map_japanese() {
    assert(Translator::to_nllb_code("ja") == "jpn_Jpan");
    std::cout << "  PASS: test_locale_map_japanese" << std::endl;
}

static void test_locale_map_all_supported() {
    assert(!Translator::to_nllb_code("en").empty());
    assert(!Translator::to_nllb_code("zh").empty());
    assert(!Translator::to_nllb_code("es").empty());
    assert(!Translator::to_nllb_code("hi").empty());
    assert(!Translator::to_nllb_code("ar").empty());
    assert(!Translator::to_nllb_code("pt").empty());
    assert(!Translator::to_nllb_code("ja").empty());
    assert(!Translator::to_nllb_code("ko").empty());
    assert(!Translator::to_nllb_code("fr").empty());
    assert(!Translator::to_nllb_code("de").empty());
    assert(!Translator::to_nllb_code("ru").empty());
    assert(!Translator::to_nllb_code("it").empty());
    std::cout << "  PASS: test_locale_map_all_supported" << std::endl;
}

static void test_locale_map_unknown() {
    assert(Translator::to_nllb_code("xyz").empty());
    assert(Translator::to_nllb_code("").empty());
    std::cout << "  PASS: test_locale_map_unknown" << std::endl;
}

static void test_locale_map_unique_codes() {
    // All NLLB codes should be different
    std::vector<std::string> locales = {"en", "zh", "es", "hi", "ar", "pt", "ja", "ko", "fr", "de", "ru", "it"};
    std::unordered_set<std::string> codes;
    for (const auto& l : locales) {
        codes.insert(Translator::to_nllb_code(l));
    }
    assert(codes.size() == locales.size());
    std::cout << "  PASS: test_locale_map_unique_codes" << std::endl;
}

int main() {
    std::cout << "=== Translator Tests ===" << std::endl;
    test_locale_map_english();
    test_locale_map_japanese();
    test_locale_map_all_supported();
    test_locale_map_unknown();
    test_locale_map_unique_codes();
    std::cout << "All translator tests passed!" << std::endl;
    return 0;
}
