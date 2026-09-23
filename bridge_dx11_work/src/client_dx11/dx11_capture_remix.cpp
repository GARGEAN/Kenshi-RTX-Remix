/*
 * Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

// DX11_V226_CAPTURE_TO_REMIX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dx11_capture_remix.h"
#include "dx11_bridge_client.h"

#include "util_bridgecommand.h"
#include "util_devicecommand.h"
#include "util_remixapi.h"

using namespace bridge_util;
using namespace remixapi::util;

namespace dx11_capture {

namespace {

  // ===================================================================== //
  // IPC send helpers (mirror of client/remix_api.cpp's wire format).      //
  // ===================================================================== //

  inline void sendUid(ClientMessage& msg, uint32_t uid) {
    msg.send_data(uid);
  }

  inline void sendBool(ClientMessage& msg, bool b) {
    uint32_t boolVal = b ? 1u : 0u;
    msg.send_data(boolVal);
  }

  template <typename SerializableT, typename BaseT>
  inline void serializeAndSend(ClientMessage& msg, const BaseT& base) {
    const SerializableT serializable(base);
    msg.send_data((uint32_t) ToRemixApiStructEnum<typename SerializableT::BaseT>);
    const auto serializableSize = serializable.size();
    auto* pSlzd = new uint8_t[serializableSize];
    serializable.serialize(pSlzd);
    msg.send_data((uint32_t) serializableSize, pSlzd);
    delete[] pSlzd;
  }

  void logf(const char* tag, const char* fmt, ...) {
    char buf[512] = {};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dx11_bridge_client::LogLine(tag, buf);
  }

  uint64_t fnv1a(const void* data, size_t size, uint64_t seed = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
  }

  // ===================================================================== //
  // DXGI vertex-format decoding.                                          //
  // ===================================================================== //

  inline float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h & 0x7C00u) >> 10;
    const uint32_t mant = (h & 0x03FFu);
    uint32_t f;
    if (exp == 0) {
      if (mant == 0) {
        f = sign; // +/-0
      } else {
        // subnormal half -> normalized float
        int e = -1;
        uint32_t m = mant;
        do { e++; m <<= 1; } while ((m & 0x0400u) == 0);
        m &= 0x03FFu;
        f = sign | ((uint32_t)(127 - 15 - e) << 23) | (m << 13);
      }
    } else if (exp == 0x1F) {
      f = sign | 0x7F800000u | (mant << 13); // Inf/NaN
    } else {
      f = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    memcpy(&out, &f, sizeof(out));
    return out;
  }

  uint32_t formatByteSize(DXGI_FORMAT fmt) {
    switch (fmt) {
      case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
      case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
      case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT:
      case DXGI_FORMAT_R32G32B32_SINT: return 12;
      case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
      case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
      case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_FLOAT:
      case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: return 8;
      case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
      case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_SNORM:
      case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT:
      case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM:
      case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
      case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_UINT:
      case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_FLOAT:
      case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: return 4;
      case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
      case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
      case DXGI_FORMAT_R16_SNORM: return 2;
      case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: return 1;
      default: return 0;
    }
  }

  // Decode up to 4 components of one attribute into floats (alpha defaults to 1).
  void decodeFormat(const uint8_t* s, DXGI_FORMAT fmt, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
    switch (fmt) {
      case DXGI_FORMAT_R32G32B32A32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(s);
        out[0]=f[0]; out[1]=f[1]; out[2]=f[2]; out[3]=f[3]; break; }
      case DXGI_FORMAT_R32G32B32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(s);
        out[0]=f[0]; out[1]=f[1]; out[2]=f[2]; break; }
      case DXGI_FORMAT_R32G32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(s);
        out[0]=f[0]; out[1]=f[1]; break; }
      case DXGI_FORMAT_R32_FLOAT: {
        out[0]=*reinterpret_cast<const float*>(s); break; }
      case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        const uint16_t* h = reinterpret_cast<const uint16_t*>(s);
        out[0]=halfToFloat(h[0]); out[1]=halfToFloat(h[1]); out[2]=halfToFloat(h[2]); out[3]=halfToFloat(h[3]); break; }
      case DXGI_FORMAT_R16G16_FLOAT: {
        const uint16_t* h = reinterpret_cast<const uint16_t*>(s);
        out[0]=halfToFloat(h[0]); out[1]=halfToFloat(h[1]); break; }
      case DXGI_FORMAT_R16_FLOAT: {
        out[0]=halfToFloat(*reinterpret_cast<const uint16_t*>(s)); break; }
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: {
        out[0]=s[0]/255.0f; out[1]=s[1]/255.0f; out[2]=s[2]/255.0f; out[3]=s[3]/255.0f; break; }
      case DXGI_FORMAT_B8G8R8A8_UNORM: {
        out[0]=s[2]/255.0f; out[1]=s[1]/255.0f; out[2]=s[0]/255.0f; out[3]=s[3]/255.0f; break; }
      case DXGI_FORMAT_B8G8R8X8_UNORM: {
        out[0]=s[2]/255.0f; out[1]=s[1]/255.0f; out[2]=s[0]/255.0f; out[3]=1.0f; break; }
      case DXGI_FORMAT_R8G8B8A8_SNORM: {
        for (int i=0;i<4;i++){ int8_t v=(int8_t)s[i]; out[i]= v <= -127 ? -1.0f : v/127.0f; } break; }
      case DXGI_FORMAT_R16G16_UNORM: {
        const uint16_t* u = reinterpret_cast<const uint16_t*>(s);
        out[0]=u[0]/65535.0f; out[1]=u[1]/65535.0f; break; }
      case DXGI_FORMAT_R16G16_SNORM: {
        const int16_t* u = reinterpret_cast<const int16_t*>(s);
        out[0]= u[0]<=-32767 ? -1.0f : u[0]/32767.0f; out[1]= u[1]<=-32767 ? -1.0f : u[1]/32767.0f; break; }
      case DXGI_FORMAT_R16G16B16A16_UNORM: {
        const uint16_t* u = reinterpret_cast<const uint16_t*>(s);
        for (int i=0;i<4;i++) out[i]=u[i]/65535.0f; break; }
      case DXGI_FORMAT_R16G16B16A16_SNORM: {
        const int16_t* u = reinterpret_cast<const int16_t*>(s);
        for (int i=0;i<4;i++) out[i]= u[i]<=-32767 ? -1.0f : u[i]/32767.0f; break; }
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        uint32_t v = *reinterpret_cast<const uint32_t*>(s);
        out[0]=((v) & 0x3FF)/1023.0f; out[1]=((v>>10)&0x3FF)/1023.0f; out[2]=((v>>20)&0x3FF)/1023.0f; out[3]=((v>>30)&0x3)/3.0f; break; }
      case DXGI_FORMAT_R8G8_UNORM: { out[0]=s[0]/255.0f; out[1]=s[1]/255.0f; break; }
      case DXGI_FORMAT_R8_UNORM: { out[0]=s[0]/255.0f; break; }
      default: break; // unsupported attribute format -> zeros
    }
  }

  inline uint32_t packColorRGBA(const float c[4]) {
    auto u8 = [](float f) -> uint32_t {
      if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
      return (uint32_t)(f * 255.0f + 0.5f);
    };
    return (u8(c[3])<<24) | (u8(c[2])<<16) | (u8(c[1])<<8) | u8(c[0]);
  }

  // ===================================================================== //
  // Capture state.                                                        //
  // ===================================================================== //

  std::recursive_mutex g_mutex;

  struct CachedBuffer {
    std::vector<uint8_t> data;
    uint32_t bindFlags = 0;
    uint64_t version = 1;     // bumped on every content change (Create/Unmap/UpdateSubresource)
  };
  std::unordered_map<ID3D11Buffer*, CachedBuffer> g_buffers;

  // Mesh dedup cache: geometry+material identity key -> already-created Remix mesh uid.
  // Lets repeated (static) draws skip the per-draw vertex decode + FNV hash + serialize +
  // IPC re-send entirely; only a fresh DrawInstance (carrying the current transform) is
  // emitted on a cache hit. This is THE bridge frame-rate fix: previously every draw
  // re-streamed its full mesh over the shared-memory bridge every frame.
  std::unordered_map<uint64_t, uint32_t> g_meshCache;

  inline uint64_t mixKey(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  }

  // Resource -> mapped CPU pointer (between Map and Unmap).
  std::unordered_map<ID3D11Resource*, void*> g_mapped;

  struct InputElement {
    std::string semantic;
    uint32_t semanticIndex;
    DXGI_FORMAT format;
    uint32_t inputSlot;
    uint32_t alignedByteOffset;
  };
  std::unordered_map<ID3D11InputLayout*, std::vector<InputElement>> g_layouts;

  struct VSReflect {
    bool hasWorld = false;     // an unambiguous object->world matrix was found
    uint32_t cbSlot = 0;       // b# register of the constant buffer holding it
    uint32_t byteOffset = 0;   // offset of the matrix inside that constant buffer
    bool columnMajor = true;   // HLSL default storage
  };
  std::unordered_map<ID3D11VertexShader*, VSReflect> g_vsReflect;

  // Material cache keyed by bound-texture identity hash.
  std::unordered_map<uint64_t, uint32_t> g_materials;

  // DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH: texture resource -> content-derived
  // identity hash, recorded at CreateTexture2D. Pointer address reuse after a
  // release self-heals because a new texture at the same address re-records
  // its entry at the only place a texture can come into existence.
  std::unordered_map<ID3D11Resource*, uint64_t> g_textureHashes;

  std::atomic<uint64_t> g_meshesStreamed { 0 };

  // ===================================================================== //
  // DX11_V265_BRIDGE_PRESENT_CAMERA: camera captured from VS constants.   //
  // ===================================================================== //

  // Both matrices row-major in the D3D row-vector convention (the layout the
  // server forwards verbatim into remixapi_CameraInfo view/projection).
  struct CameraWire {
    float view[16] = {};
    float proj[16] = {};
    bool valid = false;
    bool viewFound = false;
    uint64_t frame = ~0ull;
  };
  CameraWire g_camera;                       // guarded by g_mutex
  std::atomic<uint64_t> g_frameIndex { 0 };  // bumped in OnPresent
  uint64_t g_cameraScanFrame = ~0ull;        // guarded by g_mutex
  uint32_t g_cameraScanAttempts = 0;         // guarded by g_mutex

  bool isFinite16(const float* m) {
    for (int i = 0; i < 16; ++i)
      if (!std::isfinite(m[i])) return false;
    return true;
  }

  // D3D row-vector perspective: diag scales in m00/m11, w-generator in m[2][3],
  // m[3][3] == 0, shear terms zero.
  bool isPerspectiveRowMajor(const float* m) {
    if (!isFinite16(m)) return false;
    if (std::fabs(std::fabs(m[11]) - 1.0f) > 0.02f) return false;  // m[2][3]
    if (std::fabs(m[15]) > 0.02f) return false;                    // m[3][3]
    if (std::fabs(m[0]) < 0.1f || std::fabs(m[5]) < 0.1f) return false;
    if (std::fabs(m[1]) > 0.02f || std::fabs(m[3]) > 0.02f) return false;
    if (std::fabs(m[4]) > 0.02f || std::fabs(m[7]) > 0.02f) return false;
    if (std::fabs(m[12]) > 0.02f || std::fabs(m[13]) > 0.02f) return false;
    return true;
  }

  // Rigid-body view: orthonormal 3x3, affine last column, not identity.
  bool isViewLikeRowMajor(const float* m) {
    if (!isFinite16(m)) return false;
    if (std::fabs(m[3]) > 0.01f || std::fabs(m[7]) > 0.01f || std::fabs(m[11]) > 0.01f) return false;
    if (std::fabs(m[15] - 1.0f) > 0.01f) return false;
    for (int r = 0; r < 3; ++r) {
      const float len = m[r*4+0]*m[r*4+0] + m[r*4+1]*m[r*4+1] + m[r*4+2]*m[r*4+2];
      if (std::fabs(len - 1.0f) > 0.1f) return false;
    }
    if (std::fabs(m[0]*m[4] + m[1]*m[5] + m[2]*m[6]) > 0.08f) return false;
    if (std::fabs(m[0]*m[8] + m[1]*m[9] + m[2]*m[10]) > 0.08f) return false;
    if (std::fabs(m[4]*m[8] + m[5]*m[9] + m[6]*m[10]) > 0.08f) return false;
    bool identity = true;
    for (int r = 0; r < 4 && identity; ++r)
      for (int c = 0; c < 4; ++c)
        if (std::fabs(m[r*4+c] - (r == c ? 1.0f : 0.0f)) > 1e-5f) { identity = false; break; }
    return !identity;
  }

  void transpose16(const float* in, float* out) {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        out[c*4 + r] = in[r*4 + c];
  }

  // Scan the bound VS constant buffers (cached CPU copies - no GPU access)
  // for a projection + view pair. STRICTLY bounded: at most 4 attempts per
  // frame, slots 0-3, first 4KB per buffer - per-draw cost explosions freeze
  // in-game frame rates (the V262 lesson).
  void noteCameraFromConstants(ID3D11DeviceContext* ctx) {
    const uint64_t frame = g_frameIndex.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      if (g_camera.valid && g_camera.viewFound && g_camera.frame == frame) return;
      if (g_cameraScanFrame != frame) { g_cameraScanFrame = frame; g_cameraScanAttempts = 0; }
      if (g_cameraScanAttempts >= 4) return;
      ++g_cameraScanAttempts;
    }

    ID3D11Buffer* cbs[4] = {};
    ctx->VSGetConstantBuffers(0, 4, cbs);

    for (uint32_t slot = 0; slot < 4; ++slot) {
      ID3D11Buffer* cb = cbs[slot];
      if (!cb) continue;

      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      auto it = g_buffers.find(cb);
      if (it == g_buffers.end() || it->second.data.size() < 64) continue;
      const uint8_t* bytes = it->second.data.data();
      const size_t scanEnd = it->second.data.size() < 4096 ? it->second.data.size() : (size_t) 4096;

      // Pass 1: find the projection (as-stored or transposed).
      size_t projOff = SIZE_MAX;
      float proj[16];
      for (size_t off = 0; off + 64 <= scanEnd; off += 16) {
        const float* raw = reinterpret_cast<const float*>(bytes + off);
        if (isPerspectiveRowMajor(raw)) {
          memcpy(proj, raw, sizeof(proj));
          projOff = off;
          break;
        }
        float t[16];
        transpose16(raw, t);
        if (isPerspectiveRowMajor(t)) {
          memcpy(proj, t, sizeof(proj));
          projOff = off;
          break;
        }
      }
      if (projOff == SIZE_MAX) continue;

      // Pass 2: find the view in the same buffer (both conventions).
      bool viewFound = false;
      float view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
      for (size_t off = 0; off + 64 <= scanEnd; off += 16) {
        if (off == projOff) continue;
        const float* raw = reinterpret_cast<const float*>(bytes + off);
        if (isViewLikeRowMajor(raw)) {
          memcpy(view, raw, sizeof(view));
          viewFound = true;
          break;
        }
        float t[16];
        transpose16(raw, t);
        if (isViewLikeRowMajor(t)) {
          memcpy(view, t, sizeof(view));
          viewFound = true;
          break;
        }
      }

      // Keep the best camera this frame: any camera beats none, and a camera
      // with a real view beats a projection-only one.
      if (!g_camera.valid || viewFound || g_camera.frame != frame) {
        memcpy(g_camera.proj, proj, sizeof(proj));
        memcpy(g_camera.view, view, sizeof(view));
        g_camera.valid = true;
        g_camera.viewFound = viewFound;
        g_camera.frame = frame;

        static bool s_cameraLogged = false;
        if (!s_cameraLogged) {
          s_cameraLogged = true;
          logf("capture", "camera captured from VS constants: slot=%u projOff=%u view=%s",
               slot, (unsigned) projOff, viewFound ? "yes" : "NO (origin)");
        }
      }
      if (g_camera.viewFound)
        break;
    }

    for (uint32_t slot = 0; slot < 4; ++slot) {
      if (cbs[slot]) cbs[slot]->Release();
    }
  }

  // ===================================================================== //
  // Materials.                                                            //
  // ===================================================================== //

  uint32_t ensureMaterial(uint64_t textureHash) {
    auto it = g_materials.find(textureHash);
    if (it != g_materials.end()) return it->second;

    MaterialHandle matHandle;
    const uint32_t uid = matHandle.uid;

    remixapi_MaterialInfo matInfo = {};
    matInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    matInfo.pNext = nullptr;
    matInfo.hash = textureHash ? textureHash : 0x1ull;
    matInfo.albedoTexture = nullptr;
    matInfo.normalTexture = nullptr;
    matInfo.tangentTexture = nullptr;
    matInfo.emissiveTexture = nullptr;
    matInfo.emissiveIntensity = 0.0f;
    matInfo.emissiveColorConstant = { 0.0f, 0.0f, 0.0f };
    matInfo.spriteSheetRow = 0;
    matInfo.spriteSheetCol = 0;
    matInfo.spriteSheetFps = 0;
    matInfo.filterMode = 1;
    matInfo.wrapModeU = 0;
    matInfo.wrapModeV = 0;

    {
      ClientMessage c(Commands::RemixApi_CreateMaterial);
      serializeAndSend<serialize::MaterialInfo>(c, matInfo);
      sendBool(c, false);
      sendUid(c, uid);
    }
    g_materials[textureHash] = uid;
    return uid;
  }

  // Hash the identity of the texture bound to pixel-shader SRV slot 0 so that each
  // distinct source texture maps to a distinct, stable Remix material.
  //
  // DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH: the identity is the content hash
  // recorded at CreateTexture2D. The old code hashed the resource POINTER,
  // which ASLR randomizes every launch and the heap recycles within a run,
  // so material hashes were garbled and replacements could never key on them.
  uint64_t boundTextureHash(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    uint64_t h = 0;
    if (srv) {
      ID3D11Resource* res = nullptr;
      srv->GetResource(&res);
      if (res) {
        {
          std::lock_guard<std::recursive_mutex> lock(g_mutex);
          auto it = g_textureHashes.find(res);
          if (it != g_textureHashes.end()) h = it->second;
        }
        if (h == 0) {
          // Texture created before our hooks installed: fall back to a
          // desc-derived hash - weaker (same-shape textures collide) but
          // stable across runs, unlike the pointer.
          ID3D11Texture2D* tex2d = nullptr;
          if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**) &tex2d)) && tex2d) {
            D3D11_TEXTURE2D_DESC td = {};
            tex2d->GetDesc(&td);
            uint32_t meta[6] = { td.Width, td.Height, (uint32_t) td.Format,
                                 td.MipLevels, td.ArraySize, td.SampleDesc.Count };
            h = fnv1a(meta, sizeof(meta));
            tex2d->Release();
          }
        }
        res->Release();
      }
      srv->Release();
    }
    return h;
  }

  // ===================================================================== //
  // Vertex-shader reflection: locate the object->world transform.         //
  // ===================================================================== //

  void reflectVertexShader(ID3D11VertexShader* shader, const void* bytecode, size_t length) {
    if (!shader || !bytecode || length == 0) return;

    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(bytecode, length, __uuidof(ID3D11ShaderReflection), (void**) &refl)) || !refl) {
      return;
    }

    D3D11_SHADER_DESC sd = {};
    if (FAILED(refl->GetDesc(&sd))) { refl->Release(); return; }

    // Resolve each cbuffer's bind slot (b#) via the resource bindings.
    auto cbufferSlot = [&](const char* name) -> uint32_t {
      for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd = {};
        if (SUCCEEDED(refl->GetResourceBindingDesc(i, &bd)) &&
            bd.Type == D3D_SIT_CBUFFER && name && bd.Name && strcmp(bd.Name, name) == 0) {
          return bd.BindPoint;
        }
      }
      return 0;
    };

    VSReflect best;
    int bestScore = -1; // higher = better (prefer pure world over combined)

    for (UINT b = 0; b < sd.ConstantBuffers; ++b) {
      ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(b);
      D3D11_SHADER_BUFFER_DESC bdesc = {};
      if (!cb || FAILED(cb->GetDesc(&bdesc))) continue;
      const uint32_t slot = cbufferSlot(bdesc.Name);

      for (UINT v = 0; v < bdesc.Variables; ++v) {
        ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
        D3D11_SHADER_VARIABLE_DESC vd = {};
        if (!var || FAILED(var->GetDesc(&vd))) continue;
        ID3D11ShaderReflectionType* t = var->GetType();
        D3D11_SHADER_TYPE_DESC td = {};
        if (!t || FAILED(t->GetDesc(&td))) continue;
        const bool is4x4 = (td.Class == D3D_SVC_MATRIX_COLUMNS || td.Class == D3D_SVC_MATRIX_ROWS) &&
                           td.Rows == 4 && td.Columns == 4;
        if (!is4x4) continue;

        std::string name = vd.Name ? vd.Name : "";
        for (auto& ch : name) ch = (char) tolower((unsigned char) ch);
        const bool hasWorld = name.find("world") != std::string::npos || name.find("model") != std::string::npos;
        const bool hasView  = name.find("view") != std::string::npos;
        const bool hasProj  = name.find("proj") != std::string::npos ||
                              name.find("wvp") != std::string::npos ||
                              name.find("mvp") != std::string::npos;

        int score = -1;
        if (hasWorld && !hasView && !hasProj) score = 3; // pure object->world: ideal
        else if (hasWorld && (hasView || hasProj))  score = 1; // combined: usable only if nothing better
        else if (!hasWorld && !hasView && !hasProj) score = 0; // an unnamed/transform matrix

        if (score > bestScore) {
          bestScore = score;
          best.hasWorld = (score >= 2);    // only treat a pure-world match as authoritative
          best.cbSlot = slot;
          best.byteOffset = vd.StartOffset;
          best.columnMajor = (td.Class != D3D_SVC_MATRIX_ROWS);
        }
      }
    }

    refl->Release();

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_vsReflect[shader] = best;
  }

  // Read the 4x4 transform the bound VS uses and convert it to a Remix 3x4 affine
  // (column-vector convention: x' = M[i][0]*x + M[i][1]*y + M[i][2]*z + M[i][3]).
  // Returns false (caller uses identity) when no authoritative world matrix exists.
  bool resolveWorldTransform(ID3D11DeviceContext* ctx, remixapi_Transform& outXf) {
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;

    VSReflect r;
    {
      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      auto it = g_vsReflect.find(vs);
      if (it != g_vsReflect.end()) r = it->second;
    }
    if (!r.hasWorld) { vs->Release(); return false; }

    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(r.cbSlot, 1, &cb);
    bool ok = false;
    if (cb) {
      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      auto it = g_buffers.find(cb);
      if (it != g_buffers.end() && it->second.data.size() >= (size_t) r.byteOffset + 64) {
        const float* m = reinterpret_cast<const float*>(it->second.data.data() + r.byteOffset);
        // Reconstruct the logical 4x4 W such that the shader computes
        // mul(float4(pos,1), W) (D3D row-vector convention). HLSL stores matrices
        // column-major by default, so the 16 floats are W transposed unless the
        // variable was declared row_major.
        float W[4][4];
        if (r.columnMajor) {
          for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
              W[row][col] = m[col * 4 + row];
        } else {
          for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
              W[row][col] = m[row * 4 + col];
        }
        // Remix M (column-vector 3x4): M[i][j] = W[j][i] for basis, translation W[3][i].
        for (int i = 0; i < 3; ++i) {
          outXf.matrix[i][0] = W[0][i];
          outXf.matrix[i][1] = W[1][i];
          outXf.matrix[i][2] = W[2][i];
          outXf.matrix[i][3] = W[3][i];
        }
        ok = true;
      }
      cb->Release();
    }
    vs->Release();
    return ok;
  }

  // ===================================================================== //
  // Vertex decode using the bound input layout.                           //
  // ===================================================================== //

  struct StreamRef {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    uint32_t stride = 0;
  };

  const CachedBuffer* findBuffer(ID3D11Buffer* b) {
    if (!b) return nullptr;
    auto it = g_buffers.find(b);
    return (it != g_buffers.end() && !it->second.data.empty()) ? &it->second : nullptr;
  }

  // Build the per-element resolved byte offset (handling D3D11_APPEND_ALIGNED_ELEMENT).
  void resolveElementOffsets(const std::vector<InputElement>& elems, std::vector<uint32_t>& offsets) {
    uint32_t running[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    offsets.resize(elems.size());
    for (size_t i = 0; i < elems.size(); ++i) {
      const InputElement& e = elems[i];
      uint32_t off = e.alignedByteOffset;
      if (off == 0xFFFFFFFFu) off = running[e.inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
      offsets[i] = off;
      running[e.inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = off + formatByteSize(e.format);
    }
  }

  // ===================================================================== //
  // Stream one captured draw to Remix.                                    //
  // ===================================================================== //

  void streamDraw(ID3D11DeviceContext* ctx,
                  const std::vector<InputElement>& elems,
                  const StreamRef streams[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT],
                  const uint8_t* ibData, uint32_t ibSize, bool ib32,
                  uint32_t indexCount, uint32_t startIndex, int32_t baseVertex,
                  bool sequential, uint32_t seqStart, uint64_t geomKey) {
    if (indexCount < 3) return;
    if (indexCount > 24'000'000u) return;

    // Material identity is part of the mesh-cache key: the same geometry drawn with a
    // different bound texture must map to a distinct Remix mesh (the material is baked
    // onto the surface at CreateMesh time).
    const uint32_t materialUid = ensureMaterial(boundTextureHash(ctx));
    const uint64_t cacheKey = geomKey ? mixKey(geomKey, materialUid) : 0;

    uint32_t meshUid = 0;
    bool haveMesh = false;
    if (cacheKey) {
      auto cit = g_meshCache.find(cacheKey);
      if (cit != g_meshCache.end()) { meshUid = cit->second; haveMesh = true; }
    }

    if (!haveMesh) {
    // Locate the attribute elements we care about.
    std::vector<uint32_t> elemOffsets;
    resolveElementOffsets(elems, elemOffsets);

    int posE = -1, nrmE = -1, uvE = -1, colE = -1;
    for (size_t i = 0; i < elems.size(); ++i) {
      const std::string& s = elems[i].semantic;
      if (posE < 0 && (s == "POSITION" || s == "SV_Position" || s == "POSITION0")) posE = (int) i;
      else if (nrmE < 0 && s == "NORMAL") nrmE = (int) i;
      else if (uvE < 0 && (s == "TEXCOORD" || s == "TEXCOORD0")) uvE = (int) i;
      else if (colE < 0 && (s == "COLOR" || s == "COLOR0")) colE = (int) i;
    }
    if (posE < 0) return; // cannot capture geometry without positions

    const InputElement& pe = elems[posE];
    const StreamRef& posStream = streams[pe.inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    if (!posStream.data || posStream.stride < formatByteSize(pe.format)) return;
    const uint32_t vertexCount = posStream.size / posStream.stride;
    if (vertexCount == 0 || vertexCount > 8'000'000u) return;

    // Build the index list (object-space indices into the vertex streams).
    std::vector<uint32_t> indices;
    indices.reserve(indexCount);
    if (sequential) {
      for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t idx = seqStart + i;
        if (idx >= vertexCount) return;
        indices.push_back(idx);
      }
    } else {
      const uint32_t idxStride = ib32 ? 4u : 2u;
      for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t byteOff = (startIndex + i) * idxStride;
        if (byteOff + idxStride > ibSize) return;
        uint32_t v = ib32 ? *reinterpret_cast<const uint32_t*>(ibData + byteOff)
                          : *reinterpret_cast<const uint16_t*>(ibData + byteOff);
        const int64_t adj = (int64_t) v + (int64_t) baseVertex;
        if (adj < 0 || adj >= (int64_t) vertexCount) return;
        indices.push_back((uint32_t) adj);
      }
    }

    // Decode every vertex referenced by the position stream.
    std::vector<remixapi_HardcodedVertex> verts(vertexCount);
    const StreamRef& nStream = (nrmE >= 0) ? streams[elems[nrmE].inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] : StreamRef{};
    const StreamRef& tStream = (uvE  >= 0) ? streams[elems[uvE ].inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] : StreamRef{};
    const StreamRef& cStream = (colE >= 0) ? streams[elems[colE].inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] : StreamRef{};

    for (uint32_t i = 0; i < vertexCount; ++i) {
      remixapi_HardcodedVertex& hv = verts[i];
      memset(&hv, 0, sizeof(hv));

      float p[4];
      decodeFormat(posStream.data + (size_t) i * posStream.stride + elemOffsets[posE], pe.format, p);
      hv.position[0] = p[0]; hv.position[1] = p[1]; hv.position[2] = p[2];

      if (nrmE >= 0 && nStream.data && (size_t) i * nStream.stride + elemOffsets[nrmE] + formatByteSize(elems[nrmE].format) <= nStream.size) {
        float n[4];
        decodeFormat(nStream.data + (size_t) i * nStream.stride + elemOffsets[nrmE], elems[nrmE].format, n);
        hv.normal[0]=n[0]; hv.normal[1]=n[1]; hv.normal[2]=n[2];
      } else { hv.normal[0]=0.0f; hv.normal[1]=0.0f; hv.normal[2]=1.0f; }

      if (uvE >= 0 && tStream.data && (size_t) i * tStream.stride + elemOffsets[uvE] + formatByteSize(elems[uvE].format) <= tStream.size) {
        float t[4];
        decodeFormat(tStream.data + (size_t) i * tStream.stride + elemOffsets[uvE], elems[uvE].format, t);
        hv.texcoord[0]=t[0]; hv.texcoord[1]=t[1];
      }

      if (colE >= 0 && cStream.data && (size_t) i * cStream.stride + elemOffsets[colE] + formatByteSize(elems[colE].format) <= cStream.size) {
        float c[4];
        decodeFormat(cStream.data + (size_t) i * cStream.stride + elemOffsets[colE], elems[colE].format, c);
        hv.color = packColorRGBA(c);
      } else {
        hv.color = 0xFFFFFFFFu;
      }
    }

    uint64_t hash = fnv1a(verts.data(), verts.size() * sizeof(remixapi_HardcodedVertex));
    hash = fnv1a(indices.data(), indices.size() * sizeof(uint32_t), hash);

    // ---- CreateMesh ----
    MeshHandle meshHandle;
    {
      remixapi_MeshInfoSurfaceTriangles surf = {};
      surf.vertices_values = verts.data();
      surf.vertices_count = verts.size();
      surf.indices_values = indices.data();
      surf.indices_count = indices.size();
      surf.skinning_hasvalue = 0;
      memset(&surf.skinning_value, 0, sizeof(surf.skinning_value));
      surf.material = reinterpret_cast<remixapi_MaterialHandle>((uintptr_t) materialUid);

      remixapi_MeshInfo meshInfo = {};
      meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      meshInfo.pNext = nullptr;
      meshInfo.hash = hash;
      meshInfo.surfaces_values = &surf;
      meshInfo.surfaces_count = 1;

      ClientMessage c(Commands::RemixApi_CreateMesh);
      serializeAndSend<serialize::MeshInfo>(c, meshInfo);
      sendUid(c, meshHandle.uid);
    }

      meshUid = meshHandle.uid;
      if (cacheKey) {
        // Bound to avoid unbounded growth from genuinely dynamic geometry (a fresh
        // content-version produces a fresh key every frame). Clearing just forces those
        // to be re-streamed; static geometry repopulates immediately.
        if (g_meshCache.size() > 262144) g_meshCache.clear();
        g_meshCache[cacheKey] = meshUid;
      }

      const uint64_t n = ++g_meshesStreamed;
      if (n <= 16 || (n % 2000) == 0) {
        logf("capture", "DX11_V226_CAPTURE_TO_REMIX: streamed mesh #%llu (verts=%u, indices=%u) to Remix.",
             (unsigned long long) n, vertexCount, indexCount);
      }
    } // end if(!haveMesh): cache miss built+streamed a new mesh

    // ---- DrawInstance ---- (always emitted: carries the current per-draw transform)
    {
      remixapi_InstanceInfo instInfo = {};
      instInfo.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instInfo.pNext = nullptr;
      instInfo.categoryFlags = 0;
      instInfo.mesh = reinterpret_cast<remixapi_MeshHandle>((uintptr_t) meshUid);
      memset(&instInfo.transform, 0, sizeof(instInfo.transform));
      instInfo.transform.matrix[0][0] = 1.0f;
      instInfo.transform.matrix[1][1] = 1.0f;
      instInfo.transform.matrix[2][2] = 1.0f;
      resolveWorldTransform(ctx, instInfo.transform); // fills the real object->world when known
      instInfo.doubleSided = 1;

      ClientMessage c(Commands::RemixApi_DrawInstance);
      serializeAndSend<serialize::InstanceInfo>(c, instInfo);
      sendBool(c, false);
    }
  }

} // anonymous namespace

// ======================================================================= //
// Public entry points.                                                    //
// ======================================================================= //

void RecordBufferCreate(ID3D11Buffer* buffer, const void* pInitialData,
                        uint32_t byteWidth, uint32_t bindFlags) {
  if (!buffer || byteWidth == 0) return;
  if ((bindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_CONSTANT_BUFFER)) == 0) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  CachedBuffer& cb = g_buffers[buffer];
  cb.bindFlags = bindFlags;
  cb.data.resize(byteWidth);
  if (pInitialData) memcpy(cb.data.data(), pInitialData, byteWidth);
  else memset(cb.data.data(), 0, byteWidth);
  ++cb.version;
}

void RecordBufferRelease(ID3D11Buffer* buffer) {
  if (!buffer) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_buffers.erase(buffer);
  g_mapped.erase(reinterpret_cast<ID3D11Resource*>(buffer));
}

void RecordMap(ID3D11Resource* resource, uint32_t subresource, void* pMappedData) {
  if (!resource || subresource != 0 || !pMappedData) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (g_buffers.find(reinterpret_cast<ID3D11Buffer*>(resource)) == g_buffers.end()) return;
  g_mapped[resource] = pMappedData;
}

void RecordUnmap(ID3D11Resource* resource, uint32_t subresource) {
  if (!resource || subresource != 0) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto m = g_mapped.find(resource);
  if (m == g_mapped.end()) return;
  void* src = m->second;
  g_mapped.erase(m);
  auto it = g_buffers.find(reinterpret_cast<ID3D11Buffer*>(resource));
  if (it != g_buffers.end() && src && !it->second.data.empty()) {
    memcpy(it->second.data.data(), src, it->second.data.size());
    ++it->second.version;
  }
}

void RecordUpdateSubresource(ID3D11Resource* resource, uint32_t dstSubresource,
                             const void* pSrcData, uint32_t /*srcRowPitch*/) {
  if (!resource || dstSubresource != 0 || !pSrcData) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_buffers.find(reinterpret_cast<ID3D11Buffer*>(resource));
  if (it != g_buffers.end() && !it->second.data.empty()) {
    memcpy(it->second.data.data(), pSrcData, it->second.data.size());
    ++it->second.version;
  }
}

void RecordInputLayoutCreate(ID3D11InputLayout* layout,
                             const D3D11_INPUT_ELEMENT_DESC* descs, uint32_t numElements) {
  if (!layout || !descs || numElements == 0) return;
  std::vector<InputElement> elems;
  elems.reserve(numElements);
  for (uint32_t i = 0; i < numElements; ++i) {
    InputElement e;
    e.semantic = descs[i].SemanticName ? descs[i].SemanticName : "";
    e.semanticIndex = descs[i].SemanticIndex;
    e.format = descs[i].Format;
    e.inputSlot = descs[i].InputSlot;
    e.alignedByteOffset = descs[i].AlignedByteOffset;
    elems.push_back(std::move(e));
  }
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_layouts[layout] = std::move(elems);
}

void RecordInputLayoutRelease(ID3D11InputLayout* layout) {
  if (!layout) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_layouts.erase(layout);
}

// DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH
void RecordTexture2DCreate(ID3D11Resource* texture, const void* pMip0Data,
                           uint32_t rowPitchBytes, uint32_t width, uint32_t height,
                           uint32_t format, uint32_t mipLevels, uint32_t arraySize,
                           uint32_t bindFlags) {
  if (!texture) return;
  // Only textures the game can sample can become Remix materials.
  if ((bindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) return;

  uint64_t h = 0;
  if (pMip0Data && rowPitchBytes) {
    // Conservative row count: block-compressed formats store height/4 rows of
    // blocks while plain formats store `height` rows, so height/4 rows is a
    // safe lower bound for both - the read can never leave the caller's
    // allocation. Capped to bound creation-time cost on huge textures.
    const uint32_t safeRows = height >= 4 ? height / 4 : 1;
    uint64_t total = (uint64_t) rowPitchBytes * safeRows;
    const uint64_t kMaxHashBytes = 65536;
    if (total > kMaxHashBytes) total = kMaxHashBytes;
    h = fnv1a(pMip0Data, (size_t) total);
  }
  // Mix in structure so same-bytes/different-shape textures don't collide,
  // and so no-initial-data textures still get a stable (if weaker, desc-only)
  // identity instead of an ASLR-random pointer hash.
  const uint32_t meta[5] = { width, height, format, mipLevels, arraySize };
  h = fnv1a(meta, sizeof(meta), h ? h : 1469598103934665603ull);
  if (h == 0) h = 1;

  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_textureHashes[texture] = h;
}

void RecordTexture2DRelease(ID3D11Resource* texture) {
  if (!texture) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_textureHashes.erase(texture);
}

void RecordVertexShaderCreate(ID3D11VertexShader* shader, const void* bytecode, size_t length) {
  reflectVertexShader(shader, bytecode, length);
}

void RecordVertexShaderRelease(ID3D11VertexShader* shader) {
  if (!shader) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_vsReflect.erase(shader);
}

void CaptureDrawIndexed(ID3D11DeviceContext* context, uint32_t indexCount,
                        uint32_t startIndexLocation, int32_t baseVertexLocation) {
  if (!context || !IsStreamingEnabled() || indexCount < 3) return;

  noteCameraFromConstants(context);

  ID3D11InputLayout* layout = nullptr;
  context->IAGetInputLayout(&layout);

  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  context->IAGetPrimitiveTopology(&topo);

  ID3D11Buffer* vbs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  context->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vbs, strides, offsets);

  ID3D11Buffer* ib = nullptr;
  DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
  UINT ibOffset = 0;
  context->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);

  if (layout && ib && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
      (ibFormat == DXGI_FORMAT_R16_UINT || ibFormat == DXGI_FORMAT_R32_UINT)) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto lit = g_layouts.find(layout);
    const CachedBuffer* ibc = findBuffer(ib);
    if (lit != g_layouts.end() && ibc && ibOffset < ibc->data.size()) {
      StreamRef streams[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
      // Cheap geometry-identity key (no byte hashing): bound buffer pointers + their
      // content-versions + offsets/strides + draw params. Stable frame-to-frame for
      // static geometry, so streamDraw can skip the decode/serialize/IPC re-send.
      uint64_t geomKey = mixKey(0xC0FFEEull, reinterpret_cast<uintptr_t>(layout));
      for (uint32_t s = 0; s < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++s) {
        if (!vbs[s]) continue;
        const CachedBuffer* vbc = findBuffer(vbs[s]);
        if (vbc && offsets[s] < vbc->data.size()) {
          streams[s].data = vbc->data.data() + offsets[s];
          streams[s].size = (uint32_t)(vbc->data.size() - offsets[s]);
          streams[s].stride = strides[s];
          geomKey = mixKey(geomKey, ((uint64_t) s << 56) ^ reinterpret_cast<uintptr_t>(vbs[s]));
          geomKey = mixKey(geomKey, ((uint64_t) strides[s] << 32) | offsets[s]);
          geomKey = mixKey(geomKey, vbc->version);
        }
      }
      geomKey = mixKey(geomKey, reinterpret_cast<uintptr_t>(ib));
      geomKey = mixKey(geomKey, ((uint64_t) ibFormat << 32) | ibOffset);
      geomKey = mixKey(geomKey, ibc->version);
      geomKey = mixKey(geomKey, ((uint64_t) indexCount << 32) | startIndexLocation);
      geomKey = mixKey(geomKey, (uint64_t)(uint32_t) baseVertexLocation);
      streamDraw(context, lit->second, streams,
                 ibc->data.data() + ibOffset, (uint32_t)(ibc->data.size() - ibOffset),
                 ibFormat == DXGI_FORMAT_R32_UINT,
                 indexCount, startIndexLocation, baseVertexLocation, false, 0, geomKey);
    }
  }

  for (uint32_t s = 0; s < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++s) if (vbs[s]) vbs[s]->Release();
  if (ib) ib->Release();
  if (layout) layout->Release();
}

void CaptureDraw(ID3D11DeviceContext* context, uint32_t vertexCount,
                 uint32_t startVertexLocation) {
  if (!context || !IsStreamingEnabled() || vertexCount < 3) return;

  noteCameraFromConstants(context);

  ID3D11InputLayout* layout = nullptr;
  context->IAGetInputLayout(&layout);

  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  context->IAGetPrimitiveTopology(&topo);

  ID3D11Buffer* vbs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  context->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vbs, strides, offsets);

  if (layout && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto lit = g_layouts.find(layout);
    if (lit != g_layouts.end()) {
      StreamRef streams[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
      // Cheap geometry-identity key (sequential/non-indexed variant; distinct seed so it
      // never collides with the indexed path).
      uint64_t geomKey = mixKey(0xBEEFull, reinterpret_cast<uintptr_t>(layout));
      for (uint32_t s = 0; s < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++s) {
        if (!vbs[s]) continue;
        const CachedBuffer* vbc = findBuffer(vbs[s]);
        if (vbc && offsets[s] < vbc->data.size()) {
          streams[s].data = vbc->data.data() + offsets[s];
          streams[s].size = (uint32_t)(vbc->data.size() - offsets[s]);
          streams[s].stride = strides[s];
          geomKey = mixKey(geomKey, ((uint64_t) s << 56) ^ reinterpret_cast<uintptr_t>(vbs[s]));
          geomKey = mixKey(geomKey, ((uint64_t) strides[s] << 32) | offsets[s]);
          geomKey = mixKey(geomKey, vbc->version);
        }
      }
      geomKey = mixKey(geomKey, ((uint64_t) vertexCount << 32) | startVertexLocation);
      streamDraw(context, lit->second, streams, nullptr, 0, false,
                 vertexCount, 0, 0, true, startVertexLocation, geomKey);
    }
  }

  for (uint32_t s = 0; s < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++s) if (vbs[s]) vbs[s]->Release();
  if (layout) layout->Release();
}

// DX11_V229_PRESENT_WINDOW_FALLBACK: composition/CoreWindow swapchains do not
// expose a legacy DXGI_SWAP_CHAIN_DESC::OutputWindow. Resolve the game's real
// top-level HWND so Remix Startup is not permanently skipped for those games.
struct PresentWindowCandidateV229 {
  DWORD pid;
  HWND hwnd;
  unsigned long long area;
};

static BOOL CALLBACK FindPresentWindowV229(HWND hwnd, LPARAM param) {
  auto* best = reinterpret_cast<PresentWindowCandidateV229*>(param);
  if (!best || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != best->pid) return TRUE;
  RECT rc = {};
  if (!GetClientRect(hwnd, &rc)) return TRUE;
  const LONG width = rc.right - rc.left;
  const LONG height = rc.bottom - rc.top;
  if (width <= 0 || height <= 0) return TRUE;
  const unsigned long long area =
    static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height);
  if (area > best->area) {
    best->area = area;
    best->hwnd = hwnd;
  }
  return TRUE;
}

static HWND ResolvePresentWindowV229(IDXGISwapChain* swapChain) {
  if (swapChain) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow && IsWindow(desc.OutputWindow)) {
      return desc.OutputWindow;
    }
  }

  const DWORD selfPid = GetCurrentProcessId();
  HWND foreground = GetForegroundWindow();
  if (foreground) {
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    if (foregroundPid == selfPid) return foreground;
  }

  PresentWindowCandidateV229 best = { selfPid, nullptr, 0 };
  EnumWindows(&FindPresentWindowV229, reinterpret_cast<LPARAM>(&best));
  return best.hwnd;
}

// DX11_V265_BRIDGE_PRESENT_CAMERA: the per-frame pump that makes the server
// actually render. Startup attaches the Remix runtime to the GAME window
// (HWNDs are system-global, so the x64 server presents into the x86 game's
// window - this is what makes the Remix output primary instead of a stray
// secondary window), SetupCamera feeds the frame's captured camera, Present
// kicks the path-traced frame with the game HWND as override.
void OnPresent(IDXGISwapChain* swapChain) {
  if (!IsStreamingEnabled()) return;

  g_frameIndex.fetch_add(1, std::memory_order_relaxed);

  static uint64_t s_hwnd64 = 0;
  static bool s_startupSent = false;
  if (!s_startupSent && swapChain) {
    const HWND presentWindow = ResolvePresentWindowV229(swapChain);
    if (presentWindow) {
      s_hwnd64 = (uint64_t)(uintptr_t) presentWindow;
      ClientMessage c(Commands::RemixApi_Startup);
      c.send_data((uint32_t) sizeof(s_hwnd64), &s_hwnd64);
      s_startupSent = true;
      logf("capture", "RemixApi_Startup sent (resolved game hwnd=0x%llx)", (unsigned long long) s_hwnd64);
    }
  }
  if (!s_startupSent) return;

  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_camera.valid) {
      float blob[32];
      memcpy(blob, g_camera.view, sizeof(g_camera.view));
      memcpy(blob + 16, g_camera.proj, sizeof(g_camera.proj));
      ClientMessage c(Commands::RemixApi_SetupCamera);
      c.send_data((uint32_t) sizeof(blob), blob);
    }
  }

  {
    ClientMessage c(Commands::RemixApi_Present);
    c.send_data((uint32_t) sizeof(s_hwnd64), &s_hwnd64);
  }
}

bool IsStreamingEnabled() {
  return dx11_bridge_client::EnsureServer();
}

} // namespace dx11_capture