#include "channel_tag_plugin.h"

// --- Plugin registration ---

static CFFGLPluginInfo PluginInfo(
    PluginFactory<ChannelTagPlugin>,
    "NLCH",                    // 4-char unique ID
    "NanoLooper Ch",           // Plugin name (short for Resolume UI)
    2, 1,                      // API version
    1, 0,                      // Plugin version
    FF_EFFECT,                 // Plugin type
    "Tags this clip for NanoLooper channel assignment",
    "nattos"
);

// --- Construction ---

ChannelTagPlugin::ChannelTagPlugin() : CFFGLPlugin(false) {
  SetMinInputs(1);
  SetMaxInputs(1);

  // Option param: Off, 1, 2, 3, 4
  SetOptionParamInfo(0, "Channel", 5, 0.0f);
  SetParamElementInfo(0, 0, "Off",       0.0f);
  SetParamElementInfo(0, 1, "Channel 1", 0.2f);
  SetParamElementInfo(0, 2, "Channel 2", 0.4f);
  SetParamElementInfo(0, 3, "Channel 3", 0.6f);
  SetParamElementInfo(0, 4, "Channel 4", 0.8f);
}

// --- Rendering (always bypass) ---

FFResult ChannelTagPlugin::ProcessOpenGL(ProcessOpenGLStruct*) {
  return FF_FAIL; // bypass — no rendering, no alpha blend overhead
}

// --- Parameters ---

FFResult ChannelTagPlugin::SetFloatParameter(unsigned int index, float value) {
  if (index != 0) return FF_FAIL;
  channel_value_ = value;
  return FF_SUCCESS;
}

float ChannelTagPlugin::GetFloatParameter(unsigned int index) {
  if (index != 0) return 0.0f;
  return channel_value_;
}
