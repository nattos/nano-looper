#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <ffgl/FFGLPluginSDK.h>

#include "image_loader.h"
#include "looper/core.h"
#include "synth.h"
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
  PID_SHOW_OVERLAY,
  PID_SYNTH,
  PID_SYNTH_GAIN,
  PID_COUNT,
};

static constexpr int kNumChannels = 4;
static constexpr int kNumSteps = 16;

// The FFGL plugin identifiers used to find channel tag effects in the composition.
// Resolume may store the 4-char code, the plugin name, or an internal name.
static constexpr const char* kChannelTagFFGLCode = "NLCH";
static constexpr const char* kChannelTagPluginName = "NanoLooper Ch";
// The parameter name in the channel tag effect
static constexpr const char* kChannelParamName = "Channel";

struct PluginClipRef {
  int64_t clip_id = 0;
  int64_t connected_id = 0;
};

struct PluginChannelInfo {
  // First clip's display info (for UI)
  std::string name;
  std::string connected_state = "Empty";
  GLuint thumbnail_tex = 0;
  int thumb_w = 0, thumb_h = 0;

  // All clips assigned to this channel
  std::vector<PluginClipRef> clips;

  void clear() {
    name.clear();
    connected_state = "Empty";
    if (thumbnail_tex) { glDeleteTextures(1, &thumbnail_tex); thumbnail_tex = 0; }
    thumb_w = thumb_h = 0;
    clips.clear();
  }
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

  // Scan all clips in the composition for NanoLooper Ch effects.
  // Falls back to layer 2 first 4 clips if no tags found.
  void assignChannelsFromComposition(const resolume::Composition& comp);

  // Core logic
  looper::LooperCore looper_;
  resolume::ResolumeWsClient ws_client_;

  // Channel state
  PluginChannelInfo channels_[kNumChannels];
  double bpm_ = 120.0;
  int64_t tempo_id_ = 0;
  double phase_ = 0.0;

  // Channel trigger held state (piano-key)
  bool trigger_held_[kNumChannels] = {};

  // Mute: momentary
  bool mute_held_ = false;

  // Delete: momentary. Double-tap delete-all = undo.
  bool delete_held_ = false;
  bool delete_acted_ = false;
  bool last_action_was_clear_all_ = false;

  // Record: momentary
  bool record_held_ = false;
  int record_last_step_ = -1;
  bool step_protected_[kNumChannels][kNumSteps] = {};

  // Clip gate
  bool gate_down_[kNumChannels] = {};
  float gate_timer_[kNumChannels] = {};
  int gate_step_[kNumChannels] = {};

  // Watchdog: after gate closes, keep monitoring Resolume's reported
  // connected state. If it still says "Connected" after 100ms, send
  // another false. Repeat every 100ms until it clears or a new gate opens.
  bool watchdog_active_[kNumChannels] = {};
  float watchdog_timer_[kNumChannels] = {};
  static constexpr float kWatchdogInterval = 0.1f; // 100ms

  void gateOn(int ch, int step = -1);
  void gateOff(int ch);

  // Visual
  float flash_[kNumChannels] = {};

  // Parameter values
  float param_values_[PID_COUNT] = {};

  // Connection status
  bool ever_connected_ = false;
  float time_since_start_ = 0.0f;

  // Timing
  std::chrono::steady_clock::time_point last_tick_;
  bool first_frame_ = true;

  // Audio
  Synth synth_;

  // Rendering
  OverlayRenderer overlay_;
};
