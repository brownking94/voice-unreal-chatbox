#include "protocol.h"

namespace protocol {

// Escape special JSON characters in a string
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string make_response(const std::string& speaker,
                          const std::string& locale,
                          const std::string& original,
                          const std::vector<std::string>& flagged_words,
                          const std::string& redacted,
                          const std::string& english_translation) {
    std::string json = "{\"speaker\":\"" + json_escape(speaker) +
                       "\",\"locale\":\"" + json_escape(locale) +
                       "\",\"original\":\"" + json_escape(original) +
                       "\",\"flagged_words\":[";

    for (size_t i = 0; i < flagged_words.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + json_escape(flagged_words[i]) + "\"";
    }

    json += "],\"redacted\":\"" + json_escape(redacted) + "\"";

    if (!english_translation.empty()) {
        json += ",\"english\":\"" + json_escape(english_translation) + "\"";
    }

    json += "}";
    return json;
}

std::string make_error(const std::string& message) {
    return "{\"error\":\"" + json_escape(message) + "\"}";
}

}  // namespace protocol
