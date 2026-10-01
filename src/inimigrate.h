#pragma once
#include <windows.h>
namespace fgvk {
// fgvk.ini schema version. Existing INIs are never rewritten wholesale (the user's settings
// stay); a version bump instead patches the keys whose old default turned out wrong.
//   1 (no key): written by 0.1.0 - 1.1.0, all of which wrote TagHUDLess=1.
//   2: 1.2.0. TagHUDLess=1 causes camera-orbit flickering (#5), so it becomes 0; PresentPacing
//      is spelled out so it can be found and turned off.
constexpr int kIniVersion = 2;

// Returns the version the file had before migration (kIniVersion = nothing to do).
inline int MigrateIni(const char* path){
  int ver = (int)GetPrivateProfileIntA("fgvk","ConfigVersion",1,path);
  if(ver >= kIniVersion) return ver;
  WritePrivateProfileStringA("fgvk","TagHUDLess","0",path);
  char v[8]{};
  GetPrivateProfileStringA("fgvk","PresentPacing","",v,sizeof(v),path);
  if(!v[0]) WritePrivateProfileStringA("fgvk","PresentPacing","1",path);
  WritePrivateProfileStringA("fgvk","ConfigVersion","2",path);
  return ver;
}
}
