#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <ffgl/FFGLPluginSDK.h>

#include "image_loader.h"
#include "looper/core.h"
#include "overlay_renderer.h"
#include "resolume/composition.h"
#include "resolume/ws_client.h"

enum ParamID : FFUInt32 {
  PID_TRIGGER_1 = 0,
  PID_TRIGGER_2,
  PID_TRIGGER_3,
  PID_TRIGGER_4,
  PID_DELETE,
  PID_MUTE,
  PID_UNDO,
  PID_REDO,
  PID_RECORD,
  PID_COUNT,
};

static constexpr int kNumChannels = 4;
static constexpr int kNumSteps = 16;
static constexpr int kTargetLayer = 1;

struct PluginClipInfo {
  int64_t clip_id = 0;
  int64_t connected_id = 0;
  std::string name;
  std::string connected_state = "Empty";
  std::string thumbnail_path;
  bool thumbnail_is_default = true;
  GLuint thumbnail_tex = 0;
  int thumb_w = 0, thumb_h = 0;
};

class LooperPlugin : public CFFGLPlugin {
public:
  LooperPlugin();
  ~LooperPlugin() override;

  FFResult InitGL(const FFGLViewportStruct* vp) override;
  FFResult DeInitGL() override;
  FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
  FFResult SetFloatParameter(unsigned int index, float value) override;
  float GetFloatParameter(unsigned int index) override;

private:
  void processMessages();
  void setupFromState(const nlohmann::json& state);
  void subscribeParams();

  // Core logic
  looper::LooperCore looper_;
  resolume::ResolumeWsClient ws_client_;

  // Resolume state
  PluginClipInfo clips_[kNumChannels];
  double bpm_ = 120.0;
  int64_t tempo_id_ = 0;
  double phase_ = 0.0;

  // Channel trigger held state (piano-key)
  bool trigger_held_[kNumChannels] = {};

  // Mute: momentary. Muted while BOTH mute + channel key are held.
  bool mute_held_ = false;

  // Delete: momentary. Hold delete, press 1-4 to clear channel.
  // Release without pressing 1-4 → clear all.
  bool delete_held_ = false;
  bool delete_acted_ = false;

  // Record: momentary. Held = recording. Clears steps at playhead.
  bool record_held_ = false;
  int record_last_step_ = -1; // last step cleared by record head
  // Protect steps that were just user-added from being cleared
  bool step_protected_[kNumChannels][kNumSteps] = {};

  // Clip gate: keep clip "connected" for one 16th note
  bool gate_down_[kNumChannels] = {};
  float gate_timer_[kNumChannels] = {};
  int gate_step_[kNumChannels] = {}; // which step opened this gate (-1 = none)

  void gateOn(int ch, int step = -1);
  void gateOff(int ch);

  // Visual
  float flash_[kNumChannels] = {};

  // Parameter values (for GetFloatParameter)
  float param_values_[PID_COUNT] = {};

  // Connection status
  bool ever_connected_ = false;
  float time_since_start_ = 0.0f;

  // Timing
  std::chrono::steady_clock::time_point last_tick_;
  bool first_frame_ = true;

  // Rendering
  OverlayRenderer overlay_;
};
