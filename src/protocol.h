#pragma once

#include <string>

namespace protocol {

// Build a JSON success response: {"speaker":"<speaker>","text":"<text>"}
std::string make_response(const std::string& speaker, const std::string& text);

// Build a JSON error response: {"error":"<message>"}
std::string make_error(const std::string& message);

}  // namespace protocol
