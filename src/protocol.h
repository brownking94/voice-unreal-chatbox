#pragma once

#include <string>

namespace protocol {

// Build a JSON chat message: {"speaker":"...","locale":"...","text":"..."}
std::string make_message(const std::string& speaker,
                         const std::string& locale,
                         const std::string& text);

// Build a JSON error response: {"error":"<message>"}
std::string make_error(const std::string& message);

}  // namespace protocol
