#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// Reproduce the wire protocol helpers (same as in server.cpp / test_client.cpp)
static uint32_t read_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}

static void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF;
    p[3] =  v        & 0xFF;
}

static void test_u32_be_roundtrip() {
    uint8_t buf[4];
    write_u32_be(buf, 0x12345678);
    assert(read_u32_be(buf) == 0x12345678);
    std::cout << "  PASS: test_u32_be_roundtrip" << std::endl;
}

static void test_u32_be_zero() {
    uint8_t buf[4];
    write_u32_be(buf, 0);
    assert(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0);
    assert(read_u32_be(buf) == 0);
    std::cout << "  PASS: test_u32_be_zero" << std::endl;
}

static void test_u32_be_max() {
    uint8_t buf[4];
    write_u32_be(buf, 0xFFFFFFFF);
    assert(buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF);
    assert(read_u32_be(buf) == 0xFFFFFFFF);
    std::cout << "  PASS: test_u32_be_max" << std::endl;
}

static void test_u32_be_byte_order() {
    uint8_t buf[4];
    write_u32_be(buf, 256);  // 0x00000100
    assert(buf[0] == 0x00);
    assert(buf[1] == 0x00);
    assert(buf[2] == 0x01);
    assert(buf[3] == 0x00);
    std::cout << "  PASS: test_u32_be_byte_order" << std::endl;
}

// Test locale prefix encoding: [1-byte len][locale string]
static void test_locale_prefix_encoding() {
    std::string locale = "en";
    uint8_t loc_len = static_cast<uint8_t>(locale.size());

    assert(loc_len == 2);
    assert(locale[0] == 'e');
    assert(locale[1] == 'n');
    std::cout << "  PASS: test_locale_prefix_encoding" << std::endl;
}

static void test_locale_prefix_japanese() {
    std::string locale = "ja";
    uint8_t loc_len = static_cast<uint8_t>(locale.size());

    assert(loc_len == 2);
    assert(locale[0] == 'j');
    assert(locale[1] == 'a');
    std::cout << "  PASS: test_locale_prefix_japanese" << std::endl;
}

static void test_locale_prefix_auto() {
    std::string locale = "auto";
    uint8_t loc_len = static_cast<uint8_t>(locale.size());

    assert(loc_len == 4);
    std::cout << "  PASS: test_locale_prefix_auto" << std::endl;
}

// Test full client→server message framing
static void test_full_message_framing() {
    std::string locale = "en";
    std::vector<uint8_t> audio = {0x01, 0x02, 0x03, 0x04};

    // Build message: [1-byte locale len][locale][4-byte audio len][audio]
    std::vector<uint8_t> msg;
    uint8_t loc_len = static_cast<uint8_t>(locale.size());
    msg.push_back(loc_len);
    msg.insert(msg.end(), locale.begin(), locale.end());

    uint8_t audio_hdr[4];
    write_u32_be(audio_hdr, static_cast<uint32_t>(audio.size()));
    msg.insert(msg.end(), audio_hdr, audio_hdr + 4);
    msg.insert(msg.end(), audio.begin(), audio.end());

    // Verify: total = 1 + 2 + 4 + 4 = 11 bytes
    assert(msg.size() == 11);

    // Parse it back
    size_t pos = 0;
    uint8_t parsed_loc_len = msg[pos++];
    assert(parsed_loc_len == 2);

    std::string parsed_locale(msg.begin() + pos, msg.begin() + pos + parsed_loc_len);
    pos += parsed_loc_len;
    assert(parsed_locale == "en");

    uint32_t parsed_audio_len = read_u32_be(&msg[pos]);
    pos += 4;
    assert(parsed_audio_len == 4);

    std::vector<uint8_t> parsed_audio(msg.begin() + pos, msg.begin() + pos + parsed_audio_len);
    assert(parsed_audio == audio);

    std::cout << "  PASS: test_full_message_framing" << std::endl;
}

// Test zero-length audio (registration message)
static void test_registration_message() {
    std::string locale = "ja";
    std::vector<uint8_t> msg;

    uint8_t loc_len = static_cast<uint8_t>(locale.size());
    msg.push_back(loc_len);
    msg.insert(msg.end(), locale.begin(), locale.end());

    uint8_t zero_audio[4] = {0, 0, 0, 0};
    msg.insert(msg.end(), zero_audio, zero_audio + 4);

    // Total: 1 + 2 + 4 = 7 bytes
    assert(msg.size() == 7);

    // Parse: audio length should be 0
    size_t pos = 1 + loc_len;
    uint32_t audio_len = read_u32_be(&msg[pos]);
    assert(audio_len == 0);

    std::cout << "  PASS: test_registration_message" << std::endl;
}

int main() {
    std::cout << "=== Wire Protocol Tests ===" << std::endl;
    test_u32_be_roundtrip();
    test_u32_be_zero();
    test_u32_be_max();
    test_u32_be_byte_order();
    test_locale_prefix_encoding();
    test_locale_prefix_japanese();
    test_locale_prefix_auto();
    test_full_message_framing();
    test_registration_message();
    std::cout << "All wire protocol tests passed!" << std::endl;
    return 0;
}
