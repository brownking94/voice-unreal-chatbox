#include "protocol.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

static void test_make_response_basic() {
    std::string json = protocol::make_response(
        "Player1", "en", "hello world", {}, "hello world");

    assert(json.find("\"speaker\":\"Player1\"") != std::string::npos);
    assert(json.find("\"locale\":\"en\"") != std::string::npos);
    assert(json.find("\"original\":\"hello world\"") != std::string::npos);
    assert(json.find("\"flagged_words\":[]") != std::string::npos);
    assert(json.find("\"redacted\":\"hello world\"") != std::string::npos);
    std::cout << "  PASS: test_make_response_basic" << std::endl;
}

static void test_make_response_with_flagged_words() {
    std::vector<std::string> flagged = {"bad", "ugly"};
    std::string json = protocol::make_response(
        "Player2", "ja", "some bad ugly text", flagged, "some *** **** text");

    assert(json.find("\"flagged_words\":[\"bad\",\"ugly\"]") != std::string::npos);
    assert(json.find("\"redacted\":\"some *** **** text\"") != std::string::npos);
    std::cout << "  PASS: test_make_response_with_flagged_words" << std::endl;
}

static void test_make_response_json_escaping() {
    std::string json = protocol::make_response(
        "Player1", "en", "he said \"hello\"", {}, "he said \"hello\"");

    // Quotes inside strings must be escaped
    assert(json.find("\\\"hello\\\"") != std::string::npos);
    std::cout << "  PASS: test_make_response_json_escaping" << std::endl;
}

static void test_make_response_newline_escaping() {
    std::string json = protocol::make_response(
        "Player1", "en", "line1\nline2", {}, "line1\nline2");

    assert(json.find("\\n") != std::string::npos);
    assert(json.find("\n") == std::string::npos);  // Raw newline should not appear
    std::cout << "  PASS: test_make_response_newline_escaping" << std::endl;
}

static void test_make_error() {
    std::string json = protocol::make_error("No speech detected");
    assert(json == "{\"error\":\"No speech detected\"}");
    std::cout << "  PASS: test_make_error" << std::endl;
}

static void test_make_error_with_special_chars() {
    std::string json = protocol::make_error("Failed: \"timeout\"");
    assert(json.find("\\\"timeout\\\"") != std::string::npos);
    std::cout << "  PASS: test_make_error_with_special_chars" << std::endl;
}

int main() {
    std::cout << "=== Protocol Tests ===" << std::endl;
    test_make_response_basic();
    test_make_response_with_flagged_words();
    test_make_response_json_escaping();
    test_make_response_newline_escaping();
    test_make_error();
    test_make_error_with_special_chars();
    std::cout << "All protocol tests passed!" << std::endl;
    return 0;
}
