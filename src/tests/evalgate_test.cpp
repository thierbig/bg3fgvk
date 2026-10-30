// The eval-driven DLSS-G gate: when frame generation turns on and off, and that it never turns
// on when Streamline did not hand us DLSS-G.
#include "evalgate.h"
#include <cstdio>
static int fails = 0;
static void expectStep(const char* name, fgvk::GateStep got, fgvk::GateStep want){
  if (got != want){ printf("FAIL %s: got %d want %d\n", name, (int)got, (int)want); fails++; } else printf("ok   %s\n", name);
}
// n presents of the same kind; returns the step of the last one, or the first non-Stay step.
static fgvk::GateStep run(fgvk::GateStreaks& g, int n, bool eval, bool on, bool available){
  for (int i = 0; i < n; ++i){
    auto s = g.Step(eval, on, available, 60, 30);
    if (s != fgvk::GateStep::Stay || i == n - 1) return s;
  }
  return fgvk::GateStep::Stay;
}
int test_evalgate(){
  using fgvk::GateStep; using fgvk::GateStreaks;
  // 60 consecutive DLSS-SR frames turn generation on, not 59.
  { GateStreaks g; expectStep("59 eval frames stay off", run(g, 59, true, false, true), GateStep::Stay);
    expectStep("60th eval frame turns on", g.Step(true, false, true, 60, 30), GateStep::TurnOn); }
  // A present without DLSS-SR (menu, loading) restarts the count.
  { GateStreaks g; run(g, 59, true, false, true); run(g, 1, false, false, true);
    expectStep("idle frame restarts the eval count", run(g, 59, true, false, true), GateStep::Stay);
    expectStep("then 60 more turn on", g.Step(true, false, true, 60, 30), GateStep::TurnOn); }
  // 30 presents without DLSS-SR suspend generation while it is on.
  { GateStreaks g; expectStep("29 idle frames keep it on", run(g, 29, false, true, true), GateStep::Stay);
    expectStep("30th idle frame turns off", g.Step(false, true, true, 60, 30), GateStep::TurnOff); }
  // An eval frame restarts the idle count.
  { GateStreaks g; run(g, 29, false, true, true); run(g, 1, true, true, true);
    expectStep("eval frame restarts the idle count", run(g, 29, false, true, true), GateStep::Stay); }
  // Nothing to suspend while already off.
  { GateStreaks g; expectStep("idle while off stays", run(g, 100, false, false, true), GateStep::Stay); }
  // Issue #9: Streamline dropped sl.dlss_g, so there is no DLSS-G. The gate must not claim it is
  // on - that false "on" also made PresentPacing wait for the GPU before every present.
  { GateStreaks g; expectStep("unavailable: 59 eval frames stay", run(g, 59, true, false, false), GateStep::Stay);
    expectStep("unavailable: 60th reports unavailable", g.Step(true, false, false, 60, 30), GateStep::Unavailable);
    int turnedOn = 0; for (int i = 0; i < 1000; ++i) if (g.Step(true, false, false, 60, 30) == GateStep::TurnOn) turnedOn++;
    if (turnedOn){ printf("FAIL unavailable: turned on %d times in 1000 eval frames\n", turnedOn); fails++; }
    else printf("ok   unavailable: never turns on\n"); }
  return fails;
}
