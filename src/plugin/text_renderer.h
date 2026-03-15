#pragma once

#include <memory>

// TextRenderer: monospace text rendering with Unicode support and font fallback.
//
// Uses Core Text on macOS for glyph rasterization. Fonts are resolved with
// automatic system fallback, so CJK, emoji, etc. render correctly even if
// the primary font doesn't cover them.
//
// Usage:
//   TextRenderer tr;
//   tr.init(14.0f);                                     // font size in points
//   tr.push_text(10, 10, "Hello 世界 🎵", 1,1,1, 1);  // queue text
//   tr.flush(viewport_w, viewport_h);                   // draw all queued text
//   tr.deinit();
//
// All text input is UTF-8. Layout is strictly monospace: every codepoint
// advances by glyph_advance() pixels regardless of the actual glyph width.

class TextRenderer {
public:
  TextRenderer();
  ~TextRenderer();

  TextRenderer(const TextRenderer&) = delete;
  TextRenderer& operator=(const TextRenderer&) = delete;

  void init(float font_size);
  void deinit();

  // Metrics (available after init)
  float glyph_advance() const;
  float line_height() const;

  // Queue text for rendering. Coordinates are in pixels (top-left origin).
  // `scale` shrinks/grows the glyphs relative to the init font size (1.0 = normal).
  void push_text(float x, float y, const char* utf8_text,
                 float r, float g, float b, float a = 1.0f, float scale = 1.0f);

  // Draw all queued text. Call once per frame after all push_text calls.
  void flush(int viewport_w, int viewport_h);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
