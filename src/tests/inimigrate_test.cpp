// Unit test for the fgvk.ini migration, on real INI files in %TEMP%.
#include "inimigrate.h"
#include <cstdio>
static int fails = 0;
static void expect(const char* name, bool ok){
  if(!ok){ printf("FAIL %s\n", name); fails++; } else printf("ok   %s\n", name);
}
static void WriteIni(const char* path, const char* text){
  FILE* f=nullptr; fopen_s(&f,path,"w"); if(f){ fputs(text,f); fclose(f); }
}
static int Int(const char* path, const char* key){ return (int)GetPrivateProfileIntA("fgvk",key,-1,path); }

int test_inimigrate(){
  char dir[MAX_PATH]; GetTempPathA(MAX_PATH,dir);
  char path[MAX_PATH]; snprintf(path,sizeof(path),"%sfgvk-inimigrate-test.ini",dir);

  // An INI as 1.1.0 wrote it (TagHUDLess=1 default) with a user setting that must survive.
  WriteIni(path,"[fgvk]\nDLSSGFrames=2\nTagHUDLess=1\nTagUI=0\n");
  expect("1.1 ini reports version 1", fgvk::MigrateIni(path)==1);
  expect("TagHUDLess -> 0", Int(path,"TagHUDLess")==0);
  expect("PresentPacing spelled out as 1", Int(path,"PresentPacing")==1);
  expect("ConfigVersion=2", Int(path,"ConfigVersion")==2);
  expect("user's DLSSGFrames kept", Int(path,"DLSSGFrames")==2);
  expect("second run is a no-op", fgvk::MigrateIni(path)==fgvk::kIniVersion);

  // After migrating, the user's own choices win again.
  WritePrivateProfileStringA("fgvk","TagHUDLess","1",path);
  fgvk::MigrateIni(path);
  expect("TagHUDLess=1 chosen after migration is kept", Int(path,"TagHUDLess")==1);

  // An old INI where the user already turned pacing off (tested a pre-release) keeps it off.
  WriteIni(path,"[fgvk]\nTagHUDLess=1\nPresentPacing=0\n");
  fgvk::MigrateIni(path);
  expect("explicit PresentPacing=0 kept", Int(path,"PresentPacing")==0);

  // A fresh 1.2 INI carries ConfigVersion=2 and is left alone.
  WriteIni(path,"[fgvk]\nConfigVersion=2\nTagHUDLess=1\n");
  expect("current ini untouched", fgvk::MigrateIni(path)==2 && Int(path,"TagHUDLess")==1);

  DeleteFileA(path);
  return fails;
}
