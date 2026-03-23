#include "filter.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

ProfanityFilter::ProfanityFilter(const std::string& word_list_path) {
    std::ifstream file(word_list_path);
    if (!file) {
        std::cerr << "[filter] Warning: could not open word list: " << word_list_path << std::endl;
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Trim whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        while (!line.empty() && line.front() == ' ') {
            line.erase(line.begin());
        }

        if (line.empty() || line[0] == '#') continue;  // Skip empty lines and comments

        std::string lower = to_lower(line);
        if (lower.find(' ') != std::string::npos) {
            phrases_.push_back(lower);
        } else {
            words_.insert(lower);
        }
    }

    std::cout << "[filter] Loaded " << words_.size() << " words and "
              << phrases_.size() << " phrases" << std::endl;
}

FilterResult ProfanityFilter::filter(const std::string& text) const {
    FilterResult result;
    result.original = text;
    result.redacted = text;

    std::string text_lower = to_lower(text);

    // Check multi-word phrases first
    for (const auto& phrase : phrases_) {
        size_t pos = 0;
        while ((pos = to_lower(result.redacted).find(phrase, pos)) != std::string::npos) {
            // Record the flagged phrase (original casing from input)
            std::string matched = result.redacted.substr(pos, phrase.size());
            result.flagged_words.push_back(matched);

            // Replace with asterisks
            std::string replacement(phrase.size(), '*');
            result.redacted.replace(pos, phrase.size(), replacement);
            pos += replacement.size();
        }
    }

    // Check single words using word boundaries
    // Walk through the text and extract words
    std::string redacted_lower = to_lower(result.redacted);
    size_t i = 0;
    while (i < result.redacted.size()) {
        // Skip non-alpha characters
        if (!std::isalpha(static_cast<unsigned char>(redacted_lower[i]))) {
            i++;
            continue;
        }

        // Find word boundaries
        size_t start = i;
        while (i < redacted_lower.size() && std::isalpha(static_cast<unsigned char>(redacted_lower[i]))) {
            i++;
        }

        std::string word = redacted_lower.substr(start, i - start);

        if (words_.count(word)) {
            std::string original_word = result.redacted.substr(start, i - start);
            result.flagged_words.push_back(original_word);

            // Replace with asterisks in redacted text
            std::string replacement(i - start, '*');
            result.redacted.replace(start, i - start, replacement);
            // Update lower copy too
            redacted_lower.replace(start, i - start, replacement);
        }
    }

    // Deduplicate flagged words
    std::sort(result.flagged_words.begin(), result.flagged_words.end());
    result.flagged_words.erase(
        std::unique(result.flagged_words.begin(), result.flagged_words.end()),
        result.flagged_words.end()
    );

    return result;
}

std::string ProfanityFilter::to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}
