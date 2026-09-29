#pragma once

namespace fgvk {

enum class MfgTransitionPinResult : unsigned int {
  NotRequested = 0,
  Applied,
  Restored,
  AlreadyPinned,
  AlreadyStock,
  UnsupportedBuild,
  AmbiguousBuild,
  ValidationFailed,
  WriteFailed,
};

// Pins the verified Streamline 2.14 x4 feedback transition while x4 is active.
// The target module is resolved from the actual slDLSSGSetOptions function first;
// if that address belongs to a wrapper, loaded modules are matched against a
// closed exact-build table. Unknown or ambiguous builds are never modified.
MfgTransitionPinResult SetMfg4xTransitionPin(void* slDlssGSetOptions, bool pin);
const char* MfgTransitionPinResultName(MfgTransitionPinResult result);

} // namespace fgvk
