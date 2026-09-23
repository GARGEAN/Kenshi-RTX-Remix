#include "../util/util_kenshi_telemetry.h"
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <windows.h>
#include "../util/log/log.h"

namespace dxvk::kenshi_executable {
// V794: offsets verified against stock 1.0.68 and the exact 1.0.65 executable
// produced by RE_Kenshi 0.3.5's official courgette patch. Whole-file MD5 of the
// latter: df4a5a7ef8a29deb24b70e7b7f4a222a. No checksum bypass or pattern hook.
struct Guard { uint32_t rva; const char* hex; };
struct Profile {
  const char* name;
  uint32_t timestamp, imageSize;
  uint32_t game, player, moons, originUpdate, originSetter, blood;
  uint32_t rigidType, rigidVtable, rigidCol, rigidMethod2, rigidMethod8, rigidMethod9;
  uint32_t setPositionIat;
  const char* originPrefix;
  std::array<Guard, 9> cameraGuards;
  std::array<Guard, 8> cameraPatches;
  std::array<Guard, 8> moonGuards;
};
inline const Profile profiles[] = {
  {
    "Steam 1.0.68", 0x6602d59d, 0x232d000,
    0x2133308, 0x2134690, 0x21303c0, 0x82b6e0, 0x82b96a, 0x8e158e, 0x20fcd28, 0x174fbf8, 0x186c5e0, 0xa5b7d0, 0xa57a60, 0xa28b18, 0x2247ee8,
    "488bc4554154415541564157488da818feff",
    {{{0x6af910, "40534883ec20488bd9488b4958ff15c585b90133d2488bcbe829be97ff488b0d"}, {0x6ae520, "8b42084883c1488941e88b420c8941ec8b42108941f08b42148941f48b421889"}, {0x7ade60, "40555657488d6c24b94881ecc000000048c745e7feffffff48899c24f0000000"}, {0x6b1480, "488bc4574881ece000000048c7842480000000feffffff48895810488970180f"}, {0x6b0f90, "40555357488bec4881ec8000000080b9bf000000000fb6fa488bd9740f4881c4"}, {0x7f26c0, "4883ec2848895130488b0d390c9401488b4970ff155755a5"}, {0x6ae560, "8b059ac078014883c1488941e88b0591c078018941ec8b05"}, {0x6b1300, "488b07488d55b0488bcfff5040"}, {0x6b135a, "488b4b58488bd0ff15816bb901488b4b58ff15977cb90148"}}},
    {{{0x25c2f, "e9dc9c6800"}, {0x516a4, "e977ce6500"}, {0x21571, "e9eac87800"}, {0x50619, "e9620e6600"}, {0x38fe6, "e9a57f6700"}, {0x71c6, "e9f5b47e00"}, {0x4272b, "e930be6600"}, {0x6b1361, "ff15816bb901"}}},
    {{{0x66ef61, "4c8d85d0010000488d95b8000000488bcbe852339affeb03498bc54889053d14ac01"}, {0x66f0a2, "4c8d8520020000488d5540488bcbe814329affeb03498bc54889050713ac01"}, {0x122c9, "e952a66500"}, {0x66c9dc, "498bd4e875a59aff4889034c8b051aa8bd01488bd5488bc8ff15e6b4bd0133d2488b0bff1503bcbd01"}, {0x66ca16, "488b0b488b01450fb7c3b206ff50"}, {0x16fd718, "4d6f6f6e3200"}, {0x16fd720, "706c616e657430312e6d65736800"}, {0x16fd730, "4d6f6f6e00"}}}
  },
  {
    "Steam 1.0.65 (RE_Kenshi)", 0x65d604d7, 0x232c000,
    0x21322b8, 0x2133630, 0x212f3b0, 0x82aaf0, 0x82ad4d, 0x8e06be, 0x20fbd18, 0x174ec58, 0x186b640, 0xa5a900, 0xa56b90, 0xa27c48, 0x2246ec0,
    "488bc4554154415541564157488da838feff",
    {{{0x6b00f0, "40534883ec20488bd9488b4958ff15bd6db90133d2488bcbe830b697ff488b0d"}, {0x6aed00, "8b42084883c1488941e88b420c8941ec8b42108941f08b42148941f48b421889"}, {0x7ad2c0, "40555657488d6c24b94881ecc000000048c745e7feffffff48899c24f0000000"}, {0x6b1a30, "488bc4574881ece000000048c7842480000000feffffff48895810488970180f"}, {0x6b1540, "40555357488bec4881ec8000000080b9bf000000000fb6fa488bd9740f4881c4"}, {0x7f1b20, "4883ec2848895130488b0d89079401488b4970ff15d750a5"}, {0x6aed40, "8b05baa878014883c1488941e88b05b1a878018941ec8b05"}, {0x6b18b0, "488b07488d55b0488bcfff5040"}, {0x6b190a, "488b4b58488bd0ff15a955b901488b4b58ff15bf66b90148"}}},
    {{{0x25c25, "e9c6a46800"}, {0x51668, "e993d66500"}, {0x21567, "e954bd7800"}, {0x505dd, "e94e146600"}, {0x38fc3, "e978856700"}, {0x71c1, "e95aa97e00"}, {0x42703, "e938c66600"}, {0x6b1911, "ff15a955b901"}}},
    {{{0x66e4d1, "4c8d85d0010000488d95b8000000488bcbe8d83d9affeb03498bc5488905bd0eac01"}, {0x66e612, "4c8d8520020000488d5540488bcbe89a3c9affeb03498bc5488905870dac01"}, {0x122bf, "e9cc9b6500"}, {0x66bf4c, "498bd4e8fbaf9aff4889034c8b058aa2bd01488bd5488bc8ff154eafbd0133d2488b0bff156bb6bd01"}, {0x66bf86, "488b0b488b01450fb7c3b206ff50"}, {0x16fc5e8, "4d6f6f6e3200"}, {0x16fc5f0, "706c616e657430312e6d65736800"}, {0x16fc600, "4d6f6f6e00"}}}
  }
};
inline const Profile* identify(const uint8_t* base) {
  if (!base) return nullptr;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (pe->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  for (const auto& p : profiles)
    if (pe->FileHeader.TimeDateStamp == p.timestamp && pe->OptionalHeader.SizeOfImage == p.imageSize)
      return &p;
  return nullptr;
}
inline bool matches(const uint8_t* base, uint32_t rva, const char* hex) {
  auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (uint32_t i = 0; hex[i * 2]; ++i)
    if (base[rva + i] != (digit(hex[i * 2]) * 16 + digit(hex[i * 2 + 1]))) return false;
  return true;
}
inline void installBloodLimit() {
  static bool attempted = false; // D3D11 initialization is serialized.
  if (attempted) return;
  attempted = true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  const auto* profile = identify(base);
  if (!profile || !matches(base, profile->blood, "c783c8010000")
      || !matches(base, profile->blood + 10, "66c783cc0100000001")) {
    Logger::warn("[KenshiNative V794] unsupported blood constructor; native limit retained");
    return;
  }
  auto* count = base + profile->blood + 6;
  uint32_t oldValue;
  std::memcpy(&oldValue, count, sizeof(oldValue));
  if (oldValue == 16) return;
  if (oldValue != 8) {
    Logger::warn("[KenshiNative V794] blood limit modified by another component; retained");
    return;
  }
  DWORD protection, ignored;
  if (!VirtualProtect(count, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &protection)) return;
  const uint32_t value = 16;
  std::memcpy(count, &value, sizeof(value));
  FlushInstructionCache(GetCurrentProcess(), count, sizeof(value));
  VirtualProtect(count, sizeof(value), protection, &ignored);
  KENSHI_DIAGNOSTIC_INFO("[KenshiNative V794] blood limit 8->16 applied in memory; executable file unchanged");
}
}
