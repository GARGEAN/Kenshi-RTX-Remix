/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
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

// Descriptor-slot layout for the D3D11 capture/replacement path.
//
// The RTX passes own the low end of DxvkContext's flat resource array: the common
// bindings sit at 0..19 and the atmosphere/cloud set at 200..216. Resources the
// capture layer rebinds on behalf of the game (a replaced terrain texture, for
// example) therefore cannot be bound at their raw D3D11 register - binding a
// texture the game sampled from t6 would overwrite a ray tracing binding. This
// header maps a (stage, binding type, D3D11 register) triple into a reserved band
// above every RTX slot.
//
// The band is bounded: DxvkContext's array is MaxNumResourceSlots entries, so a
// register that would map past the end yields kInvalidResourceSlot and callers
// skip the bind. That is the safe direction - a missing replacement texture is a
// visual regression, whereas an out-of-range slot is a memory-corrupting write
// into an unrelated binding.

#include <cstdint>
#include <utility>

#include "d3d11_shader_caps.h"
#include "../dxvk/dxvk_limits.h"

namespace dxvk {

  // Shader stages the capture layer can rebind resources for. D3D11 has more
  // stages, but only these two ever source material textures in this path.
  // Values index the per-stage window below, so do not reorder.
  namespace D3D11ShaderStages {
    enum D3D11ShaderStage : uint16_t {
      VertexShader = 0,
      PixelShader  = 1,
      Count        = 2,
    };
  }
  using D3D11ShaderStage = D3D11ShaderStages::D3D11ShaderStage;

  enum class D3D11BindingType : uint32_t {
    ConstantBuffer,
    ShaderResource
  };

  // Slot 0 is the natural "unset" value and is a real RTX binding
  // (BINDING_ACCELERATION_STRUCTURE), so it doubles as the invalid sentinel:
  // nothing in the capture path may legitimately bind there.
  constexpr uint32_t kInvalidResourceSlot = 0;

  // Start of the reserved band, chosen to clear every RTX binding index.
  constexpr uint32_t kReservedSlotBase = 1000;
  constexpr uint32_t kReservedSlotCount =
    uint32_t(DxvkLimits::MaxNumResourceSlots) - kReservedSlotBase;

  // Each stage gets an equal window of the band.
  constexpr uint32_t kSlotWindowPerStage =
    kReservedSlotCount / uint32_t(D3D11ShaderStages::Count);

  // Within a stage window the constant buffers come first, then shader resources.
  // D3D11 allows 14 CB registers and 128 SRV registers per stage; both windows
  // together must fit kSlotWindowPerStage, so the SRV range is what gets capped.
  constexpr uint32_t kConstantBufferSlotsPerStage =
    D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
  constexpr uint32_t kShaderResourceSlotsPerStage =
    kSlotWindowPerStage - kConstantBufferSlotsPerStage;

  static_assert(kConstantBufferSlotsPerStage < kSlotWindowPerStage,
                "Reserved slot window is too small to hold a stage's constant buffers");
  static_assert(kReservedSlotBase + kSlotWindowPerStage * uint32_t(D3D11ShaderStages::Count)
                  <= uint32_t(DxvkLimits::MaxNumResourceSlots),
                "Reserved slot band overruns DxvkContext's resource array");

  /**
   * \brief Maps a D3D11 register to a reserved DxvkContext resource slot
   *
   * \param [in] shaderStage  Stage the register belongs to
   * \param [in] bindingType  Whether the register is a CB or an SRV
   * \param [in] registerIndex D3D11 register number (b# or t#)
   * \returns The reserved slot, or kInvalidResourceSlot if the register falls
   *          outside the band this build can address.
   */
  constexpr uint32_t computeResourceSlotId(
        D3D11ShaderStage shaderStage,
        D3D11BindingType bindingType,
        uint32_t         registerIndex) {
    const uint32_t stageBase =
      kReservedSlotBase + kSlotWindowPerStage * uint32_t(shaderStage);

    switch (bindingType) {
      case D3D11BindingType::ConstantBuffer:
        return registerIndex < kConstantBufferSlotsPerStage
          ? stageBase + registerIndex
          : kInvalidResourceSlot;

      case D3D11BindingType::ShaderResource:
        return registerIndex < kShaderResourceSlotsPerStage
          ? stageBase + kConstantBufferSlotsPerStage + registerIndex
          : kInvalidResourceSlot;
    }

    return kInvalidResourceSlot;
  }

  // Maps a texture stage to the (shader stage, D3D11 register) pair used to
  // compute its slot. Material textures are sampled by the pixel shader, and the
  // capture layer records the register the game actually used, so the stage index
  // is the register.
  inline std::pair<D3D11ShaderStage, uint32_t> remapStateSamplerShader(uint32_t samplerIndex) {
    return std::make_pair(D3D11ShaderStages::PixelShader, samplerIndex);
  }

}
