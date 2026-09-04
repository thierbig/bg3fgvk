#pragma once
#include <cstdint>
namespace fgvk {
// fgvk.ini next to fgvk.dll (written with defaults on first run). Read once at DLL attach.
struct Config {
  uint32_t dlssgFrames = 3;      // generated frames per real frame: 1 = x2, 2 = x3, 3 = x4
  int      reflexMode  = 2;      // 0 off, 1 low latency, 2 low latency + boost (PureDark's default)
  bool     reflexSleep = true;   // slReflexSleep once per frame (Reflex checklist: always on)
  bool     tagHudless  = false;  // tag the DLSS-SR output as HUD-less color (pre-tonemap; off = backbuffer only, PureDark parity)
  bool     tagUI       = false;  // tag a transparent UI color+alpha layer
  uint32_t onAfterEvalFrames  = 60;   // DLSS-SR frames before DLSS-G turns on
  uint32_t offAfterIdleFrames = 30;   // presents without DLSS-SR before DLSS-G suspends
};
const Config& Cfg();
void LoadConfig();
}
