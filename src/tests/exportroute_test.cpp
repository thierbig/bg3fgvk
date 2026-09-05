// Routing of vulkan-1 EXPORT lookups (vkGetDeviceProcAddr) made by third parties.
#include "exportroute.h"
#include <cstdio>
static int fails = 0;
static void expectBool(const char* name, bool got, bool want){
  if (got != want){ printf("FAIL %s: got %d want %d\n", name, (int)got, (int)want); fails++; } else printf("ok   %s\n", name);
}
static void expectView(const char* name, fgvk::ExportView got, fgvk::ExportView want){
  if (got != want){ printf("FAIL %s: got %s want %s\n", name, got==fgvk::ExportView::Game?"Game":"Real", want==fgvk::ExportView::Game?"Game":"Real"); fails++; }
  else printf("ok   %s\n", name);
}
int test_exportroute(){
  using fgvk::ExportView; using fgvk::ExportRouting;
  // Outside any interposer call: overlays, the Script Extender -> the game's view (fgvk wrappers).
  expectView("outside interposer -> game view", ExportRouting(false, false), ExportView::Game);
  // Streamline forwarding to the driver (re-entrant) -> real loader, or it would recurse into us.
  expectView("interposer forwarding -> real", ExportRouting(true, false), ExportView::Real);
  // Inside the LOADER's vkCreateDevice (a third party's own vkCreateDevice export detour is the
  // only code that resolves functions there; Streamline builds its table after the loader
  // returns) -> game view, so their present hook lands on fgvk's wrapper, above DLSS-G, not on
  // the driver's present under Streamline's pacer (the OptiScaler menu deadlock).
  expectView("third party inside loader vkCreateDevice -> game view", ExportRouting(true, true), ExportView::Game);
  expectView("loader window without re-entry -> game view", ExportRouting(false, true), ExportView::Game);
  // The window must not swallow the interposer's OWN forwarding that happens while we resolve the
  // third party's game view (our GameViewGDPA pushes a re-entry scope, Streamline forwards unknown
  // names back through the same export). Only a lookup at the re-entry depth the window opened
  // with is the third party's; a deeper one is Streamline's and must go real, or it recurses until
  // the stack is gone (the 11:49 crash: write AV just above RSP inside the loader vkCreateDevice).
  using fgvk::InLoaderCreateWindow;
  expectBool("window open, same depth -> third party", InLoaderCreateWindow(1, 1, 1), true);
  expectBool("window open, nested re-entry -> not third party", InLoaderCreateWindow(1, 2, 1), false);
  expectBool("window closed -> not third party", InLoaderCreateWindow(0, 1, 1), false);
  expectBool("window opened outside any re-entry, same depth -> third party", InLoaderCreateWindow(1, 0, 0), true);
  return fails;
}
