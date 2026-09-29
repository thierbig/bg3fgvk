#include "mfg_transition_pin.h"
#include "log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fgvk {
namespace {

struct BuildSpec {
  const char* name;
  DWORD timestamp;
  DWORD imageSize;
  std::array<unsigned char, 32> sha256;
  uintptr_t targetRva;
};

// The proprietary presentCommon implementation is byte-identical at the
// relevant transition in these two verified 2.14 builds.
static constexpr BuildSpec kKnownBuilds[] = {
  {
    "Streamline 2.14.0-rc2 (614ea534a)",
    0x6A8DB8C8u,
    0x0009C000u,
    { 0x73,0xB8,0xA7,0x8A,0x27,0x5B,0x5A,0x3D,0xB5,0x80,0x38,0xA4,0xFF,0x70,0x1A,0x1F,
      0xB4,0x04,0x9D,0xB5,0xC3,0x79,0xEA,0xB9,0x47,0x3A,0x09,0xFA,0x85,0x6B,0xA8,0x4E },
    0x46E30u,
  },
  {
    "Streamline 2.14.1-rc0 (98614dad6)",
    0x6A9AFD6Bu,
    0x0009C000u,
    { 0xF4,0xA6,0xB2,0xB1,0x4D,0xCC,0x0B,0x14,0x85,0x98,0x9E,0x43,0x0D,0x3B,0x4E,0x3A,
      0x44,0xAC,0x18,0x00,0xB9,0x2B,0xA1,0xAD,0x74,0xF4,0x76,0xE6,0x4F,0xB2,0xB0,0x9C },
    0x46E30u,
  },
};

static constexpr uintptr_t kSignatureRva = 0x46E27u;
static constexpr std::array<unsigned char, 9> kSignaturePrefix = {
  0x84,0xDB,0x75,0xB6,0x83,0xFF,0x1E,0x72,0xB1
};
static constexpr std::array<unsigned char, 19> kSignatureSuffix = {
  0xEB,0xAF,0x40,0x84,0xF6,0x74,0x0C,0x41,0x83,0xBE,0x88,0x40,0x00,0x00,0x01,0x41,0x0F,0x43,0xC4
};
static constexpr USHORT kStockWord  = 0x01B3u; // B3 01: mov bl, 1
static constexpr USHORT kPinnedWord = 0x9090u; // 90 90: nop; nop

struct ResolvedBuild {
  HMODULE module{};
  const BuildSpec* spec{};
};

static bool ReadPeIdentity(HMODULE module, DWORD& timestamp, DWORD& imageSize){
  if(!module) return false;
  auto* base = reinterpret_cast<const unsigned char*>(module);
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if(nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
  timestamp = nt->FileHeader.TimeDateStamp;
  imageSize = nt->OptionalHeader.SizeOfImage;
  return true;
}

static bool HashFileSha256(const wchar_t* path, std::array<unsigned char, 32>& digest){
  if(!path || !*path) return false;

  HANDLE file = CreateFileW(path, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if(file == INVALID_HANDLE_VALUE) return false;

  BCRYPT_ALG_HANDLE alg{};
  BCRYPT_HASH_HANDLE hash{};
  std::vector<unsigned char> object;
  bool ok = false;

  if(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0){
    DWORD objectLength = 0, cb = 0;
    if(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                         reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &cb, 0) == 0){
      object.resize(objectLength);
      if(BCryptCreateHash(alg, &hash, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0) == 0){
        std::array<unsigned char, 64 * 1024> buffer{};
        bool streamOk = true;
        for(;;){
          DWORD read = 0;
          if(!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)){
            streamOk = false;
            break;
          }
          if(read == 0) break;
          if(BCryptHashData(hash, buffer.data(), read, 0) != 0){
            streamOk = false;
            break;
          }
        }
        if(streamOk && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0)
          ok = true;
      }
    }
  }

  if(hash) BCryptDestroyHash(hash);
  if(alg) BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(file);
  return ok;
}

static bool ModulePath(HMODULE module, std::wstring& path){
  std::wstring buf(32768, L'\0');
  DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
  if(n == 0 || n >= buf.size() - 1) return false;
  buf.resize(n);
  path = buf;
  return true;
}

static const BuildSpec* MatchKnownBuild(HMODULE module){
  DWORD timestamp = 0, imageSize = 0;
  if(!ReadPeIdentity(module, timestamp, imageSize)) return nullptr;

  bool metadataCandidate = false;
  for(const auto& spec : kKnownBuilds){
    if(spec.timestamp == timestamp && spec.imageSize == imageSize){
      metadataCandidate = true;
      break;
    }
  }
  if(!metadataCandidate) return nullptr;

  std::wstring path;
  if(!ModulePath(module, path)) return nullptr;
  std::array<unsigned char, 32> digest{};
  if(!HashFileSha256(path.c_str(), digest)) return nullptr;

  for(const auto& spec : kKnownBuilds){
    if(spec.timestamp == timestamp && spec.imageSize == imageSize && digest == spec.sha256)
      return &spec;
  }
  return nullptr;
}

static bool HoldModule(HMODULE module, HMODULE& held){
  held = nullptr;
  if(!module) return false;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(module), &held) != FALSE;
}

static MfgTransitionPinResult ResolveKnownBuild(void* slDlssGSetOptions, ResolvedBuild& out){
  HMODULE direct = nullptr;
  if(slDlssGSetOptions &&
     GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(slDlssGSetOptions), &direct)){
    if(const BuildSpec* spec = MatchKnownBuild(direct)){
      HMODULE held{};
      if(!HoldModule(direct, held)) return MfgTransitionPinResult::ValidationFailed;
      out = { held, spec };
      Log("MFG4xTransitionPin: resolved directly to %s", spec->name);
      return MfgTransitionPinResult::NotRequested;
    }
    Log("MFG4xTransitionPin: slDLSSGSetOptions owner is not a verified sl.dlss_g build; checking loaded modules");
  }

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
  if(snapshot == INVALID_HANDLE_VALUE) return MfgTransitionPinResult::UnsupportedBuild;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  HMODULE matchModule = nullptr;
  const BuildSpec* matchSpec = nullptr;
  unsigned matches = 0;

  if(Module32FirstW(snapshot, &entry)){
    do {
      if(const BuildSpec* spec = MatchKnownBuild(entry.hModule)){
        ++matches;
        matchModule = entry.hModule;
        matchSpec = spec;
      }
    } while(Module32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);

  if(matches == 0) return MfgTransitionPinResult::UnsupportedBuild;
  if(matches != 1) return MfgTransitionPinResult::AmbiguousBuild;

  HMODULE held{};
  if(!HoldModule(matchModule, held)) return MfgTransitionPinResult::ValidationFailed;
  out = { held, matchSpec };
  Log("MFG4xTransitionPin: resolved exactly one verified loaded module: %s", matchSpec->name);
  return MfgTransitionPinResult::NotRequested;
}

static bool ValidateLocalShape(const ResolvedBuild& build, USHORT& current){
  if(!build.module || !build.spec) return false;
  auto* base = reinterpret_cast<const unsigned char*>(build.module);
  auto* prefix = base + kSignatureRva;
  auto* target = base + build.spec->targetRva;
  auto* suffix = target + 2;

  if(std::memcmp(prefix, kSignaturePrefix.data(), kSignaturePrefix.size()) != 0) return false;
  if(std::memcmp(suffix, kSignatureSuffix.data(), kSignatureSuffix.size()) != 0) return false;
  std::memcpy(&current, target, sizeof(current));
  return current == kStockWord || current == kPinnedWord;
}

static MfgTransitionPinResult WriteTransitionWord(const ResolvedBuild& build, bool pin){
  USHORT current = 0;
  if(!ValidateLocalShape(build, current)) return MfgTransitionPinResult::ValidationFailed;

  const USHORT desired = pin ? kPinnedWord : kStockWord;
  if(current == desired)
    return pin ? MfgTransitionPinResult::AlreadyPinned : MfgTransitionPinResult::AlreadyStock;

  auto* target = reinterpret_cast<unsigned char*>(build.module) + build.spec->targetRva;
  if((reinterpret_cast<uintptr_t>(target) & 1u) != 0) return MfgTransitionPinResult::ValidationFailed;

  DWORD oldProtect = 0;
  if(!VirtualProtect(target, sizeof(USHORT), PAGE_EXECUTE_READWRITE, &oldProtect))
    return MfgTransitionPinResult::WriteFailed;

  InterlockedExchange16(reinterpret_cast<volatile SHORT*>(target), static_cast<SHORT>(desired));

  bool flushed = FlushInstructionCache(GetCurrentProcess(), target, sizeof(USHORT)) != FALSE;
  DWORD ignored = 0;
  bool protectedAgain = VirtualProtect(target, sizeof(USHORT), oldProtect, &ignored) != FALSE;

  USHORT after = 0;
  std::memcpy(&after, target, sizeof(after));
  if(!flushed || !protectedAgain || after != desired) return MfgTransitionPinResult::WriteFailed;
  return pin ? MfgTransitionPinResult::Applied : MfgTransitionPinResult::Restored;
}

} // namespace

MfgTransitionPinResult SetMfg4xTransitionPin(void* slDlssGSetOptions, bool pin){
  static bool attempted = false;
  static ResolvedBuild build{};
  static MfgTransitionPinResult resolution = MfgTransitionPinResult::UnsupportedBuild;
  static bool resolutionLogged = false;

  if(!attempted){
    attempted = true;
    resolution = ResolveKnownBuild(slDlssGSetOptions, build);
  }

  if(!build.module || !build.spec){
    if(!resolutionLogged){
      resolutionLogged = true;
      Log("MFG4xTransitionPin: unavailable (%s); Streamline bytes left untouched",
          MfgTransitionPinResultName(resolution));
    }
    return resolution;
  }

  return WriteTransitionWord(build, pin);
}

const char* MfgTransitionPinResultName(MfgTransitionPinResult result){
  switch(result){
    case MfgTransitionPinResult::NotRequested:     return "not-requested";
    case MfgTransitionPinResult::Applied:          return "applied";
    case MfgTransitionPinResult::Restored:         return "restored";
    case MfgTransitionPinResult::AlreadyPinned:    return "already-pinned";
    case MfgTransitionPinResult::AlreadyStock:     return "already-stock";
    case MfgTransitionPinResult::UnsupportedBuild: return "unsupported-build";
    case MfgTransitionPinResult::AmbiguousBuild:   return "ambiguous-build";
    case MfgTransitionPinResult::ValidationFailed: return "validation-failed";
    case MfgTransitionPinResult::WriteFailed:      return "write-failed";
  }
  return "unknown";
}

} // namespace fgvk
