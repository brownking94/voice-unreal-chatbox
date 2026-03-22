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

std::string make_response(const std::string& speaker, const std::string& text) {
    return "{\"speaker\":\"" + json_escape(speaker) +
           "\",\"text\":\"" + json_escape(text) + "\"}";
}

std::string make_error(const std::string& message) {
    return "{\"error\":\"" + json_escape(message) + "\"}";
}

}  // namespace protocol
