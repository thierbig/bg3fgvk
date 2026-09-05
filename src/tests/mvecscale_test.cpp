// Unit test for the Streamline motion-vector scale constant. Built as fgvk-tests.exe.
#include "mvecscale.h"
#include <cstdio>
#include <cmath>
static int fails = 0;
static void expectNear(const char* name, float got, float want){
  if (fabsf(got - want) > 1e-7f){ printf("FAIL %s: got %.9g want %.9g\n", name, got, want); fails++; }
  else printf("ok   %s\n", name);
}
int main(){
  // BG3 hands DLSS-SR mvScale (-1,-1): pixel-space vectors, sign flipped. Streamline wants the
  // factor that normalises the buffer into [-1,1]; its DLSS-G plugin multiplies that factor back
  // by the mvec extent before the driver sees it, so to land at (-1,-1) again the constant must
  // be raw / extent (PureDark's direct NGX recipe passed -1,-1; SE-NgxTap.log shows the
  // Streamline path turning a raw -1 into -1707/-960 at 1707x960).
  auto s = fgvk::MvecScaleForStreamline(-1.f, -1.f, 1505, 847, true);
  expectNear("normalized x = -1/1505", s.x, -1.f / 1505.f);
  expectNear("normalized y = -1/847",  s.y, -1.f / 847.f);
  auto r = fgvk::MvecScaleForStreamline(-1.f, -1.f, 1505, 847, false);
  expectNear("raw mode keeps x", r.x, -1.f);
  expectNear("raw mode keeps y", r.y, -1.f);
  auto z = fgvk::MvecScaleForStreamline(-1.f, -1.f, 0, 0, true);
  expectNear("zero extent falls back to raw x", z.x, -1.f);
  expectNear("zero extent falls back to raw y", z.y, -1.f);
  printf("%d failure(s)\n", fails);
  return fails ? 1 : 0;
}
