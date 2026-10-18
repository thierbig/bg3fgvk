#pragma once
#include <cstdint>
namespace fgvk {
// The eval-driven DLSS-G gate's decision, one call per present (vkhooks EvalGate applies it).
// DLSS-G turns on after onAfter consecutive presents that ran a DLSS-SR evaluate (the game is
// rendering the world) and is suspended after offAfter presents without one (menus, loading
// screens, videos). Never by frame time: see EvalGate.
// Without DLSS-G (Streamline dropped sl.dlss_g, issue #9) the gate reports Unavailable where it
// would have turned on: a false "on" also made PresentPacing wait for the GPU every present.
enum class GateStep { Stay, TurnOn, TurnOff, Unavailable };
struct GateStreaks {
  uint32_t eval = 0, idle = 0;
  GateStep Step(bool evalThisFrame, bool on, bool dlssgAvailable, uint32_t onAfter, uint32_t offAfter){
    if (evalThisFrame){
      idle = 0;
      if (eval < 100000) eval++;
      if (!on && eval >= onAfter) return dlssgAvailable ? GateStep::TurnOn : GateStep::Unavailable;
    } else {
      eval = 0;
      if (idle < 100000) idle++;
      if (on && idle >= offAfter) return GateStep::TurnOff;
    }
    return GateStep::Stay;
  }
  void Reset(){ eval = 0; idle = 0; }
};
}
