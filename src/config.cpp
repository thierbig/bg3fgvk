#include "config.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
namespace fgvk {
static Config g_cfg;
static Runtime g_rt;
const Config& Cfg(){ return g_cfg; }
Runtime& Rt(){ return g_rt; }

// Edge-triggered key polling on the present thread. GetAsyncKeyState is cheap (no message pump).
static bool Pressed(int vk, bool& was){
  if(vk<=0) return false;
  bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  bool edge = down && !was; was = down; return edge;
}
bool PollHotkeys(){
  static bool wT=false, wC=false, wH=false;
  bool changed=false;
  if(Pressed(g_cfg.keyToggleFG, wT)){ g_rt.fgUserOff = !g_rt.fgUserOff; Log("hotkey: DLSS-G %s by user", g_rt.fgUserOff?"OFF":"ON"); changed=true; }
  if(Pressed(g_cfg.keyCycleFrames, wC)){ g_rt.frames = g_rt.frames>=3 ? 1 : g_rt.frames+1; Log("hotkey: generated frames -> %u (x%u)", g_rt.frames, g_rt.frames+1); changed=true; }
  if(Pressed(g_cfg.keyToggleHudless, wH)){ g_rt.tagHudless = !g_rt.tagHudless; Log("hotkey: HUD-less tag %s", g_rt.tagHudless?"ON":"OFF"); changed=true; }
  return changed;
}

static void IniPath(char* out, size_t n){
  HMODULE self{}; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)&IniPath,&self);
  char p[MAX_PATH]{}; GetModuleFileNameA(self,p,MAX_PATH); char* s=strrchr(p,'\\'); if(s) *(s+1)=0;
  snprintf(out,n,"%sfgvk.ini",p);
}

static void WriteDefaults(const char* path){
  FILE* f=nullptr; if(fopen_s(&f,path,"w")!=0 || !f) return;
  fputs(
    "; fgvk - DLSS Frame Generation for Baldur's Gate 3 (Vulkan). Edit and restart the game.\n"
    "[fgvk]\n"
    "; Generated frames per real frame: 1 = x2, 2 = x3, 3 = x4 (clamped to what the GPU supports)\n"
    "DLSSGFrames=3\n"
    "; Reflex: 0 = off, 1 = low latency, 2 = low latency + boost\n"
    "ReflexMode=2\n"
    "; Call slReflexSleep once per frame (1) - required by NVIDIA's Reflex checklist\n"
    "ReflexSleep=1\n"
    "; Feed the DLSS-SR output (R16G16B16A16_SFLOAT, display res) as the HUD-less color buffer.\n"
    "; PureDark's captured recipe tags exactly this image every frame; it is what keeps the static HUD from\n"
    "; smearing over a moving background. 0 = backbuffer only (more ghosting on fast camera moves).\n"
    "TagHUDLess=1\n"
    "; Feed a fully transparent UI color+alpha layer (0 = none)\n"
    "TagUI=0\n"
    "; DLSS-G turns on after this many consecutive frames with a DLSS-SR evaluate (world rendering)\n"
    "OnAfterEvalFrames=60\n"
    "; ...and suspends after this many presents without one (menu / loading screen / video)\n"
    "OffAfterIdleFrames=30\n"
    "; Hotkeys as Windows virtual-key codes (decimal or 0x hex; 0 disables). 106 = numpad *, 35 = End, 36 = Home\n"
    "KeyToggleFG=0x6A\n"
    "KeyCycleFrames=0x23\n"
    "KeyToggleHUDLess=0x24\n", f);
  fclose(f);
}

void LoadConfig(){
  char path[MAX_PATH]; IniPath(path,sizeof(path));
  if(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES) WriteDefaults(path);
  Config c;
  c.dlssgFrames = (uint32_t)GetPrivateProfileIntA("fgvk","DLSSGFrames",(int)c.dlssgFrames,path);
  if(c.dlssgFrames<1) c.dlssgFrames=1; if(c.dlssgFrames>3) c.dlssgFrames=3;
  c.reflexMode  = GetPrivateProfileIntA("fgvk","ReflexMode",c.reflexMode,path);
  if(c.reflexMode<0||c.reflexMode>2) c.reflexMode=2;
  c.reflexSleep = GetPrivateProfileIntA("fgvk","ReflexSleep",c.reflexSleep?1:0,path)!=0;
  c.tagHudless  = GetPrivateProfileIntA("fgvk","TagHUDLess",c.tagHudless?1:0,path)!=0;
  c.tagUI       = GetPrivateProfileIntA("fgvk","TagUI",c.tagUI?1:0,path)!=0;
  c.onAfterEvalFrames  = (uint32_t)GetPrivateProfileIntA("fgvk","OnAfterEvalFrames",(int)c.onAfterEvalFrames,path);
  c.offAfterIdleFrames = (uint32_t)GetPrivateProfileIntA("fgvk","OffAfterIdleFrames",(int)c.offAfterIdleFrames,path);
  if(c.onAfterEvalFrames<1) c.onAfterEvalFrames=1; if(c.offAfterIdleFrames<1) c.offAfterIdleFrames=1;
  char kb[32];
  auto key=[&](const char* k, int def){ GetPrivateProfileStringA("fgvk",k,"",kb,sizeof(kb),path); if(!kb[0]) return def; return (int)strtol(kb,nullptr,0); };
  c.keyToggleFG = key("KeyToggleFG", c.keyToggleFG);
  c.keyCycleFrames = key("KeyCycleFrames", c.keyCycleFrames);
  c.keyToggleHudless = key("KeyToggleHUDLess", c.keyToggleHudless);
  g_cfg=c;
  g_rt.frames = c.dlssgFrames; g_rt.tagHudless = c.tagHudless; g_rt.fgUserOff = false;
  Log("config %s: DLSSGFrames=%u (x%u) ReflexMode=%d ReflexSleep=%d TagHUDLess=%d TagUI=%d OnAfterEvalFrames=%u OffAfterIdleFrames=%u keys: toggleFG=0x%x cycle=0x%x hudless=0x%x",
      path, c.dlssgFrames, c.dlssgFrames+1, c.reflexMode, (int)c.reflexSleep, (int)c.tagHudless, (int)c.tagUI, c.onAfterEvalFrames, c.offAfterIdleFrames,
      c.keyToggleFG, c.keyCycleFrames, c.keyToggleHudless);
}
}
