#pragma once

#include <cstddef>
#include <cstdint>

namespace utf8 {

// Decode one codepoint from a UTF-8 string.
// Advances `p` past the decoded bytes.
// Returns U+FFFD on invalid input and advances by 1 byte.
inline uint32_t decode(const char*& p) {
  const auto* s = reinterpret_cast<const uint8_t*>(p);

  if (s[0] < 0x80) {
    p += 1;
    return s[0];
  }
  if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    uint32_t cp = ((uint32_t)(s[0] & 0x1F) << 6)
                | (s[1] & 0x3F);
    p += 2;
    return cp >= 0x80 ? cp : 0xFFFD; // reject overlong
  }
  if ((s[0] & 0xF0) == 0xE0 &&
      (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    uint32_t cp = ((uint32_t)(s[0] & 0x0F) << 12)
                | ((uint32_t)(s[1] & 0x3F) << 6)
                | (s[2] & 0x3F);
    p += 3;
    return cp >= 0x800 ? cp : 0xFFFD;
  }
  if ((s[0] & 0xF8) == 0xF0 &&
      (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
      (s[3] & 0xC0) == 0x80) {
    uint32_t cp = ((uint32_t)(s[0] & 0x07) << 18)
                | ((uint32_t)(s[1] & 0x3F) << 12)
                | ((uint32_t)(s[2] & 0x3F) << 6)
                | (s[3] & 0x3F);
    p += 4;
    return (cp >= 0x10000 && cp <= 0x10FFFF) ? cp : 0xFFFD;
  }

  // Invalid lead byte
  p += 1;
  return 0xFFFD;
}

// Count the number of codepoints in a null-terminated UTF-8 string.
inline size_t length(const char* s) {
  size_t n = 0;
  while (*s) {
    decode(s);
    ++n;
  }
  return n;
}

// Encode a codepoint to UTF-8. Returns number of bytes written (1-4).
// `out` must have room for at least 4 bytes.
inline int encode(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp <= 0x10FFFF) {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
  }
  // Invalid codepoint → replacement character
  return encode(0xFFFD, out);
}

} // namespace utf8
