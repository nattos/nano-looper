#pragma once

#include <ffgl/FFGLPluginSDK.h>

// Lightweight FFGL effect that tags a clip with a looper channel number.
// Always returns FF_FAIL from ProcessOpenGL (bypass rendering).
// The main NanoLooper plugin scans the composition for these effects
// to discover which clips are assigned to which channels.

class ChannelTagPlugin : public CFFGLPlugin {
public:
  ChannelTagPlugin();
  ~ChannelTagPlugin() override = default;

  FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
  FFResult SetFloatParameter(unsigned int index, float value) override;
  float GetFloatParameter(unsigned int index) override;

private:
  float channel_value_ = 0.0f; // 0=off, 0.2=ch1, 0.4=ch2, 0.6=ch3, 0.8=ch4
};
