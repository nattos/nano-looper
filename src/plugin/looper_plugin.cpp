#include "looper_plugin.h"

#include <cmath>
#include <algorithm>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#endif

// --- Plugin registration ---

static CFFGLPluginInfo PluginInfo(
    PluginFactory<LooperPlugin>,
    "NLPR",
    "NanoLooper",
    2, 1,
    1, 0,
    FF_EFFECT,
    "Looper overlay for Resolume clip triggering",
    "nattos"
);

// --- Construction ---

LooperPlugin::LooperPlugin()
    : CFFGLPlugin(false),
      looper_(static_cast<double>(kNumSteps), kNumChannels) {
  SetMinInputs(1);
  SetMaxInputs(1);

  // All params are boolean (piano-key / momentary)
  SetParamInfo(PID_TRIGGER_1, "Trigger 1", FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PID_TRIGGER_2, "Trigger 2", FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PID_TRIGGER_3, "Trigger 3", FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PID_TRIGGER_4, "Trigger 4", FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PID_DELETE,    "Delete",    FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PID_MUTE,      "Mute",      FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PID_UNDO,      "Undo",      FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PID_REDO,      "Redo",      FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PID_RECORD,    "Record",    FF_TYPE_BOOLEAN, 0.0f);

  looper_.set_quantize(1.0);
}

LooperPlugin::~LooperPlugin() = default;

// --- GL lifecycle ---

FFResult LooperPlugin::InitGL(const FFGLViewportStruct* vp) {
  overlay_.init();
  ws_client_.connect("ws://127.0.0.1:8080/api/v1");
  last_tick_ = std::chrono::steady_clock::now();
  first_frame_ = true;
  return CFFGLPlugin::InitGL(vp);
}

FFResult LooperPlugin::DeInitGL() {
  ws_client_.disconnect();
  overlay_.deinit();
  return FF_SUCCESS;
}

// --- Resolume integration ---

void LooperPlugin::setupFromState(const nlohmann::json& state) {
  auto comp = resolume::parse_composition(state);

  // Free old thumbnails
  for (int i = 0; i < kNumChannels; ++i) {
    if (clips_[i].thumbnail_tex) {
      glDeleteTextures(1, &clips_[i].thumbnail_tex);
    }
    clips_[i] = {};
  }

  if (static_cast<int>(comp.layers.size()) > kTargetLayer) {
    auto& layer = comp.layers[kTargetLayer];
    for (int i = 0; i < kNumChannels && i < static_cast<int>(layer.clips.size()); ++i) {
      clips_[i].clip_id = layer.clips[i].id;
      clips_[i].connected_id = layer.clips[i].connected_id;
      clips_[i].name = layer.clips[i].name;
      clips_[i].connected_state = layer.clips[i].connected_state;
      clips_[i].thumbnail_path = layer.clips[i].thumbnail_path;
      clips_[i].thumbnail_is_default = layer.clips[i].thumbnail_is_default;

      // Fetch thumbnail for clips with content, using clip ID
      if (clips_[i].connected_state != "Empty" && clips_[i].clip_id != 0) {
        std::string url = "http://127.0.0.1:8080/api/v1/composition/clips/by-id/"
                        + std::to_string(clips_[i].clip_id) + "/thumbnail";
        clips_[i].thumbnail_tex = load_texture_from_url(
            url.c_str(), &clips_[i].thumb_w, &clips_[i].thumb_h);
      }
    }
  }
  if (state.contains("tempocontroller") &&
      state["tempocontroller"].contains("tempo")) {
    auto& tempo = state["tempocontroller"]["tempo"];
    if (tempo.contains("id")) tempo_id_ = tempo["id"].get<int64_t>();
    if (tempo.contains("value")) bpm_ = tempo["value"].get<double>();
  }
}

void LooperPlugin::subscribeParams() {
  for (int i = 0; i < kNumChannels; ++i) {
    if (clips_[i].connected_id != 0)
      ws_client_.subscribe_by_id(clips_[i].connected_id);
  }
  if (tempo_id_ != 0)
    ws_client_.subscribe_by_id(tempo_id_);
}

void LooperPlugin::processMessages() {
  for (auto& msg : ws_client_.poll()) {
    if (auto* cs = std::get_if<resolume::CompositionState>(&msg)) {
      setupFromState(cs->data);
      subscribeParams();
    } else if (auto* pu = std::get_if<resolume::ParameterUpdate>(&msg)) {
      if (pu->id == tempo_id_ && pu->value.is_number())
        bpm_ = pu->value.get<double>();
      for (int i = 0; i < kNumChannels; ++i) {
        if (pu->id == clips_[i].connected_id && pu->value.is_string())
          clips_[i].connected_state = pu->value.get<std::string>();
      }
    }
  }
}

// --- Gate helpers ---

void LooperPlugin::gateOn(int ch, int step) {
  if (clips_[ch].clip_id == 0) return;
  if (gate_down_[ch]) {
    ws_client_.trigger_clip_off(clips_[ch].clip_id);
  }
  ws_client_.trigger_clip_on(clips_[ch].clip_id);
  gate_down_[ch] = true;
  gate_step_[ch] = step;
  gate_timer_[ch] = (float)(60.0 / bpm_ / 4.0);
}

void LooperPlugin::gateOff(int ch) {
  if (!gate_down_[ch]) return;
  if (clips_[ch].clip_id != 0)
    ws_client_.trigger_clip_off(clips_[ch].clip_id);
  gate_down_[ch] = false;
  gate_step_[ch] = -1;
  gate_timer_[ch] = 0;
}

// --- Parameter handling ---

FFResult LooperPlugin::SetFloatParameter(unsigned int index, float value) {
  if (index >= PID_COUNT) return FF_FAIL;

  float prev = param_values_[index];
  param_values_[index] = value;
  bool rising = (value >= 0.5f && prev < 0.5f);
  bool falling = (value < 0.5f && prev >= 0.5f);

  switch (index) {
    case PID_TRIGGER_1:
    case PID_TRIGGER_2:
    case PID_TRIGGER_3:
    case PID_TRIGGER_4: {
      int ch = index - PID_TRIGGER_1;
      trigger_held_[ch] = (value >= 0.5f);

      if (rising) {
        if (delete_held_) {
          looper_.clear_channel(ch);
          delete_acted_ = true;
          // Release gate if channel was playing
          gateOff(ch);
        } else if (!mute_held_) {
          // Normal trigger: record + gate on
          looper_.trigger(ch, phase_);
          int step = static_cast<int>(std::floor(phase_)) % kNumSteps;
          gateOn(ch, step);
          flash_[ch] = 0.25f;
          if (record_held_)
            step_protected_[ch][step] = true;
        }
      }
      break;
    }

    case PID_DELETE:
      if (rising) {
        delete_held_ = true;
        delete_acted_ = false;
      } else if (falling && delete_held_) {
        if (!delete_acted_) {
          looper_.clear_all();
          // Release all gates
          for (int c = 0; c < kNumChannels; ++c) gateOff(c);
        }
        delete_held_ = false;
      }
      break;

    case PID_MUTE:
      mute_held_ = (value >= 0.5f);
      break;

    case PID_UNDO:
      if (rising) {
        looper_.undo();
        for (int c = 0; c < kNumChannels; ++c) gateOff(c);
      }
      break;
    case PID_REDO:
      if (rising) looper_.redo();
      break;

    case PID_RECORD:
      if (rising) {
        looper_.begin_destructive_record();
        record_held_ = true;
        record_last_step_ = -1;
        for (int c = 0; c < kNumChannels; ++c)
          for (int s = 0; s < kNumSteps; ++s)
            step_protected_[c][s] = false;
      } else if (falling && record_held_) {
        looper_.end_destructive_record();
        record_held_ = false;
      }
      break;
  }

  return FF_SUCCESS;
}

float LooperPlugin::GetFloatParameter(unsigned int index) {
  if (index >= PID_COUNT) return 0.0f;
  return param_values_[index];
}

// --- Main render ---

FFResult LooperPlugin::ProcessOpenGL(ProcessOpenGLStruct* pGL) {
  if (!pGL || pGL->numInputTextures < 1 || !pGL->inputTextures[0])
    return FF_FAIL;

  auto now = std::chrono::steady_clock::now();
  float dt = first_frame_ ? (1.0f / 60.0f)
      : std::chrono::duration<float>(now - last_tick_).count();
  last_tick_ = now;
  first_frame_ = false;
  dt = std::min(dt, 0.1f);

  processMessages();

  // Advance looper
  double prev = phase_;
  double sixteenths_per_sec = (bpm_ / 60.0) * 4.0;
  phase_ = std::fmod(phase_ + sixteenths_per_sec * dt, static_cast<double>(kNumSteps));

  // Record: clear steps at playhead as it advances
  if (record_held_) {
    int cur_step = static_cast<int>(std::floor(phase_)) % kNumSteps;
    if (cur_step != record_last_step_) {
      // Entered a new step — clear unprotected events here
      for (int ch = 0; ch < kNumChannels; ++ch) {
        if (!step_protected_[ch][cur_step])
          looper_.clear_at(ch, cur_step);
      }
      // Previous step is no longer protected (playhead has passed it)
      if (record_last_step_ >= 0) {
        for (int ch = 0; ch < kNumChannels; ++ch)
          step_protected_[ch][record_last_step_] = false;
      }
      record_last_step_ = cur_step;
    }
  }

  // Playback: fire events via gate, tracking which step triggered
  {
    // Collect which steps fired per channel
    int fired_step[kNumChannels];
    for (int c = 0; c < kNumChannels; ++c) fired_step[c] = -1;

    for (const auto& e : looper_.events()) {
      bool in_range;
      if (phase_ >= prev)
        in_range = e.time >= prev - 1e-6 && e.time < phase_ - 1e-6;
      else
        in_range = e.time >= prev - 1e-6 || e.time < phase_ - 1e-6;
      if (in_range && e.channel >= 0 && e.channel < kNumChannels) {
        fired_step[e.channel] = static_cast<int>(std::floor(e.time)) % kNumSteps;
      }
    }

    for (int ch = 0; ch < kNumChannels; ++ch) {
      if (fired_step[ch] >= 0) {
        bool ch_muted = mute_held_ && trigger_held_[ch];
        if (!ch_muted) {
          gateOn(ch, fired_step[ch]);
        } else if (gate_down_[ch]) {
          gateOff(ch);
        }
      }
    }
  }

  // Gate timers: release after one 16th note
  for (int ch = 0; ch < kNumChannels; ++ch) {
    if (gate_down_[ch]) {
      gate_timer_[ch] -= dt;
      if (gate_timer_[ch] <= 0)
        gateOff(ch);
    }
  }

  // Check mute: if a channel just became muted while gate is open, release
  for (int ch = 0; ch < kNumChannels; ++ch) {
    bool ch_muted = mute_held_ && trigger_held_[ch];
    if (ch_muted && gate_down_[ch])
      gateOff(ch);
  }

  // Decay channel flash
  for (int i = 0; i < kNumChannels; ++i)
    flash_[i] = std::max(0.0f, flash_[i] - dt);

  // --- Render ---
  FFGLTextureStruct* tex = pGL->inputTextures[0];
  overlay_.drawPassthrough(tex, currentViewport);

  OverlayState state;
  state.viewport_w = currentViewport.width;
  state.viewport_h = currentViewport.height;
  state.phase = static_cast<float>(phase_);
  state.bpm = static_cast<float>(bpm_);
  state.recording = record_held_;
  state.delete_held = delete_held_;
  state.mute_held = mute_held_;
  bool is_connected = ws_client_.is_connected();
  if (is_connected) ever_connected_ = true;
  time_since_start_ += dt;
  state.connected = is_connected;
  state.ever_connected = ever_connected_;
  state.time_since_start = time_since_start_;

  for (int ch = 0; ch < kNumChannels; ++ch) {
    state.clip_names[ch] = clips_[ch].name;
    state.clip_connected[ch] = (clips_[ch].connected_state == "Connected");
    state.clip_has_content[ch] = (clips_[ch].connected_state != "Empty");
    state.clip_thumbnail[ch] = clips_[ch].thumbnail_tex;
    state.clip_thumb_w[ch] = clips_[ch].thumb_w;
    state.clip_thumb_h[ch] = clips_[ch].thumb_h;
    // Mute display: momentary, both keys held
    state.muted[ch] = mute_held_ && trigger_held_[ch];
    state.flash[ch] = flash_[ch];

    state.active_step[ch] = gate_down_[ch] ? gate_step_[ch] : -1;

    auto events = looper_.events_for_channel(ch);
    for (const auto& e : events) {
      int step = static_cast<int>(std::floor(e.time));
      if (step >= 0 && step < kNumSteps)
        state.grid[ch][step] = true;
    }
  }

  overlay_.drawOverlay(state);

  glBindFramebuffer(GL_FRAMEBUFFER, pGL->HostFBO);
  return FF_SUCCESS;
}
