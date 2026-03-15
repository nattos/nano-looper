#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "plugin/utf8.h"

// --- Decode ---

TEST_CASE("Decode ASCII", "[utf8]") {
  const char* s = "Hello";
  REQUIRE(utf8::decode(s) == 'H');
  REQUIRE(utf8::decode(s) == 'e');
  REQUIRE(utf8::decode(s) == 'l');
  REQUIRE(utf8::decode(s) == 'l');
  REQUIRE(utf8::decode(s) == 'o');
  REQUIRE(*s == '\0');
}

TEST_CASE("Decode 2-byte (Latin)", "[utf8]") {
  const char* s = "\xC3\xA9"; // é (U+00E9)
  REQUIRE(utf8::decode(s) == 0x00E9);
  REQUIRE(*s == '\0');
}

TEST_CASE("Decode 3-byte (CJK)", "[utf8]") {
  const char* s = "\xE4\xB8\x96"; // 世 (U+4E16)
  REQUIRE(utf8::decode(s) == 0x4E16);
  REQUIRE(*s == '\0');
}

TEST_CASE("Decode 4-byte (Emoji)", "[utf8]") {
  const char* s = "\xF0\x9F\x8E\xB5"; // 🎵 (U+1F3B5)
  REQUIRE(utf8::decode(s) == 0x1F3B5);
  REQUIRE(*s == '\0');
}

TEST_CASE("Decode mixed ASCII and multibyte", "[utf8]") {
  const char* s = "A\xC3\xA9\xE4\xB8\x96Z";
  REQUIRE(utf8::decode(s) == 'A');
  REQUIRE(utf8::decode(s) == 0x00E9);
  REQUIRE(utf8::decode(s) == 0x4E16);
  REQUIRE(utf8::decode(s) == 'Z');
}

TEST_CASE("Decode invalid lead byte returns U+FFFD", "[utf8]") {
  const char* s = "\xFF" "A";
  REQUIRE(utf8::decode(s) == 0xFFFD);
  REQUIRE(utf8::decode(s) == 'A');
}

TEST_CASE("Decode truncated 2-byte returns U+FFFD", "[utf8]") {
  // 0xC3 expects a continuation byte, but next is 'A' (not 10xxxxxx)
  const char* s = "\xC3" "A";
  REQUIRE(utf8::decode(s) == 0xFFFD);
}

TEST_CASE("Decode overlong 2-byte returns U+FFFD", "[utf8]") {
  // 0xC0 0x80 = overlong encoding of U+0000
  const char* s = "\xC0\x80";
  REQUIRE(utf8::decode(s) == 0xFFFD);
}

// --- Length ---

TEST_CASE("Length of ASCII string", "[utf8]") {
  REQUIRE(utf8::length("Hello") == 5);
}

TEST_CASE("Length of empty string", "[utf8]") {
  REQUIRE(utf8::length("") == 0);
}

TEST_CASE("Length of multibyte string", "[utf8]") {
  // "Aé世🎵" = 4 codepoints
  REQUIRE(utf8::length("A\xC3\xA9\xE4\xB8\x96\xF0\x9F\x8E\xB5") == 4);
}

TEST_CASE("Length counts replacement chars for invalid bytes", "[utf8]") {
  REQUIRE(utf8::length("\xFF\xFF") == 2);
}

// --- Encode ---

TEST_CASE("Encode ASCII", "[utf8]") {
  char buf[4];
  int n = utf8::encode('A', buf);
  REQUIRE(n == 1);
  REQUIRE(buf[0] == 'A');
}

TEST_CASE("Encode 2-byte", "[utf8]") {
  char buf[4];
  int n = utf8::encode(0x00E9, buf); // é
  REQUIRE(n == 2);
  REQUIRE((uint8_t)buf[0] == 0xC3);
  REQUIRE((uint8_t)buf[1] == 0xA9);
}

TEST_CASE("Encode 3-byte", "[utf8]") {
  char buf[4];
  int n = utf8::encode(0x4E16, buf); // 世
  REQUIRE(n == 3);
  REQUIRE((uint8_t)buf[0] == 0xE4);
  REQUIRE((uint8_t)buf[1] == 0xB8);
  REQUIRE((uint8_t)buf[2] == 0x96);
}

TEST_CASE("Encode 4-byte", "[utf8]") {
  char buf[4];
  int n = utf8::encode(0x1F3B5, buf); // 🎵
  REQUIRE(n == 4);
  REQUIRE((uint8_t)buf[0] == 0xF0);
  REQUIRE((uint8_t)buf[1] == 0x9F);
  REQUIRE((uint8_t)buf[2] == 0x8E);
  REQUIRE((uint8_t)buf[3] == 0xB5);
}

TEST_CASE("Encode-decode roundtrip", "[utf8]") {
  uint32_t codepoints[] = { 'A', 0xE9, 0x4E16, 0x1F3B5, 0xFFFD };
  for (uint32_t cp : codepoints) {
    char buf[5] = {};
    int n = utf8::encode(cp, buf);
    const char* p = buf;
    REQUIRE(utf8::decode(p) == cp);
    REQUIRE(p == buf + n);
  }
}
