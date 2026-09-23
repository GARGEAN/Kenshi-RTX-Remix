/*
 * Copyright (c) 2026 NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <guiddef.h>
#endif

namespace remix::emulator {

  // This GUID is written to ID3D11DeviceContext private data by an emulator
  // immediately around a guest draw. Native applications never set it, which
  // makes the path explicitly opt-in instead of relying on vertex heuristics.
#if defined(_WIN32)
  inline constexpr GUID kDrawMetadataGuid = {
    0x63d58e61, 0xc637, 0x4c47,
    { 0x93, 0xd0, 0x1d, 0xf5, 0x31, 0xb8, 0x44, 0x62 }
  };
#endif

  inline constexpr std::uint32_t kMagic = 0x4d455852u; // "RXEM"
  inline constexpr std::uint16_t kAbiMajor = 1;
  inline constexpr std::uint16_t kAbiMinor = 0;

  enum class Provider : std::uint32_t {
    Unknown = 0,
    Pcsx2 = 0x32584350u, // "PCX2"
    Dolphin = 0x504c4f44u, // "DOLP"
    Xenia = 0x494e4558u, // "XENI"
    Ppsspp = 0x50535050u, // "PPSP"
    Cemu = 0x554d4543u, // "CEMU"
    DuckStation = 0x4b435544u, // "DUCK"
  };

  enum class CoordinateSpace : std::uint32_t {
    Unknown = 0,
    // PS2 GS 12.4 fixed-point XY and 32-bit depth after the guest VU has
    // already applied its world/view/projection transform.
    Pcsx2GsPostTransform = 1,
    View = 2,
    World = 3,
  };

  enum DrawFlags : std::uint32_t {
    DrawFlagNone = 0,
    DrawFlagTextured = 1u << 0,
    DrawFlagFixedUv = 1u << 1,
    DrawFlagAlphaBlend = 1u << 2,
    DrawFlagUiCandidate = 1u << 3,
    DrawFlagVertexShaderExpanded = 1u << 4,
  };

#pragma pack(push, 1)
  struct DrawMetadataV1 {
    std::uint32_t magic = kMagic;
    std::uint16_t abiMajor = kAbiMajor;
    std::uint16_t abiMinor = kAbiMinor;
    std::uint32_t structSize = 0;
    std::uint32_t payloadHash = 0;
    Provider provider = Provider::Unknown;
    CoordinateSpace coordinateSpace = CoordinateSpace::Unknown;
    std::uint64_t frameId = 0;
    std::uint64_t drawId = 0;
    char gameSerial[32] = {};
    std::uint32_t gameCrc = 0;
    std::uint32_t flags = DrawFlagNone;
    std::uint32_t topology = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint64_t guestTextureHash = 0;
    std::uint32_t textureWidth = 0;
    std::uint32_t textureHeight = 0;
    float vertexScale[2] = {};
    float vertexOffset[2] = {};
    float textureScale[2] = {};
    float textureOffset[2] = {};
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float depthScale = 0x1p-32f;
    std::uint32_t reserved[10] = {};
  };
#pragma pack(pop)

  static_assert(sizeof(DrawMetadataV1) == 192);
  static_assert(offsetof(DrawMetadataV1, payloadHash) == 12);

  inline std::uint32_t fnv1a32(const void* bytes, std::size_t size,
                               std::uint32_t hash = 2166136261u) {
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t i = 0; i < size; ++i) {
      hash ^= data[i];
      hash *= 16777619u;
    }
    return hash;
  }

  inline std::uint32_t calculatePayloadHash(const DrawMetadataV1& metadata) {
    constexpr std::size_t hashOffset = offsetof(DrawMetadataV1, payloadHash);
    constexpr std::size_t hashSize = sizeof(metadata.payloadHash);
    std::uint32_t hash = fnv1a32(&metadata, hashOffset);
    const std::uint32_t zero = 0;
    hash = fnv1a32(&zero, sizeof(zero), hash);
    return fnv1a32(reinterpret_cast<const std::uint8_t*>(&metadata)
                    + hashOffset + hashSize,
                  sizeof(metadata) - hashOffset - hashSize, hash);
  }

  inline void seal(DrawMetadataV1& metadata) {
    metadata.magic = kMagic;
    metadata.abiMajor = kAbiMajor;
    metadata.abiMinor = kAbiMinor;
    metadata.structSize = sizeof(DrawMetadataV1);
    metadata.payloadHash = 0;
    metadata.payloadHash = calculatePayloadHash(metadata);
  }

  inline bool isSafeGameSerial(const char (&serial)[32]) {
    bool hasCharacter = false;
    bool terminated = false;
    for (char c : serial) {
      if (c == '\0') {
        terminated = true;
        break;
      }
      const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                     || (c >= '0' && c <= '9') || c == '-' || c == '_';
      if (!safe)
        return false;
      hasCharacter = true;
    }
    return hasCharacter && terminated;
  }

  inline bool isKnownProvider(Provider provider) {
    return provider == Provider::Pcsx2
        || provider == Provider::Dolphin
        || provider == Provider::Xenia
        || provider == Provider::Ppsspp
        || provider == Provider::Cemu
        || provider == Provider::DuckStation;
  }

  inline bool validate(const DrawMetadataV1& metadata) {
    return metadata.magic == kMagic
        && metadata.abiMajor == kAbiMajor
        && metadata.structSize == sizeof(DrawMetadataV1)
        && isKnownProvider(metadata.provider)
        && metadata.coordinateSpace != CoordinateSpace::Unknown
        && isSafeGameSerial(metadata.gameSerial)
        && metadata.gameCrc != 0
        && metadata.vertexCount > 0
        && metadata.payloadHash == calculatePayloadHash(metadata);
  }

  // Optional per-frame camera block published under its own GUID by emulators
  // that can recover the guest camera - GC/Wii XF registers in Dolphin, a
  // title-specific PS2 VU provider, PSP/GE matrices in PPSSPP, and so on.
  // Post-transform providers that cannot supply it (generic PCSX2) receive a
  // runtime-estimated camera instead. Publish once per guest frame before the
  // frame's draws; clear with SetPrivateData(guid, 0, nullptr) when the
  // transforms become unknown so a stale camera is never consumed.
#if defined(_WIN32)
  inline constexpr GUID kCameraMetadataGuid = {
    0x7a1c9b04, 0x52d3, 0x4bd8,
    { 0xae, 0x27, 0x66, 0x09, 0x4c, 0xd1, 0x8f, 0x35 }
  };
#endif

  inline constexpr std::uint32_t kCameraMagic = 0x43455852u; // "RXEC"

  enum CameraFlags : std::uint32_t {
    CameraFlagNone = 0,
    CameraFlagHasWorldToView = 1u << 0,
    CameraFlagHasViewToProjection = 1u << 1,
  };

#pragma pack(push, 1)
  struct CameraMetadataV1 {
    std::uint32_t magic = kCameraMagic;
    std::uint16_t abiMajor = kAbiMajor;
    std::uint16_t abiMinor = kAbiMinor;
    std::uint32_t structSize = 0;
    std::uint32_t payloadHash = 0;
    Provider provider = Provider::Unknown;
    std::uint32_t flags = CameraFlagNone;
    std::uint64_t frameId = 0;
    // Row-vector convention matching D3D constant-buffer usage: p' = p * M,
    // translation in row 3 (elements 12..14).
    float worldToView[16] = {};
    float viewToProjection[16] = {};
    std::uint32_t reserved[8] = {};
  };
#pragma pack(pop)

  static_assert(sizeof(CameraMetadataV1) == 192);
  static_assert(offsetof(CameraMetadataV1, payloadHash) == 12);

  inline std::uint32_t calculateCameraPayloadHash(const CameraMetadataV1& metadata) {
    constexpr std::size_t hashOffset = offsetof(CameraMetadataV1, payloadHash);
    constexpr std::size_t hashSize = sizeof(metadata.payloadHash);
    std::uint32_t hash = fnv1a32(&metadata, hashOffset);
    const std::uint32_t zero = 0;
    hash = fnv1a32(&zero, sizeof(zero), hash);
    return fnv1a32(reinterpret_cast<const std::uint8_t*>(&metadata)
                    + hashOffset + hashSize,
                  sizeof(metadata) - hashOffset - hashSize, hash);
  }

  inline void sealCamera(CameraMetadataV1& metadata) {
    metadata.magic = kCameraMagic;
    metadata.abiMajor = kAbiMajor;
    metadata.abiMinor = kAbiMinor;
    metadata.structSize = sizeof(CameraMetadataV1);
    metadata.payloadHash = 0;
    metadata.payloadHash = calculateCameraPayloadHash(metadata);
  }

  inline bool validateCamera(const CameraMetadataV1& metadata) {
    return metadata.magic == kCameraMagic
        && metadata.abiMajor == kAbiMajor
        && metadata.structSize == sizeof(CameraMetadataV1)
        && isKnownProvider(metadata.provider)
        && (metadata.flags
            & (CameraFlagHasWorldToView | CameraFlagHasViewToProjection)) != 0
        && metadata.payloadHash == calculateCameraPayloadHash(metadata);
  }

} // namespace remix::emulator
