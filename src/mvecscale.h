#pragma once
#include <cstdint>
namespace fgvk {
struct MvecScale { float x, y; };
// The sl::Constants::mvecScale value to send to Streamline for the game's DLSS-SR motion-vector
// scale (rawX, rawY) and the motion-vector buffer extent (w, h).
//
// NGX DLSS-SR takes a scale that turns buffer values into PIXELS (BG3 passes -1,-1: pixel-space,
// sign flipped). Streamline's constant is the factor that normalises the buffer into [-1,1]; its
// DLSS-G plugin multiplies it back by the mvec extent before the driver's DLSS-G feature sees it
// (SE-NgxTap.log: a raw -1 constant reached the driver as -1707/-960 at 1707x960, while
// PureDark's direct NGX recipe passed -1,-1). So the constant must be raw / extent.
inline MvecScale MvecScaleForStreamline(float rawX, float rawY, uint32_t w, uint32_t h, bool normalized){
  if (!normalized || w == 0 || h == 0) return { rawX, rawY };
  return { rawX / (float)w, rawY / (float)h };
}
}
