#include "text_renderer.h"
#include "utf8.h"

#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif

#include <unordered_map>
#include <vector>

// --- Shaders ---

static const char* kTextVert = R"(
#version 150
in vec2 aPos;
in vec2 aUV;
in vec4 aColor;
out vec2 vUV;
out vec4 vColor;
uniform vec2 uViewport;
void main() {
  vec2 ndc = (aPos / uViewport) * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);
  vUV = aUV;
  vColor = aColor;
})";

static const char* kTextFrag = R"(
#version 150
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
uniform sampler2D uFont;
void main() {
  float a = texture(uFont, vUV).r;
  fragColor = vec4(vColor.rgb, vColor.a * a);
})";

// --- Glyph cache entry ---

struct CachedGlyph {
  float u0, v0, u1, v1; // UV in atlas
  float bmp_w, bmp_h;   // bitmap size in pixels
  float bearing_x, bearing_y; // offset from pen position to top-left of bitmap
  bool valid = false;
};

// --- Implementation ---

struct TextRenderer::Impl {
  CTFontRef font = nullptr;
  float font_size = 0;
  float advance = 0;  // monospace advance width
  float ascent = 0;
  float descent = 0;
  float leading = 0;

  // GL resources
  GLuint program = 0;
  GLuint vao = 0;
  GLuint vbo = 0;
  GLuint atlas_tex = 0;
  int atlas_w = 0;
  int atlas_h = 0;
  int cursor_x = 0;
  int cursor_y = 0;
  int row_h = 0;

  // Glyph cache: codepoint → CachedGlyph
  std::unordered_map<uint32_t, CachedGlyph> cache;

  // Vertex batch: pos(2) + uv(2) + color(4)
  struct Vertex { float x, y, u, v, r, g, b, a; };
  std::vector<Vertex> batch;

  // --- Font setup ---

  void createFont(float size) {
    font_size = size;

    // Primary: Menlo. Fallback: system monospace.
    font = CTFontCreateWithName(CFSTR("Menlo"), size, nullptr);
    if (!font) {
      // macOS 10.15+
      font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr);
    }

    // Get monospace advance width from space character
    UniChar space = ' ';
    CGGlyph spaceGlyph;
    CTFontGetGlyphsForCharacters(font, &space, &spaceGlyph, 1);
    CGSize adv;
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationDefault, &spaceGlyph, &adv, 1);
    advance = (float)adv.width;

    ascent = (float)CTFontGetAscent(font);
    descent = (float)CTFontGetDescent(font);
    leading = (float)CTFontGetLeading(font);
  }

  // --- Atlas management ---

  void createAtlas() {
    atlas_w = 512;
    atlas_h = 512;
    cursor_x = 1; // 1px border
    cursor_y = 1;
    row_h = 0;

    glGenTextures(1, &atlas_tex);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_w, atlas_h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  void growAtlas() {
    int new_w = atlas_w * 2;
    int new_h = atlas_h * 2;

    // Read old pixels
    std::vector<uint8_t> old_pixels(atlas_w * atlas_h);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, old_pixels.data());

    // Create new texture
    std::vector<uint8_t> new_pixels(new_w * new_h, 0);
    for (int y = 0; y < atlas_h; ++y)
      memcpy(&new_pixels[y * new_w], &old_pixels[y * atlas_w], atlas_w);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, new_w, new_h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, new_pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Update UV coords for all cached glyphs
    float sx = (float)atlas_w / new_w;
    float sy = (float)atlas_h / new_h;
    for (auto& [cp, g] : cache) {
      g.u0 *= sx; g.u1 *= sx;
      g.v0 *= sy; g.v1 *= sy;
    }

    atlas_w = new_w;
    atlas_h = new_h;
  }

  // --- Glyph rasterization ---

  CachedGlyph& getGlyph(uint32_t codepoint) {
    auto it = cache.find(codepoint);
    if (it != cache.end()) return it->second;
    return rasterize(codepoint);
  }

  CachedGlyph& rasterize(uint32_t codepoint) {
    CachedGlyph& g = cache[codepoint];
    g.valid = false;

    // Convert codepoint to UTF-16
    UniChar chars[2];
    int charCount;
    if (codepoint <= 0xFFFF) {
      chars[0] = (UniChar)codepoint;
      charCount = 1;
    } else {
      uint32_t cp = codepoint - 0x10000;
      chars[0] = 0xD800 + (cp >> 10);
      chars[1] = 0xDC00 + (cp & 0x3FF);
      charCount = 2;
    }

    // Get font for this character (handles fallback)
    CFStringRef str = CFStringCreateWithCharacters(nullptr, chars, charCount);
    CTFontRef glyphFont = CTFontCreateForString(font, str, CFRangeMake(0, charCount));
    CFRelease(str);
    if (!glyphFont) glyphFont = (CTFontRef)CFRetain(font);

    // Get glyph ID
    CGGlyph glyphs[2] = {};
    bool found = CTFontGetGlyphsForCharacters(glyphFont, chars, glyphs, charCount);
    if (!found) {
      // Use replacement character glyph
      UniChar repl = 0xFFFD;
      CTFontGetGlyphsForCharacters(font, &repl, glyphs, 1);
    }

    // Get bounding rect
    CGRect bbox = CTFontGetBoundingRectsForGlyphs(
        glyphFont, kCTFontOrientationDefault, glyphs, nullptr, 1);

    int bmp_w = (int)ceil(bbox.size.width) + 2;
    int bmp_h = (int)ceil(bbox.size.height) + 2;
    if (bmp_w < 1) bmp_w = 1;
    if (bmp_h < 1) bmp_h = 1;

    // Rasterize into a grayscale bitmap
    std::vector<uint8_t> pixels(bmp_w * bmp_h, 0);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), bmp_w, bmp_h, 8, bmp_w,
        colorSpace, kCGImageAlphaNone);
    CGColorSpaceRelease(colorSpace);

    CGContextSetGrayFillColor(ctx, 1.0, 1.0);

    // Position glyph so its bbox lands inside the bitmap
    CGPoint pos = CGPointMake(-bbox.origin.x + 1, -bbox.origin.y + 1);
    CTFontDrawGlyphs(glyphFont, glyphs, &pos, 1, ctx);
    CGContextRelease(ctx);
    CFRelease(glyphFont);

    // Pack into atlas
    if (cursor_x + bmp_w + 1 > atlas_w) {
      cursor_x = 1;
      cursor_y += row_h + 1;
      row_h = 0;
    }
    if (cursor_y + bmp_h + 1 > atlas_h) {
      growAtlas();
    }

    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, cursor_x, cursor_y,
                    bmp_w, bmp_h, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    g.u0 = (float)cursor_x / atlas_w;
    g.v0 = (float)cursor_y / atlas_h;
    g.u1 = (float)(cursor_x + bmp_w) / atlas_w;
    g.v1 = (float)(cursor_y + bmp_h) / atlas_h;
    g.bmp_w = (float)bmp_w;
    g.bmp_h = (float)bmp_h;
    g.bearing_x = (float)bbox.origin.x - 1;
    g.bearing_y = (float)bbox.origin.y - 1;
    g.valid = true;

    cursor_x += bmp_w + 1;
    if (bmp_h > row_h) row_h = bmp_h;

    return g;
  }

  // --- GL setup ---

  void createGL() {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kTextVert, nullptr);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &kTextFrag, nullptr);
    glCompileShader(fs);

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    // pos(2) + uv(2) + color(4) = 32 bytes
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void*)8);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, (void*)16);
    glBindVertexArray(0);
  }
};

// --- Public API ---

TextRenderer::TextRenderer() : impl_(std::make_unique<Impl>()) {}
TextRenderer::~TextRenderer() { deinit(); }

void TextRenderer::init(float font_size) {
  impl_->createFont(font_size);
  impl_->createAtlas();
  impl_->createGL();
}

void TextRenderer::deinit() {
  if (impl_->program) { glDeleteProgram(impl_->program); impl_->program = 0; }
  if (impl_->vao) { glDeleteVertexArrays(1, &impl_->vao); impl_->vao = 0; }
  if (impl_->vbo) { glDeleteBuffers(1, &impl_->vbo); impl_->vbo = 0; }
  if (impl_->atlas_tex) { glDeleteTextures(1, &impl_->atlas_tex); impl_->atlas_tex = 0; }
  if (impl_->font) { CFRelease(impl_->font); impl_->font = nullptr; }
  impl_->cache.clear();
}

float TextRenderer::glyph_advance() const { return impl_->advance; }
float TextRenderer::line_height() const {
  return impl_->ascent + impl_->descent + impl_->leading;
}

void TextRenderer::push_text(float x, float y, const char* utf8_text,
                              float r, float g, float b, float a, float scale) {
  float pen_x = x;
  float baseline_y = y + impl_->ascent * scale;
  const char* p = utf8_text;

  while (*p) {
    uint32_t cp = utf8::decode(p);
    auto& glyph = impl_->getGlyph(cp);

    if (glyph.valid) {
      float gx = pen_x + glyph.bearing_x * scale;
      float gy = baseline_y - (glyph.bearing_y + glyph.bmp_h) * scale;

      float x1 = gx + glyph.bmp_w * scale;
      float y1 = gy + glyph.bmp_h * scale;

      auto emit = [&](float px, float py, float pu, float pv) {
        impl_->batch.push_back({px, py, pu, pv, r, g, b, a});
      };
      emit(gx, gy, glyph.u0, glyph.v0);
      emit(x1, gy, glyph.u1, glyph.v0);
      emit(gx, y1, glyph.u0, glyph.v1);
      emit(x1, gy, glyph.u1, glyph.v0);
      emit(x1, y1, glyph.u1, glyph.v1);
      emit(gx, y1, glyph.u0, glyph.v1);
    }

    pen_x += impl_->advance * scale;
  }
}

void TextRenderer::flush(int vp_w, int vp_h) {
  if (impl_->batch.empty()) return;

  glUseProgram(impl_->program);
  glUniform2f(glGetUniformLocation(impl_->program, "uViewport"),
              (float)vp_w, (float)vp_h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, impl_->atlas_tex);
  glUniform1i(glGetUniformLocation(impl_->program, "uFont"), 0);

  glBindVertexArray(impl_->vao);
  glBindBuffer(GL_ARRAY_BUFFER, impl_->vbo);
  glBufferData(GL_ARRAY_BUFFER,
               impl_->batch.size() * sizeof(Impl::Vertex),
               impl_->batch.data(), GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)impl_->batch.size());
  glBindVertexArray(0);
  glUseProgram(0);
  impl_->batch.clear();
}
