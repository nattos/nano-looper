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
  SetParamInfo(PID_SHOW_OVERLAY, "Show Overlay", FF_TYPE_BOOLEAN, 1.0f);

  looper_.set_quantize(1.0);
  param_values_[PID_SHOW_OVERLAY] = 1.0f;
}

LooperPlugin::~LooperPlugin() {
  // Belt-and-suspenders: release any open gates before WS client destructor
  // disconnects. DeInitGL should have done this already, but be safe.
  for (int ch = 0; ch < kNumChannels; ++ch) {
    if (gate_down_[ch] && !channels_[ch].clips.empty()) {
      for (const auto& cr : channels_[ch].clips)
        ws_client_.trigger_clip_off(cr.clip_id);
      gate_down_[ch] = false;
    }
  }
  // ws_client_ destructor calls disconnect() → stops IXWebSocket thread
  // TextRenderer destructor calls deinit() — GL calls may be no-ops without context,
  // but the CTFont and atlas memory are freed.
  // Channel thumbnail textures should have been freed in DeInitGL.
}

// --- GL lifecycle ---

FFResult LooperPlugin::InitGL(const FFGLViewportStruct* vp) {
  overlay_.init();
  ws_client_.connect("ws://127.0.0.1:8080/api/v1");
  last_tick_ = std::chrono::steady_clock::now();
  first_frame_ = true;
  return CFFGLPlugin::InitGL(vp);
}

FFResult LooperPlugin::DeInitGL() {
  // Release any open gates before disconnecting
  for (int ch = 0; ch < kNumChannels; ++ch)
    gateOff(ch);

  ws_client_.disconnect();

  // Free thumbnail textures (GL context is still current here)
  for (int i = 0; i < kNumChannels; ++i)
    channels_[i].clear();

  overlay_.deinit();
  return FF_SUCCESS;
}

// --- Resolume integration ---

// Check if a string matches any of our channel tag identifiers.
static bool is_channel_tag_effect(const std::string& s) {
  return s == kChannelTagFFGLCode ||
         s == kChannelTagPluginName ||
         // Resolume may store names with slight variations
         s.find("NanoLooper") != std::string::npos ||
         s.find("NLCH") != std::string::npos;
}

// Determine which channel (0-3) a clip is tagged with via NanoLooper Ch effect.
// Returns -1 if no tag found or tag is "Off".
static int channel_from_clip(const resolume::Clip& clip) {
  for (const auto& eff : clip.effects) {
    if (!is_channel_tag_effect(eff.name) &&
        !is_channel_tag_effect(eff.display_name))
      continue;

    // Find the Channel param — try exact name, then any param that looks like an option
    auto it = eff.params.find(kChannelParamName);
    if (it == eff.params.end()) {
      // Resolume may capitalize differently or use display name
      for (auto candidate = eff.params.begin(); candidate != eff.params.end(); ++candidate) {
        if (candidate->first.find("hannel") != std::string::npos ||
            candidate->second.valuetype == "ParamChoice" ||
            candidate->second.valuetype == "ParamOption") {
          it = candidate;
          break;
        }
      }
    }
    if (it == eff.params.end()) continue;

    // Resolume stores ParamChoice as string label ("Channel 1", "Off", etc.)
    // or as float (0.0=Off, 0.2=Ch1, 0.4=Ch2, 0.6=Ch3, 0.8=Ch4)
    if (it->second.value.is_string()) {
      std::string val = it->second.value.get<std::string>();
      if (val.find('1') != std::string::npos) return 0;
      if (val.find('2') != std::string::npos) return 1;
      if (val.find('3') != std::string::npos) return 2;
      if (val.find('4') != std::string::npos) return 3;
      return -1; // "Off" or unrecognized
    }
    if (it->second.value.is_number()) {
      float v = it->second.value.get<float>();
      if (v < 0.1f) return -1;
      if (v < 0.3f) return 0;
      if (v < 0.5f) return 1;
      if (v < 0.7f) return 2;
      return 3;
    }
    return -1;
  }
  return -1;
}

void LooperPlugin::assignChannelsFromComposition(const resolume::Composition& comp) {
  for (int i = 0; i < kNumChannels; ++i)
    channels_[i].clear();

  // Scan ALL layers and clips for channel tag effects
  for (const auto& layer : comp.layers) {
    for (const auto& clip : layer.clips) {
      int ch = channel_from_clip(clip);
      if (ch < 0 || ch >= kNumChannels) continue;
      auto& chan = channels_[ch];
      chan.clips.push_back({clip.id, clip.connected_id});

      // First clip sets the display info
      if (chan.clips.size() == 1) {
        chan.name = clip.name;
        chan.connected_state = clip.connected_state;
        // Fetch thumbnail
        if (clip.connected_state != "Empty" && clip.id != 0) {
          std::string url = "http://127.0.0.1:8080/api/v1/composition/clips/by-id/"
                          + std::to_string(clip.id) + "/thumbnail";
          chan.thumbnail_tex = load_texture_from_url(
              url.c_str(), &chan.thumb_w, &chan.thumb_h);
        }
      }
    }
  }

}

void LooperPlugin::setupFromState(const nlohmann::json& state) {
  auto comp = resolume::parse_composition(state);
  assignChannelsFromComposition(comp);

  if (state.contains("tempocontroller") &&
      state["tempocontroller"].contains("tempo")) {
    auto& tempo = state["tempocontroller"]["tempo"];
    if (tempo.contains("id")) tempo_id_ = tempo["id"].get<int64_t>();
    if (tempo.contains("value")) bpm_ = tempo["value"].get<double>();
  }
}

void LooperPlugin::subscribeParams() {
  for (int i = 0; i < kNumChannels; ++i) {
    for (const auto& cr : channels_[i].clips) {
      if (cr.connected_id != 0)
        ws_client_.subscribe_by_id(cr.connected_id);
    }
  }
  if (tempo_id_ != 0)
    ws_client_.subscribe_by_id(tempo_id_);
}

void LooperPlugin::processMessages() {
  for (auto& msg : ws_client_.poll()) {
    if (auto* cs = std::get_if<resolume::CompositionState>(&msg)) {
      // New composition state (reconnect or clip change) — release all gates
      // since clip IDs may have changed
      for (int c = 0; c < kNumChannels; ++c) gateOff(c);
      setupFromState(cs->data);
      subscribeParams();
    } else if (auto* pu = std::get_if<resolume::ParameterUpdate>(&msg)) {
      if (pu->id == tempo_id_ && pu->value.is_number())
        bpm_ = pu->value.get<double>();
      // Update connected state for any matching clip
      for (int i = 0; i < kNumChannels; ++i) {
        for (const auto& cr : channels_[i].clips) {
          if (pu->id == cr.connected_id && pu->value.is_string())
            channels_[i].connected_state = pu->value.get<std::string>();
        }
      }
    }
  }
}

// --- Gate helpers ---

void LooperPlugin::gateOn(int ch, int step) {
  if (channels_[ch].clips.empty()) return;
  // Cancel watchdog — we're opening the gate intentionally
  watchdog_active_[ch] = false;
  if (gate_down_[ch]) {
    // Already connected — just extend the gate. Don't send false+true,
    // which causes Resolume race conditions (especially at bar boundaries
    // where step 15→0 fires back-to-back).
    gate_step_[ch] = step;
    gate_timer_[ch] = (float)(60.0 / bpm_ / 4.0);
    return;
  }
  for (const auto& cr : channels_[ch].clips)
    ws_client_.trigger_clip_on(cr.clip_id);
  gate_down_[ch] = true;
  gate_step_[ch] = step;
  gate_timer_[ch] = (float)(60.0 / bpm_ / 4.0);
}

void LooperPlugin::gateOff(int ch) {
  if (!gate_down_[ch]) return;
  for (const auto& cr : channels_[ch].clips)
    ws_client_.trigger_clip_off(cr.clip_id);
  gate_down_[ch] = false;
  gate_step_[ch] = -1;
  gate_timer_[ch] = 0;
  // Start watchdog: keep checking if Resolume actually disconnected.
  // If it still reports "Connected" after 100ms, send another false.
  watchdog_active_[ch] = true;
  watchdog_timer_[ch] = kWatchdogInterval;
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
          last_action_was_clear_all_ = false;
          // Release gate if channel was playing
          gateOff(ch);
        } else if (!mute_held_) {
          // Normal trigger: record + gate on
          last_action_was_clear_all_ = false;
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
          if (last_action_was_clear_all_ && looper_.can_undo()) {
            // Double-tap delete-all = undo
            looper_.undo();
            last_action_was_clear_all_ = false;
          } else {
            looper_.clear_all();
            last_action_was_clear_all_ = true;
          }
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
        last_action_was_clear_all_ = false;
        for (int c = 0; c < kNumChannels; ++c) gateOff(c);
      }
      break;
    case PID_REDO:
      if (rising) {
        looper_.redo();
        last_action_was_clear_all_ = false;
        for (int c = 0; c < kNumChannels; ++c) gateOff(c);
      }
      break;

    case PID_RECORD:
      if (rising) {
        last_action_was_clear_all_ = false;
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

  // Advance looper phase.
  // Prefer host-provided barPhase (from SetBeatInfo) when available —
  // it's perfectly synced to Resolume's transport. Fall back to internal
  // clock for the test harness which doesn't call SetBeatInfo.
  double prev = phase_;
  if (bpm > 0 && barPhase >= 0) {
    // barPhase is 0..1 over one bar (4 beats = 16 sixteenth notes)
    phase_ = static_cast<double>(barPhase) * kNumSteps;
  } else {
    double sixteenths_per_sec = (bpm_ / 60.0) * 4.0;
    phase_ = std::fmod(phase_ + sixteenths_per_sec * dt, static_cast<double>(kNumSteps));
  }

  // Record: clear steps at playhead as it advances
  if (record_held_) {
    int cur_step = static_cast<int>(std::floor(phase_)) % kNumSteps;
    if (cur_step != record_last_step_) {
      // Entered a new step — clear unprotected events here
      for (int ch = 0; ch < kNumChannels; ++ch) {
        if (!step_protected_[ch][cur_step]) {
          looper_.clear_at(ch, cur_step);
          // If we just erased the step that was holding a gate open, release it
          if (gate_down_[ch] && gate_step_[ch] == cur_step)
            gateOff(ch);
        }
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
    // Watchdog: after gate closed, keep checking Resolume's reported state.
    // If it still says "Connected", send another false every 100ms.
    if (watchdog_active_[ch]) {
      watchdog_timer_[ch] -= dt;
      if (watchdog_timer_[ch] <= 0) {
        if (channels_[ch].connected_state == "Connected") {
          // Resolume is stuck — send true+false to unstick
          for (const auto& cr : channels_[ch].clips) {
            ws_client_.trigger_clip_on(cr.clip_id);
            ws_client_.trigger_clip_off(cr.clip_id);
          }
          watchdog_timer_[ch] = kWatchdogInterval; // check again in 100ms
        } else {
          // Resolume confirms disconnected — stop watching
          watchdog_active_[ch] = false;
        }
      }
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
  // If overlay is hidden, skip all rendering (return FF_FAIL = bypass)
  if (param_values_[PID_SHOW_OVERLAY] < 0.5f)
    return FF_FAIL;

  FFGLTextureStruct* tex = pGL->inputTextures[0];
  overlay_.drawPassthrough(tex, currentViewport);

  OverlayState state;
  state.viewport_w = currentViewport.width;
  state.viewport_h = currentViewport.height;
  state.phase = static_cast<float>(phase_);
  // Use host BPM if available, otherwise WS-fetched BPM
  if (bpm > 0) bpm_ = bpm;
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

  // Build watchdog status string
  {
    std::string wd;
    for (int ch = 0; ch < kNumChannels; ++ch) {
      if (watchdog_active_[ch]) {
        if (!wd.empty()) wd += " ";
        wd += "WD" + std::to_string(ch + 1);
        wd += (channels_[ch].connected_state == "Connected") ? ":stuck" : ":ok";
      }
    }
    state.status_extra = std::move(wd);
  }

  for (int ch = 0; ch < kNumChannels; ++ch) {
    state.clip_names[ch] = channels_[ch].name;
    state.clip_connected[ch] = (channels_[ch].connected_state == "Connected");
    state.clip_has_content[ch] = !channels_[ch].clips.empty();
    state.clip_thumbnail[ch] = channels_[ch].thumbnail_tex;
    state.clip_thumb_w[ch] = channels_[ch].thumb_w;
    state.clip_thumb_h[ch] = channels_[ch].thumb_h;
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
