#include "overlay_renderer.h"
#include "utf8.h"

#include <cmath>
#include <cstdio>

// --- Shaders ---

static const char* kPassthroughVert = R"(
#version 150
in vec2 aPos;
in vec2 aUV;
out vec2 vUV;
void main() {
  gl_Position = vec4(aPos, 0.0, 1.0);
  vUV = aUV;
})";

static const char* kPassthroughFrag = R"(
#version 150
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
  fragColor = texture(uTex, vUV);
})";

static const char* kQuadVert = R"(
#version 150
in vec2 aPos;
in vec4 aColor;
out vec4 vColor;
uniform vec2 uViewport;
void main() {
  vec2 ndc = (aPos / uViewport) * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);
  vColor = aColor;
})";

static const char* kQuadFrag = R"(
#version 150
in vec4 vColor;
out vec4 fragColor;
void main() {
  fragColor = vColor;
})";

// Image quad: pixel-space position, samples texture
static const char* kImageVert = R"(
#version 150
in vec2 aPos;
in vec2 aUV;
out vec2 vUV;
uniform vec2 uViewport;
void main() {
  vec2 ndc = (aPos / uViewport) * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);
  vUV = aUV;
})";

static const char* kImageFrag = R"(
#version 150
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
  fragColor = texture(uTex, vUV);
})";

// --- Helpers ---

GLuint OverlayRenderer::compileShader(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  return s;
}

GLuint OverlayRenderer::linkProgram(GLuint vert, GLuint frag) {
  GLuint p = glCreateProgram();
  glAttachShader(p, vert);
  glAttachShader(p, frag);
  glLinkProgram(p);
  glDeleteShader(vert);
  glDeleteShader(frag);
  return p;
}

// --- Init / Deinit ---

void OverlayRenderer::init() {
  // Passthrough
  {
    GLuint v = compileShader(GL_VERTEX_SHADER, kPassthroughVert);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, kPassthroughFrag);
    pt_program_ = linkProgram(v, f);
    float verts[] = { -1,-1, 0,0,  1,-1, 1,0,  -1,1, 0,1,  1,1, 1,1 };
    glGenVertexArrays(1, &pt_vao_);
    glGenBuffers(1, &pt_vbo_);
    glBindVertexArray(pt_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, pt_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glBindVertexArray(0);
  }

  // Quad shader
  {
    GLuint v = compileShader(GL_VERTEX_SHADER, kQuadVert);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, kQuadFrag);
    quad_program_ = linkProgram(v, f);
    glGenVertexArrays(1, &quad_vao_);
    glGenBuffers(1, &quad_vbo_);
    glBindVertexArray(quad_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void*)8);
    glBindVertexArray(0);
  }

  // Image shader (pixel-space textured quad)
  {
    GLuint v = compileShader(GL_VERTEX_SHADER, kImageVert);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, kImageFrag);
    img_program_ = linkProgram(v, f);
    glGenVertexArrays(1, &img_vao_);
    glGenBuffers(1, &img_vbo_);
    glBindVertexArray(img_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, img_vbo_);
    // pos(2) + uv(2) = 16 bytes
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glBindVertexArray(0);
  }

  // Text renderer (42pt monospace)
  text_.init(42.0f);
}

void OverlayRenderer::deinit() {
  auto del = [](GLuint& id, auto fn) { if (id) { fn(1, &id); id = 0; } };
  auto delP = [](GLuint& id) { if (id) { glDeleteProgram(id); id = 0; } };
  delP(pt_program_); del(pt_vao_, glDeleteVertexArrays); del(pt_vbo_, glDeleteBuffers);
  delP(quad_program_); del(quad_vao_, glDeleteVertexArrays); del(quad_vbo_, glDeleteBuffers);
  delP(img_program_); del(img_vao_, glDeleteVertexArrays); del(img_vbo_, glDeleteBuffers);
  text_.deinit();
}

// --- Passthrough ---

void OverlayRenderer::drawPassthrough(FFGLTextureStruct* tex, FFGLViewportStruct viewport) {
  glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
  glUseProgram(pt_program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex->Handle);
  glUniform1i(glGetUniformLocation(pt_program_, "uTex"), 0);
  glBindVertexArray(pt_vao_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glUseProgram(0);
}

// --- Quad batching ---

void OverlayRenderer::pushQuad(float x, float y, float w, float h,
                                float r, float g, float b, float a) {
  float x1 = x + w, y1 = y + h;
  auto emit = [&](float px, float py) {
    quad_batch_.push_back({px, py, r, g, b, a});
  };
  emit(x, y); emit(x1, y); emit(x, y1);
  emit(x1, y); emit(x1, y1); emit(x, y1);
}

void OverlayRenderer::flushQuads(int vp_w, int vp_h) {
  if (quad_batch_.empty()) return;
  glUseProgram(quad_program_);
  glUniform2f(glGetUniformLocation(quad_program_, "uViewport"),
              (float)vp_w, (float)vp_h);
  glBindVertexArray(quad_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
  glBufferData(GL_ARRAY_BUFFER,
               quad_batch_.size() * sizeof(ColorVertex),
               quad_batch_.data(), GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)quad_batch_.size());
  glBindVertexArray(0);
  glUseProgram(0);
  quad_batch_.clear();
}

// --- Draw a single textured quad ---

void OverlayRenderer::drawImage(GLuint tex, float x, float y, float w, float h,
                                 int vp_w, int vp_h) {
  if (!tex) return;
  float x1 = x + w, y1 = y + h;
  // Standard UV: top-left = (0,0), bottom-right = (1,1)
  float verts[] = {
    x,  y,  0, 0,
    x1, y,  1, 0,
    x,  y1, 0, 1,
    x1, y,  1, 0,
    x1, y1, 1, 1,
    x,  y1, 0, 1,
  };
  glUseProgram(img_program_);
  glUniform2f(glGetUniformLocation(img_program_, "uViewport"), (float)vp_w, (float)vp_h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glUniform1i(glGetUniformLocation(img_program_, "uTex"), 0);
  glBindVertexArray(img_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, img_vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glBindVertexArray(0);
  glUseProgram(0);
}

// --- Channel colors ---

static const float kChColor[4][3] = {
  {1.0f, 0.33f, 0.33f},
  {0.33f, 1.0f, 0.33f},
  {1.0f, 1.0f, 0.33f},
  {0.33f, 1.0f, 1.0f},
};

// --- Draw overlay ---

void OverlayRenderer::drawOverlay(const OverlayState& state) {
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  int vw = state.viewport_w;
  int vh = state.viewport_h;

  float gw = text_.glyph_advance();
  float lh = text_.line_height();
  float margin = 16.0f;
  float row_gap = 4.0f;

  // Layout top-down from top-left
  float y = margin;
  float content_w = 0; // track widest element for panel sizing

  // --- Title + connection status ---
  text_.push_text(margin, y, "Looper", 0.9f, 0.9f, 0.9f, 0.9f);

  if (state.recording)
    text_.push_text(margin + gw * 8, y, "* REC", 1, 0.2f, 0.2f, 1);

  y += lh + row_gap;

  // --- Connection status indicator ---
  {
    float dot_size = lh * 0.5f;
    float dot_x = margin;
    float dot_y = y + (lh - dot_size) * 0.5f;
    float text_x = margin + dot_size + gw * 0.5f;

    if (state.connected) {
      // Solid green dot + "OK"
      pushQuad(dot_x, dot_y, dot_size, dot_size, 0.2f, 0.9f, 0.2f, 0.9f);
      text_.push_text(text_x, y, "Connected", 0.2f, 0.9f, 0.2f, 0.7f);
    } else if (state.ever_connected) {
      // Was connected, now disconnected — pulsing yellow
      float pulse = 0.4f + 0.6f * (0.5f + 0.5f * std::sin(state.time_since_start * 6.0f));
      pushQuad(dot_x, dot_y, dot_size, dot_size, 0.9f, 0.7f, 0.1f, pulse);
      text_.push_text(text_x, y, "Reconnecting...", 0.9f, 0.7f, 0.1f, 0.6f);
    } else if (state.time_since_start > 5.0f) {
      // Never connected, been trying for >5 seconds — error
      float pulse = 0.3f + 0.7f * (0.5f + 0.5f * std::sin(state.time_since_start * 4.0f));
      pushQuad(dot_x, dot_y, dot_size, dot_size, 0.9f, 0.2f, 0.2f, pulse);
      text_.push_text(text_x, y, "Check Resolume webserver", 0.9f, 0.3f, 0.3f, 0.7f);
    } else {
      // Still trying — pulsing blue
      float pulse = 0.3f + 0.7f * (0.5f + 0.5f * std::sin(state.time_since_start * 8.0f));
      pushQuad(dot_x, dot_y, dot_size, dot_size, 0.3f, 0.5f, 1.0f, pulse);
      text_.push_text(text_x, y, "Connecting...", 0.4f, 0.6f, 1.0f, 0.6f);
    }
  }

  y += lh + row_gap;

  // --- Clip cards (row of 4, like Resolume) ---
  {
    float card_w = lh * 5;        // card width
    float thumb_h = card_w * 0.6f; // thumbnail aspect ~5:3
    float card_h = thumb_h + lh + 4; // thumb + text row + padding
    float card_gap = 8.0f;
    float border = 3.0f;

    // Flush quads so card backgrounds render first
    flushQuads(vw, vh);

    for (int i = 0; i < kOverlayChannels; ++i) {
      float cx = margin + i * (card_w + card_gap);

      // Border color: channel color if actively playing (gate open), dim otherwise
      bool ch_active = (state.active_step[i] >= 0);
      float br, bg, bb, ba;
      if (ch_active) {
        br = kChColor[i][0]; bg = kChColor[i][1]; bb = kChColor[i][2]; ba = 0.9f;
      } else if (state.clip_has_content[i]) {
        br = kChColor[i][0] * 0.4f; bg = kChColor[i][1] * 0.4f; bb = kChColor[i][2] * 0.4f; ba = 0.5f;
      } else {
        br = 0.25f; bg = 0.25f; bb = 0.25f; ba = 0.3f;
      }

      // Outer border
      pushQuad(cx, y, card_w, card_h, br, bg, bb, ba);
      // Inner background (dark)
      pushQuad(cx + border, y + border,
               card_w - border * 2, card_h - border * 2,
               0.05f, 0.05f, 0.05f, 0.85f);

      flushQuads(vw, vh);

      // Thumbnail
      float thumb_x = cx + border + 1;
      float thumb_y = y + border + 1;
      float thumb_w = card_w - border * 2 - 2;
      float th_h = thumb_h - border - 2;

      if (state.clip_thumbnail[i]) {
        drawImage(state.clip_thumbnail[i], thumb_x, thumb_y, thumb_w, th_h, vw, vh);
      } else {
        // Placeholder
        pushQuad(thumb_x, thumb_y, thumb_w, th_h, 0.12f, 0.12f, 0.12f, 0.6f);
      }

      // Clip name at ~70% size, truncated to fit card width
      float name_scale = 0.55f;
      float name_gw = gw * name_scale;
      std::string name = state.clip_names[i];
      if (name.empty()) name = "(empty)";
      int max_chars = (int)(thumb_w / name_gw);
      size_t cp_count = utf8::length(name.c_str());
      if ((int)cp_count > max_chars && max_chars > 1) {
        const char* p = name.c_str();
        int kept = 0;
        const char* end = p;
        while (*end && kept < max_chars - 1) {
          utf8::decode(end);
          ++kept;
        }
        name = std::string(name.c_str(), end - name.c_str()) + "\xe2\x80\xa6";
      }
      float name_y = y + thumb_h + 2;
      text_.push_text(cx + border + 2, name_y, name.c_str(),
                      0.7f, 0.7f, 0.7f, 0.7f, name_scale);

      // Mute overlay
      if (state.muted[i]) {
        pushQuad(cx + border, y + border,
                 card_w - border * 2, card_h - border * 2,
                 0, 0, 0, 0.6f);
        text_.push_text(cx + card_w * 0.25f, y + thumb_h * 0.4f,
                        "MUTE", 0.8f, 0.3f, 0.3f, 0.8f);
      }
    }

    float cards_total_w = kOverlayChannels * card_w + (kOverlayChannels - 1) * card_gap;
    if (cards_total_w > content_w) content_w = cards_total_w;
    y += card_h + row_gap * 2;
  }

  // --- Beat markers ---
  float cells_x = margin + gw * 2;
  float cell = lh + 4;
  float grid_total_w = cells_x - margin + kOverlaySteps * cell + gw;
  if (grid_total_w > content_w) content_w = grid_total_w;

  int current_step = (int)std::floor(state.phase);
  if (current_step >= kOverlaySteps) current_step = 0;

  for (int beat = 0; beat < 4; ++beat) {
    float bx = cells_x + beat * 4 * cell;
    char buf[4];
    snprintf(buf, sizeof(buf), "|%d", beat + 1);
    text_.push_text(bx, y, buf, 0.5f, 0.5f, 0.5f, 0.4f);
  }
  y += lh * 0.8f + row_gap;

  // --- Grid ---
  for (int ch = 0; ch < kOverlayChannels; ++ch) {
    bool is_muted = state.muted[ch];
    float cr = kChColor[ch][0], cg = kChColor[ch][1], cb = kChColor[ch][2];

    char label[2] = { (char)('1' + ch), 0 };
    text_.push_text(margin, y, label,
                    is_muted ? cr * 0.3f : cr,
                    is_muted ? cg * 0.3f : cg,
                    is_muted ? cb * 0.3f : cb);

    int act_step = state.active_step[ch]; // -1 if no step is active

    for (int s = 0; s < kOverlaySteps; ++s) {
      float cx = cells_x + s * cell;
      bool has_event = state.grid[ch][s];
      bool cur = (s == current_step);
      bool playing = (s == act_step); // THIS specific step is currently playing

      if (cur)
        pushQuad(cx - 1, y - 1, cell, cell, 0.5f, 0.5f, 0.5f, 0.25f);

      if (has_event) {
        if (is_muted) {
          pushQuad(cx, y, cell - 2, lh, cr, cg, cb, 0.25f);
        } else if (playing) {
          // This step is actively playing — full vibrant color
          pushQuad(cx, y, cell - 2, lh, cr, cg, cb, 1.0f);
        } else {
          // Has event but not currently playing — faded
          pushQuad(cx, y, cell - 2, lh, cr * 0.5f, cg * 0.5f, cb * 0.5f, 0.7f);
        }
      } else {
        pushQuad(cx, y, cell - 2, lh, 0.5f, 0.5f, 0.5f, cur ? 0.15f : 0.06f);
      }
    }
    y += cell;
  }

  y += row_gap;

  // --- Trigger indicators + modifiers ---
  for (int i = 0; i < kOverlayChannels; ++i) {
    float x = margin + i * gw * 3;
    float alpha = state.flash[i] > 0 ? 1.0f : 0.3f;
    char label[2] = { (char)('1' + i), 0 };
    text_.push_text(x, y, label, kChColor[i][0], kChColor[i][1], kChColor[i][2], alpha);
  }
  float mod_x = margin + kOverlayChannels * gw * 3 + gw * 2;
  text_.push_text(mod_x, y, "D", 1, 0.2f, 0.2f, state.delete_held ? 1.0f : 0.25f);
  text_.push_text(mod_x + gw * 2, y, "M", 1, 1, 0.2f, state.mute_held ? 1.0f : 0.25f);
  y += lh + row_gap;

  // Background panel — draw FIRST so it's behind everything
  float panel_w = content_w;
  // Insert at the beginning: flush any pending quads, draw panel, then re-flush content
  // We need to separate the panel from content. Flush content quads first.
  std::vector<ColorVertex> content_quads = std::move(quad_batch_);
  quad_batch_.clear();
  pushQuad(margin - 8, margin - 8, panel_w + 16, y - margin + 8, 0, 0, 0, 0.55f);
  flushQuads(vw, vh); // draw panel

  // Now draw content quads on top
  quad_batch_ = std::move(content_quads);
  flushQuads(vw, vh);

  // Text on top of everything
  text_.flush(vw, vh);

  glDisable(GL_BLEND);
}
