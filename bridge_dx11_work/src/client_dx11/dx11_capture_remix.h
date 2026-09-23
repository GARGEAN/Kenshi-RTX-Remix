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
#pragma once

// DX11_V226_CAPTURE_TO_REMIX
// Full translation layer: turns captured in-game DX11 geometry into Remix scene
// API commands (RemixApi_CreateMaterial / RemixApi_CreateMesh / RemixApi_DrawInstance
// / RemixApi camera) and streams them over the bridge IPC to the x64 server, which
// replays them into the .trex Remix runtime.
//
// This is a real capture engine, not a sampler:
//  * Vertices are decoded from the actual bound D3D11 input layout - per-element
//    semantic, DXGI format and byte offset across all bound vertex-buffer slots
//    (position, normal, tangent, texcoord, color).
//  * Vertex/index buffer contents are tracked through their full lifecycle:
//    CreateBuffer initial data, Map/Unmap writes, and UpdateSubresource.
//  * The per-draw object->world transform is recovered by reflecting the bound
//    vertex shader's constant buffers and reading the live constant-buffer bytes.
//  * The bound pixel-shader albedo texture identity drives a stable per-material
//    hash so Remix can key replacements.

struct ID3D11Buffer;
struct ID3D11Resource;
struct ID3D11InputLayout;
struct ID3D11VertexShader;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct D3D11_INPUT_ELEMENT_DESC;

#include <cstdint>
#include <cstddef>

namespace dx11_capture {

  // ---- Resource lifecycle tracking (called from the d3d11.dll capture hooks) ----

  void RecordBufferCreate(ID3D11Buffer* buffer, const void* pInitialData,
                          uint32_t byteWidth, uint32_t bindFlags);
  void RecordBufferRelease(ID3D11Buffer* buffer);

  // Map returns a CPU pointer the game writes into; Unmap commits it. We snapshot
  // the written bytes on Unmap so dynamic geometry is captured too.
  void RecordMap(ID3D11Resource* resource, uint32_t subresource, void* pMappedData);
  void RecordUnmap(ID3D11Resource* resource, uint32_t subresource);

  // CopyResource / UpdateSubresource style CPU->GPU upload of buffer contents.
  void RecordUpdateSubresource(ID3D11Resource* resource, uint32_t dstSubresource,
                               const void* pSrcData, uint32_t srcRowPitch);

  // DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH: record a content-derived identity
  // for a texture at creation so material hashes are identical across runs
  // and GPUs. The old material key hashed the resource POINTER, which ASLR
  // randomizes every launch - replacements could never key on it.
  void RecordTexture2DCreate(ID3D11Resource* texture, const void* pMip0Data,
                             uint32_t rowPitchBytes, uint32_t width, uint32_t height,
                             uint32_t format, uint32_t mipLevels, uint32_t arraySize,
                             uint32_t bindFlags);
  void RecordTexture2DRelease(ID3D11Resource* texture);

  // Input layouts: cache the element array so draws can decode every vertex stream.
  void RecordInputLayoutCreate(ID3D11InputLayout* layout,
                               const D3D11_INPUT_ELEMENT_DESC* descs, uint32_t numElements);
  void RecordInputLayoutRelease(ID3D11InputLayout* layout);

  // Vertex shaders: reflect the bytecode to locate the object->world (or combined)
  // transform matrix inside its constant buffers.
  void RecordVertexShaderCreate(ID3D11VertexShader* shader, const void* bytecode, size_t length);
  void RecordVertexShaderRelease(ID3D11VertexShader* shader);

  // ---- Draw capture ----

  void CaptureDrawIndexed(ID3D11DeviceContext* context, uint32_t indexCount,
                          uint32_t startIndexLocation, int32_t baseVertexLocation);
  void CaptureDraw(ID3D11DeviceContext* context, uint32_t vertexCount,
                   uint32_t startVertexLocation);

  // Frame boundary (hooked Present). DX11_V265_BRIDGE_PRESENT_CAMERA: sends
  // RemixApi_Startup (once, with the game window HWND from the swapchain),
  // RemixApi_SetupCamera (per frame, from the camera scanned out of the bound
  // VS constant buffers at draw time), and RemixApi_Present (per frame, with
  // the game HWND as override so Remix presents INTO the game window instead
  // of a secondary one). Without this pump the server never starts the Remix
  // runtime and nothing path-traced reaches the screen for x86 bridge games.
  void OnPresent(IDXGISwapChain* swapChain);

  // True once the bridge IPC handshake is up and streaming is enabled.
  bool IsStreamingEnabled();

} // namespace dx11_capture