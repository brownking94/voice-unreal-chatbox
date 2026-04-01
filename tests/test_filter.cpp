#include "filter.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

// Helper: create a temporary word list file and return its path
static std::string create_temp_wordlist(const std::vector<std::string>& words) {
    std::string path = "test_wordlist.txt";
    std::ofstream f(path);
    for (const auto& w : words) {
        f << w << "\n";
    }
    f.close();
    return path;
}

static void test_single_word_redaction() {
    auto path = create_temp_wordlist({"damn", "hell"});
    ProfanityFilter filter(path);

    FilterResult r = filter.filter("what the damn hell");
    assert(r.flagged_words.size() == 2);
    assert(r.redacted.find("damn") == std::string::npos);
    assert(r.redacted.find("hell") == std::string::npos);
    assert(r.redacted.find("****") != std::string::npos);
    std::cout << "  PASS: test_single_word_redaction" << std::endl;
}

static void test_case_insensitive() {
    auto path = create_temp_wordlist({"badword"});
    ProfanityFilter filter(path);

    FilterResult r = filter.filter("This BADWORD is here");
    assert(r.flagged_words.size() == 1);
    assert(r.redacted.find("BADWORD") == std::string::npos);
    std::cout << "  PASS: test_case_insensitive" << std::endl;
}

static void test_clean_text_passes_through() {
    auto path = create_temp_wordlist({"profanity"});
    ProfanityFilter filter(path);

    FilterResult r = filter.filter("This is perfectly clean text");
    assert(r.flagged_words.empty());
    assert(r.redacted == r.original);
    std::cout << "  PASS: test_clean_text_passes_through" << std::endl;
}

static void test_empty_text() {
    auto path = create_temp_wordlist({"badword"});
    ProfanityFilter filter(path);

    FilterResult r = filter.filter("");
    assert(r.flagged_words.empty());
    assert(r.redacted.empty());
    std::cout << "  PASS: test_empty_text" << std::endl;
}

static void test_word_count() {
    auto path = create_temp_wordlist({"one", "two", "three"});
    ProfanityFilter filter(path);

    assert(filter.word_count() == 3);
    std::cout << "  PASS: test_word_count" << std::endl;
}

static void test_original_preserved() {
    auto path = create_temp_wordlist({"bad"});
    ProfanityFilter filter(path);

    FilterResult r = filter.filter("this is bad");
    assert(r.original == "this is bad");
    assert(r.redacted != r.original);
    std::cout << "  PASS: test_original_preserved" << std::endl;
}

static void cleanup() {
    std::remove("test_wordlist.txt");
}

int main() {
    std::cout << "=== Filter Tests ===" << std::endl;
    test_single_word_redaction();
    test_case_insensitive();
    test_clean_text_passes_through();
    test_empty_text();
    test_word_count();
    test_original_preserved();
    cleanup();
    std::cout << "All filter tests passed!" << std::endl;
    return 0;
}
