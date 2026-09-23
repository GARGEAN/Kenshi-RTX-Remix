#include "dxbc_chunk_rdef.h"

namespace dxvk {

  namespace {

    // Names are stored at chunk-relative offsets. DxbcReader::readString() scans
    // for a terminator without consulting the reader's size, so a truncated or
    // hostile chunk could walk past the end of the container. Reflection data is
    // untrusted shader content, so resolve names through clone() (which bounds
    // checks the offset) and stop at the chunk end.
    constexpr size_t kMaxReflectionNameLength = 4096;

    std::string readBoundedString(
      const DxbcReader& base,
            size_t      offset) {
      try {
        DxbcReader reader = base.clone(offset);
        std::string result;

        while (!reader.eof()) {
          const char c = static_cast<char>(reader.readu8());

          if (c == '\0')
            return result;

          if (result.size() >= kMaxReflectionNameLength)
            break;

          result.push_back(c);
        }
      } catch (const DxvkError&) {
        // Offset outside the chunk - fall through to the empty name.
      }

      // Unterminated within the chunk: treat as malformed rather than guessing.
      return std::string();
    }

    constexpr uint32_t kResourceBindingStride = 32;
    constexpr uint32_t kConstantBufferStride  = 24;
    constexpr uint32_t kVariableStrideSm4     = 24;
    constexpr uint32_t kVariableStrideSm5     = 40;

    // D3D_SHADER_VARIABLE_FLAGS
    constexpr uint32_t kShaderVariableUsed = 0x2;

  }


  DxbcRdef::DxbcRdef(DxbcReader reader) {
    // A shader may ship with reflection stripped, and the chunk is advisory for
    // our purposes. Parse best-effort: on any malformed field, keep whatever was
    // decoded so far and mark the chunk unusable rather than failing shader
    // creation outright.
    try {
      DxbcReader base = reader;

      const uint32_t cbufferCount    = reader.readu32();
      const uint32_t cbufferOffset   = reader.readu32();
      const uint32_t bindingCount    = reader.readu32();
      const uint32_t bindingOffset   = reader.readu32();
      reader.readu8();  // minor version
      const uint32_t majorVersion    = reader.readu8();

      // The remaining header fields (program type, flags, creator offset, and
      // the RD11 extension) are not needed here; every table is reached through
      // its own absolute offset below.

      // Shader model 5 widened the constant-buffer variable record with texture
      // and sampler binding info. Bound-resource records stay 32 bytes for every
      // model D3D11 can express (the 40-byte form with register space is 5.1,
      // which is D3D12-only).
      const uint32_t variableStride = majorVersion >= 5
        ? kVariableStrideSm5
        : kVariableStrideSm4;

      m_bindings.reserve(bindingCount);

      for (uint32_t i = 0; i < bindingCount; i++) {
        DxbcReader entry = base.clone(bindingOffset + i * kResourceBindingStride);

        const uint32_t nameOffset = entry.readu32();
        const uint32_t inputType  = entry.readu32();
        entry.readu32();  // return type
        entry.readu32();  // view dimension
        entry.readu32();  // sample count
        const uint32_t bindPoint  = entry.readu32();
        const uint32_t bindCount  = entry.readu32();
        entry.readu32();  // input flags

        DxbcResourceBinding binding;
        binding.name      = readBoundedString(base, nameOffset);
        binding.kind      = static_cast<DxbcResourceKind>(inputType);
        binding.bindPoint = bindPoint;
        binding.bindCount = bindCount;

        m_bindings.push_back(std::move(binding));
      }

      m_constantBuffers.reserve(cbufferCount);

      for (uint32_t i = 0; i < cbufferCount; i++) {
        DxbcReader entry = base.clone(cbufferOffset + i * kConstantBufferStride);

        const uint32_t nameOffset     = entry.readu32();
        const uint32_t variableCount  = entry.readu32();
        const uint32_t variableOffset = entry.readu32();
        const uint32_t bufferSize     = entry.readu32();

        DxbcConstantBufferInfo cbuffer;
        cbuffer.name = readBoundedString(base, nameOffset);
        cbuffer.size = bufferSize;
        cbuffer.variables.reserve(variableCount);

        for (uint32_t j = 0; j < variableCount; j++) {
          DxbcReader var = base.clone(variableOffset + j * variableStride);

          const uint32_t varNameOffset = var.readu32();
          const uint32_t startOffset   = var.readu32();
          const uint32_t varSize       = var.readu32();
          const uint32_t varFlags      = var.readu32();

          DxbcConstantVariable variable;
          variable.name   = readBoundedString(base, varNameOffset);
          variable.offset = startOffset;
          variable.size   = varSize;
          variable.used   = (varFlags & kShaderVariableUsed) != 0;

          cbuffer.variables.push_back(std::move(variable));
        }

        m_constantBuffers.push_back(std::move(cbuffer));
      }

      m_valid = true;
    } catch (const DxvkError&) {
      m_valid = false;
    }
  }


  DxbcRdef::~DxbcRdef() {

  }


  const DxbcResourceBinding* DxbcRdef::findBinding(
          DxbcResourceKind kind,
          uint32_t         bindPoint) const {
    for (const auto& binding : m_bindings) {
      if (binding.kind != kind)
        continue;

      // bindCount is 0 for unbounded arrays; treat those as covering the slot
      // from their base upwards so array-declared textures still resolve.
      const bool inRange = binding.bindCount == 0
        ? bindPoint >= binding.bindPoint
        : bindPoint >= binding.bindPoint && bindPoint < binding.bindPoint + binding.bindCount;

      if (inRange)
        return &binding;
    }

    return nullptr;
  }

}
