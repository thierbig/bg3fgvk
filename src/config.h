#pragma once
#include <cstdint>
namespace fgvk {
// fgvk.ini next to fgvk.dll (written with defaults on first run). Read once at DLL attach.
struct Config {
  uint32_t dlssgFrames = 3;      // generated frames per real frame: 1 = x2, 2 = x3, 3 = x4
  int      reflexMode  = 2;      // 0 off, 1 low latency, 2 low latency + boost (PureDark's default)
  bool     reflexSleep = true;   // slReflexSleep once per frame (Reflex checklist: always on)
  bool     tagHudless  = true;   // tag the DLSS-SR output as HUD-less color (PureDark's recipe tags it every frame)
  bool     tagUI       = false;  // tag a transparent UI color+alpha layer
  uint32_t onAfterEvalFrames  = 60;   // DLSS-SR frames before DLSS-G turns on
  uint32_t offAfterIdleFrames = 30;   // presents without DLSS-SR before DLSS-G suspends
  // Hotkeys (Windows virtual-key codes, 0 = disabled). Defaults follow PureDark's INI: numpad * toggles
  // FG, End cycles x2/x3/x4; Home toggles the HUD-less tag for live A/B.
  int keyToggleFG   = 0x6A;   // VK_MULTIPLY
  int keyCycleFrames= 0x23;   // VK_END
  int keyToggleHudless = 0x24; // VK_HOME
};
const Config& Cfg();
void LoadConfig();
// Runtime overrides (hotkeys). Read on the present thread; changes take effect next present.
struct Runtime {
  bool     fgUserOff = false;   // user toggled FG off
  uint32_t frames = 3;          // current generated-frames setting
  bool     tagHudless = true;   // current HUD-less tagging
};
Runtime& Rt();
// Poll hotkeys (call once per present on the present thread). Returns true if anything changed.
bool PollHotkeys();
}
