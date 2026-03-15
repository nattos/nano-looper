#pragma once

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif

#include <ffgl/FFGL.h>
#include <string>
#include <vector>

#include "text_renderer.h"

static constexpr int kOverlayChannels = 4;
static constexpr int kOverlaySteps = 16;

struct OverlayState {
  int viewport_w = 0;
  int viewport_h = 0;
  float phase = 0.0f;
  float bpm = 120.0f;
  bool recording = false;
  bool delete_held = false;
  bool mute_held = false;
  bool connected = false;
  bool ever_connected = false;
  float time_since_start = 0.0f;

  std::string clip_names[kOverlayChannels];
  bool clip_connected[kOverlayChannels] = {};
  bool clip_has_content[kOverlayChannels] = {};
  GLuint clip_thumbnail[kOverlayChannels] = {};
  int clip_thumb_w[kOverlayChannels] = {};
  int clip_thumb_h[kOverlayChannels] = {};
  bool muted[kOverlayChannels] = {};
  float flash[kOverlayChannels] = {};
  bool grid[kOverlayChannels][kOverlaySteps] = {};
  int active_step[kOverlayChannels] = {-1, -1, -1, -1}; // step currently playing (-1 = none)
};

class OverlayRenderer {
public:
  void init();
  void deinit();

  void drawPassthrough(FFGLTextureStruct* tex, FFGLViewportStruct viewport);
  void drawOverlay(const OverlayState& state);

private:
  // Passthrough
  GLuint pt_program_ = 0;
  GLuint pt_vao_ = 0;
  GLuint pt_vbo_ = 0;

  // Flat colored quads
  GLuint quad_program_ = 0;
  GLuint quad_vao_ = 0;
  GLuint quad_vbo_ = 0;

  // Text
  TextRenderer text_;

  // Textured quad (for thumbnails)
  GLuint img_program_ = 0;
  GLuint img_vao_ = 0;
  GLuint img_vbo_ = 0;

  GLuint compileShader(GLenum type, const char* src);
  GLuint linkProgram(GLuint vert, GLuint frag);

  // Batched quad drawing
  struct ColorVertex { float x, y, r, g, b, a; };
  std::vector<ColorVertex> quad_batch_;
  void pushQuad(float x, float y, float w, float h, float r, float g, float b, float a);
  void flushQuads(int vp_w, int vp_h);

  // Single textured quad
  void drawImage(GLuint tex, float x, float y, float w, float h, int vp_w, int vp_h);
};
