#pragma once
namespace fgvk {
enum class ExportView { Real, Game };
// Which view a vulkan-1 EXPORT lookup (vkGetDeviceProcAddr) gets.
//  forceReal:            the call is re-entrant, i.e. made while we are inside an interposer call
//  inLoaderCreateDevice: the call is a third party's, made inside the loader's exported vkCreateDevice
// Streamline resolves its own device table only AFTER the loader's vkCreateDevice returns, so a
// lookup that arrives inside that call comes from a third party's vkCreateDevice export detour
// (OptiScaler's HookDevice). It must get the game's view: its present hook then lands on fgvk's
// wrapper on the game thread, above DLSS-G, instead of on the driver's present under Streamline's
// frame-pacing thread (the OptiScaler-menu deadlock).
inline ExportView ExportRouting(bool forceReal, bool inLoaderCreateDevice){
  if (inLoaderCreateDevice) return ExportView::Game;
  return forceReal ? ExportView::Real : ExportView::Game;
}
// True when a vulkan-1 export lookup is made by a third party inside the loader's vkCreateDevice.
//  loaderCreateDepth:    >0 while the loader's exported vkCreateDevice executes on this thread
//  reentryNow:           our re-entry depth at the lookup
//  reentryAtWindowOpen:  our re-entry depth when that vkCreateDevice call started
inline bool InLoaderCreateWindow(int loaderCreateDepth, int reentryNow, int reentryAtWindowOpen){
  // A deeper re-entry means one of OUR interposer-forwarding scopes was pushed after the window
  // opened: Streamline forwarding to the loader while we resolve the third party's game view.
  // Routing that to the game view again recurses through the same export until the stack is gone
  // (crash of 2026-09-05 11:49: 4881 turns of hook -> GameViewGDPA -> sl.interposer -> loader).
  return loaderCreateDepth > 0 && reentryNow == reentryAtWindowOpen;
}
}
