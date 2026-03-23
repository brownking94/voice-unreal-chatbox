#pragma once

#include <string>
#include <vector>

namespace protocol {

// Build a JSON success response with profanity filter results
std::string make_response(const std::string& speaker,
                          const std::string& original,
                          const std::vector<std::string>& flagged_words,
                          const std::string& redacted);

// Build a JSON error response: {"error":"<message>"}
std::string make_error(const std::string& message);

}  // namespace protocol
