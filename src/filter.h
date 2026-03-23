#pragma once

#include <string>
#include <unordered_set>
#include <vector>

struct FilterResult {
    std::string original;
    std::vector<std::string> flagged_words;
    std::string redacted;
};

class ProfanityFilter {
public:
    // Load bad words from a text file (one word/phrase per line).
    explicit ProfanityFilter(const std::string& word_list_path);

    // Filter the input text. Returns original, flagged words, and redacted version.
    FilterResult filter(const std::string& text) const;

    size_t word_count() const { return words_.size(); }

private:
    std::unordered_set<std::string> words_;        // Single-word entries (lowercased)
    std::vector<std::string>        phrases_;       // Multi-word entries (lowercased)

    static std::string to_lower(const std::string& s);
};
