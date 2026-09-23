#include "../util/util_kenshi_telemetry.h"
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "d3d11_device.h"
#include "d3d11_shader.h"

namespace dxvk {

  // DX11_V298_SILENT_PREWARMER: shader-cache writes emit no log output unless
  // DXVK_REMIX_PREWARM_LOG=1 (matches the prewarm logging gate elsewhere).
  static bool shaderCacheLoggingEnabled() {
    static const bool enabled =
      env::getEnvVar("DXVK_REMIX_PREWARM_LOG") == "1";
    return enabled;
  }

  static void persistGameShaderBytecode(
      const DxvkShaderKey& shaderKey,
      const void* shaderBytecode,
      size_t bytecodeLength) {
    if (env::getEnvVar("DXVK_GAME_SHADER_CACHE") == "0"
     || shaderBytecode == nullptr
     || bytecodeLength == 0u
     || bytecodeLength > (8u << 20))
      return;

    static dxvk::mutex cacheMutex;
    std::lock_guard<dxvk::mutex> lock(cacheMutex);

    const std::filesystem::path cacheDirectory =
      std::filesystem::path(env::getExePath()).parent_path()
      / "rtx-remix" / "cache" / "d3d11-shaders";
    std::error_code error;
    std::filesystem::create_directories(cacheDirectory, error);
    if (error) {
      if (shaderCacheLoggingEnabled()) {
        Logger::warn(str::format(
          "[Remix-DX11][game-shader-cache] could not create '",
          cacheDirectory.string(), "': ", error.message()));
      }
      return;
    }

    const std::filesystem::path target =
      cacheDirectory / (shaderKey.toString() + ".dxbc");
    const uintmax_t existingSize = std::filesystem::file_size(target, error);
    if (!error && existingSize == bytecodeLength) {
      std::vector<char> existing(bytecodeLength);
      std::ifstream input(target, std::ios::in | std::ios::binary);
      input.read(existing.data(), static_cast<std::streamsize>(existing.size()));
      if (input
       && static_cast<size_t>(input.gcount()) == existing.size()
       && std::memcmp(existing.data(), shaderBytecode, bytecodeLength) == 0)
        return;
    }
    error.clear();

    std::filesystem::path temporary = target;
    temporary += str::format(".tmp.", GetCurrentProcessId());
    {
      std::ofstream output(
        temporary, std::ios::out | std::ios::binary | std::ios::trunc);
      output.write(
        reinterpret_cast<const char*>(shaderBytecode),
        static_cast<std::streamsize>(bytecodeLength));
      output.flush();
      if (!output) {
        output.close();
        std::filesystem::remove(temporary, error);
        if (shaderCacheLoggingEnabled()) {
          Logger::warn(str::format(
            "[Remix-DX11][game-shader-cache] failed to write '",
            target.string(), "'."));
        }
        return;
      }
    }

    std::filesystem::remove(target, error);
    error.clear();
    std::filesystem::rename(temporary, target, error);
    if (error) {
      std::error_code cleanupError;
      std::filesystem::remove(temporary, cleanupError);
      if (shaderCacheLoggingEnabled()) {
        Logger::warn(str::format(
          "[Remix-DX11][game-shader-cache] failed to publish '",
          target.string(), "': ", error.message()));
      }
    }
  }

  // DX11_V291_EXECUTABLE_SHADER_PROFILES: compatibility behavior is selected
  // from an executable-scoped database keyed by the exact VS SHA-1. Signature
  // analysis is only the discovery mechanism for a previously unseen shader;
  // once discovered, its decision is persisted and every later run/draw uses
  // the profile entry. A user can change any entry to "disabled" without
  // recompiling, and unrelated games never inherit each other's decisions.
  //
  // File format (append-only; the last duplicate entry wins):
  //   VS_<sha1>|world|POSITION|1|auto
  //   VS_<sha1>|view|VIEWPOSITION|0|manual
  //   VS_<sha1>|disabled|||manual
  struct D3D11PositionProfileRule {
    bool enabled = false;
    std::string semanticName;
    uint32_t semanticIndex = 0;
    D3D11CapturedPositionSpace positionSpace = D3D11CapturedPositionSpace::View;
  };

  class D3D11ExecutableShaderProfile {
  public:
    static D3D11ExecutableShaderProfile& instance() {
      static D3D11ExecutableShaderProfile profile;
      return profile;
    }

    bool resolve(
      const std::string& shaderKey,
      const std::string& discoveredSemantic,
      uint32_t discoveredIndex,
      D3D11CapturedPositionSpace discoveredSpace,
      D3D11PositionProfileRule& result,
      bool& loadedFromProfile) {
      std::lock_guard<dxvk::mutex> lock(m_mutex);
      ensureLoaded();

      const auto existing = m_positionRules.find(shaderKey);
      if (existing != m_positionRules.end()) {
        result = existing->second;
        loadedFromProfile = true;
        return result.enabled;
      }

      loadedFromProfile = false;
      if (discoveredSemantic.empty())
        return false;

      result.enabled = true;
      result.semanticName = discoveredSemantic;
      result.semanticIndex = discoveredIndex;
      result.positionSpace = discoveredSpace;
      m_positionRules.insert({ shaderKey, result });
      appendAutoRule(shaderKey, result);
      return true;
    }

  private:
    static std::string trim(std::string value) {
      const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
      value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
      value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
      return value;
    }

    static std::vector<std::string> splitProfileLine(const std::string& line) {
      std::vector<std::string> fields;
      size_t begin = 0;
      while (begin <= line.size()) {
        const size_t end = line.find('|', begin);
        fields.push_back(trim(line.substr(begin,
          end == std::string::npos ? std::string::npos : end - begin)));
        if (end == std::string::npos)
          break;
        begin = end + 1;
      }
      return fields;
    }

    void ensureLoaded() {
      if (m_loaded)
        return;
      m_loaded = true;

      std::string exeName = env::getExeNameNoSuffix();
      if (exeName.empty())
        exeName = "unknown";
      for (char& c : exeName) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '-' && c != '_' && c != '.')
          c = '_';
      }

      std::error_code ec;
      const std::filesystem::path directory =
        std::filesystem::path("rtx-remix") / "dx11-profiles";
      std::filesystem::create_directories(directory, ec);
      m_path = directory / (exeName + ".profile");

      std::ifstream input(m_path, std::ios::in);
      std::string line;
      uint32_t loadedRules = 0;
      std::unordered_map<std::string, bool> migrateAutoViewToWorld;
      while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#')
          continue;
        const std::vector<std::string> fields = splitProfileLine(line);
        if (fields.size() < 2 || fields[0].rfind("VS_", 0) != 0)
          continue;

        D3D11PositionProfileRule rule;
        std::string mode = fields[1];
        std::transform(mode.begin(), mode.end(), mode.begin(),
          [](unsigned char c) { return char(std::tolower(c)); });
        if (mode == "disabled") {
          rule.enabled = false;
          migrateAutoViewToWorld[fields[0]] = false;
        } else if ((mode == "view" || mode == "world")
                && fields.size() >= 4 && !fields[2].empty()) {
          try {
            const unsigned long parsedIndex = std::stoul(fields[3]);
            if (parsedIndex > UINT32_MAX)
              continue;
            rule.enabled = true;
            rule.semanticName = fields[2];
            rule.semanticIndex = static_cast<uint32_t>(parsedIndex);
            rule.positionSpace = mode == "world"
              ? D3D11CapturedPositionSpace::World
              : D3D11CapturedPositionSpace::View;

            // Version-1 auto discovery classified plain POSITION1 as view
            // space. In Skyrim/Bethesda-style deferred shaders it is a world
            // position that is consumed by a later view-projection multiply.
            // Upgrade only generated rules; a manual view rule always wins.
            std::string semanticUpper = rule.semanticName;
            std::transform(semanticUpper.begin(), semanticUpper.end(), semanticUpper.begin(),
              [](unsigned char c) { return char(std::toupper(c)); });
            const bool generatedRule = fields.size() >= 5
              && fields[4].rfind("auto", 0) == 0;
            const bool migrate = mode == "view"
              && generatedRule
              && semanticUpper == "POSITION"
              && rule.semanticIndex > 0;
            if (migrate)
              rule.positionSpace = D3D11CapturedPositionSpace::World;
            migrateAutoViewToWorld[fields[0]] = migrate;
          } catch (...) {
            continue;
          }
        } else {
          continue;
        }

        m_positionRules[fields[0]] = std::move(rule);
        ++loadedRules;
      }
      input.close();

      uint32_t migratedRules = 0;
      for (const auto& migration : migrateAutoViewToWorld) {
        if (!migration.second)
          continue;
        const auto rule = m_positionRules.find(migration.first);
        if (rule == m_positionRules.end())
          continue;
        appendAutoRule(migration.first, rule->second, "auto-v2-migrated");
        ++migratedRules;
      }

      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][profile] executable='", env::getExeName(),
        "' file='", m_path.string(), "' positionRules=", loadedRules,
        " migratedWorldRules=", migratedRules));
    }

    void appendAutoRule(
      const std::string& shaderKey,
      const D3D11PositionProfileRule& rule,
      const char* source = "auto") {
      if (m_path.empty())
        return;

      const bool needsHeader = !std::filesystem::exists(m_path);
      std::ofstream output(m_path, std::ios::out | std::ios::app);
      if (!output)
        return;
      if (needsHeader) {
        output << "# DXVK Remix DX11 executable shader profile v2\n";
        output << "# executable=" << env::getExeName() << "\n";
        output << "# shader|position-space|semantic|index|source\n";
      }
      output << shaderKey << "|"
             << (rule.positionSpace == D3D11CapturedPositionSpace::World ? "world" : "view")
             << "|" << rule.semanticName << "|"
             << rule.semanticIndex << "|" << source << "\n";
      output.flush();
    }

    dxvk::mutex m_mutex;
    bool m_loaded = false;
    std::filesystem::path m_path;
    std::unordered_map<std::string, D3D11PositionProfileRule> m_positionRules;
  };

  static D3D11PositionTransformBinding findPositionTransformBinding(const DxbcModule& module) {
    D3D11PositionTransformBinding result;

    const Rc<DxbcIsgn> outputSignature = module.osgn();
    const Rc<DxbcIsgn> inputSignature = module.isgn();
    if (outputSignature == nullptr || inputSignature == nullptr)
      return result;

    uint32_t positionRegister = UINT32_MAX;
    for (const DxbcSgnEntry& entry : *outputSignature) {
      std::string semantic = entry.semanticName;
      std::transform(semantic.begin(), semantic.end(), semantic.begin(),
        [](unsigned char c) { return char(std::toupper(c)); });
      if (entry.systemValue == DxbcSystemValue::Position
       || semantic == "SV_POSITION"
       || semantic == "POSITION") {
        positionRegister = entry.registerId;
        break;
      }
    }
    if (positionRegister == UINT32_MAX)
      return result;

    uint32_t positionInputRegister = UINT32_MAX;
    bool positionInputHasW = false;
    for (const DxbcSgnEntry& entry : *inputSignature) {
      std::string semantic = entry.semanticName;
      std::transform(semantic.begin(), semantic.end(), semantic.begin(),
        [](unsigned char c) { return char(std::toupper(c)); });
      if ((semantic == "POSITION" || semantic == "SV_POSITION")
       && entry.semanticIndex == 0) {
        positionInputRegister = entry.registerId;
        positionInputHasW = entry.componentMask[3];
        break;
      }
    }
    if (positionInputRegister == UINT32_MAX)
      return result;

    constexpr int32_t kInvalidRegister = -1;
    constexpr int32_t kSyntheticAffineRow = -2;

    struct TransformComponents {
      std::array<int32_t, 4> cbSlots = {
        kInvalidRegister, kInvalidRegister, kInvalidRegister, kInvalidRegister };
      std::array<int32_t, 4> cbRegisters = {
        kInvalidRegister, kInvalidRegister, kInvalidRegister, kInvalidRegister };
      std::array<uint32_t, 4> prefixCounts = { 0, 0, 0, 0 };
      std::array<D3D11PositionTransformMatrixBinding, 4> prefixes;
    } position;
    std::unordered_map<uint32_t, TransformComponents> temporaryTransforms;

    enum class PositionOrigin : uint8_t {
      Unknown,
      PositionX,
      PositionY,
      PositionZ,
      One,
    };
    using PositionOrigins = std::array<PositionOrigin, 4>;
    std::unordered_map<uint32_t, PositionOrigins> temporaryOrigins;

    auto staticRegisterIndex = [](const DxbcRegister& reg, uint32_t dimension, int32_t& index) {
      if (reg.idxDim <= dimension || reg.idx[dimension].relReg != nullptr)
        return false;
      index = reg.idx[dimension].offset;
      return index >= 0;
    };

    auto matrixBindingEqual = [](const D3D11PositionTransformMatrixBinding& a,
                                 const D3D11PositionTransformMatrixBinding& b) {
      return a.constantBufferSlot == b.constantBufferSlot
          && a.constantRegisters == b.constantRegisters;
    };

    auto collapseTransform = [&](const TransformComponents& transform,
                                 D3D11PositionTransformBinding& chain) {
      const bool syntheticW = transform.cbRegisters[3] == kSyntheticAffineRow;
      const uint32_t realRowCount = syntheticW ? 3u : 4u;
      const int32_t slot = transform.cbSlots[0];
      if (slot < 0)
        return false;

      for (uint32_t component = 0; component < realRowCount; ++component) {
        if (transform.cbSlots[component] != slot || transform.cbRegisters[component] < 0)
          return false;
      }
      if (syntheticW) {
        if (transform.cbSlots[3] != kSyntheticAffineRow)
          return false;
      } else if (transform.cbSlots[3] != slot || transform.cbRegisters[3] < 0) {
        return false;
      }

      std::array<int32_t, 4> sortedRegisters = transform.cbRegisters;
      std::sort(sortedRegisters.begin(), sortedRegisters.begin() + realRowCount);
      for (uint32_t i = 1; i < realRowCount; ++i) {
        if (sortedRegisters[i] != sortedRegisters[0] + int32_t(i))
          return false;
      }

      const uint32_t prefixCount = transform.prefixCounts[0];
      if (prefixCount > 1)
        return false;
      for (uint32_t component = 1; component < 4; ++component) {
        if (transform.prefixCounts[component] != prefixCount)
          return false;
        if (prefixCount != 0
         && !matrixBindingEqual(transform.prefixes[component], transform.prefixes[0]))
          return false;
      }

      chain = {};
      chain.valid = true;
      chain.matrixCount = prefixCount + 1;
      if (prefixCount != 0)
        chain.matrices[0] = transform.prefixes[0];

      D3D11PositionTransformMatrixBinding& current = chain.matrices[prefixCount];
      current.constantBufferSlot = uint32_t(slot);
      for (uint32_t component = 0; component < 4; ++component) {
        current.constantRegisters[component] = transform.cbRegisters[component] == kSyntheticAffineRow
          ? UINT32_MAX
          : uint32_t(transform.cbRegisters[component]);
      }
      return true;
    };

    auto invalidateWrittenComponents = [=](TransformComponents& transform, const DxbcRegMask& mask) {
      for (uint32_t component = 0; component < 4; ++component) {
        if (mask[component]) {
          transform.cbSlots[component] = kInvalidRegister;
          transform.cbRegisters[component] = kInvalidRegister;
          transform.prefixCounts[component] = 0;
        }
      }
    };

    auto invalidateOrigins = [](PositionOrigins& origins, const DxbcRegMask& mask) {
      for (uint32_t component = 0; component < 4; ++component) {
        if (mask[component])
          origins[component] = PositionOrigin::Unknown;
      }
    };

    auto sourceOrigin = [&](const DxbcRegister& source, uint32_t destinationComponent) {
      if (!source.modifiers.isClear())
        return PositionOrigin::Unknown;
      const uint32_t sourceComponent = source.swizzle[destinationComponent];

      int32_t sourceRegister = -1;
      if (source.type == DxbcOperandType::Input
       && staticRegisterIndex(source, 0, sourceRegister)
       && uint32_t(sourceRegister) == positionInputRegister) {
        switch (sourceComponent) {
          case 0: return PositionOrigin::PositionX;
          case 1: return PositionOrigin::PositionY;
          case 2: return PositionOrigin::PositionZ;
          default: return PositionOrigin::Unknown;
        }
      }

      if (source.type == DxbcOperandType::Temp
       && staticRegisterIndex(source, 0, sourceRegister)) {
        const auto entry = temporaryOrigins.find(uint32_t(sourceRegister));
        if (entry != temporaryOrigins.end())
          return entry->second[sourceComponent];
      }

      if (source.type == DxbcOperandType::Imm32) {
        const uint32_t bits = source.componentCount == DxbcComponentCount::Component1
          ? source.imm.u32_1
          : source.imm.u32_4[sourceComponent];
        if (bits == 0x3f800000u)
          return PositionOrigin::One;
      }

      return PositionOrigin::Unknown;
    };

    auto isCanonicalPositionVector = [&](const DxbcRegister& vector) {
      if (!vector.modifiers.isClear())
        return false;

      int32_t vectorRegister = -1;
      if (vector.type == DxbcOperandType::Input
       && positionInputHasW
       && staticRegisterIndex(vector, 0, vectorRegister)
       && uint32_t(vectorRegister) == positionInputRegister) {
        return vector.swizzle == DxbcRegSwizzle(0, 1, 2, 3);
      }

      if (vector.type != DxbcOperandType::Temp
       || !staticRegisterIndex(vector, 0, vectorRegister))
        return false;
      const auto origins = temporaryOrigins.find(uint32_t(vectorRegister));
      if (origins == temporaryOrigins.end())
        return false;

      const std::array<PositionOrigin, 4> expected = {
        PositionOrigin::PositionX,
        PositionOrigin::PositionY,
        PositionOrigin::PositionZ,
        PositionOrigin::One,
      };
      for (uint32_t component = 0; component < 4; ++component) {
        if (origins->second[vector.swizzle[component]] != expected[component])
          return false;
      }
      return true;
    };

    auto recordDp4 = [&](TransformComponents& transform, const DxbcRegister& dst,
                         const DxbcShaderInstruction& ins) {
      if (dst.mask.popCount() != 1)
        return false;

      const DxbcRegister* cb = nullptr;
      const DxbcRegister* vector = nullptr;
      if (ins.src[0].type == DxbcOperandType::ConstantBuffer) {
        cb = &ins.src[0];
        vector = &ins.src[1];
      } else if (ins.src[1].type == DxbcOperandType::ConstantBuffer) {
        cb = &ins.src[1];
        vector = &ins.src[0];
      }
      if (cb == nullptr || vector == nullptr || !cb->modifiers.isClear()
       || cb->swizzle != DxbcRegSwizzle(0, 1, 2, 3))
        return false;

      D3D11PositionTransformBinding prefix;
      if (isCanonicalPositionVector(*vector)) {
        prefix.valid = true;
        prefix.matrixCount = 0;
      } else {
        int32_t vectorRegister = -1;
        if (vector->type != DxbcOperandType::Temp
         || !vector->modifiers.isClear()
         || vector->swizzle != DxbcRegSwizzle(0, 1, 2, 3)
         || !staticRegisterIndex(*vector, 0, vectorRegister))
          return false;
        const auto source = temporaryTransforms.find(uint32_t(vectorRegister));
        if (source == temporaryTransforms.end()
         || !collapseTransform(source->second, prefix)
         || prefix.matrixCount != 1)
          return false;
      }

      int32_t slot = -1;
      int32_t cbRegister = -1;
      if (!staticRegisterIndex(*cb, 0, slot)
       || !staticRegisterIndex(*cb, 1, cbRegister))
        return false;

      const uint32_t component = dst.mask.firstSet();
      transform.cbSlots[component] = slot;
      transform.cbRegisters[component] = cbRegister;
      transform.prefixCounts[component] = prefix.matrixCount;
      if (prefix.matrixCount != 0)
        transform.prefixes[component] = prefix.matrices[0];
      return true;
    };

    DxbcCodeSlice code = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!code.atEnd()) {
      decoder.decodeInstruction(code);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      if (ins.dstCount == 0)
        continue;

      const DxbcRegister& dst = ins.dst[0];
      int32_t dstRegister = -1;
      const bool isPositionOutput = dst.type == DxbcOperandType::Output
        && staticRegisterIndex(dst, 0, dstRegister)
        && uint32_t(dstRegister) == positionRegister;
      const bool isTemporary = dst.type == DxbcOperandType::Temp
        && staticRegisterIndex(dst, 0, dstRegister);

      if (ins.op == DxbcOpcode::Dp4 && ins.dstCount == 1 && ins.srcCount == 2) {
        if (isPositionOutput) {
          if (!recordDp4(position, dst, ins))
            invalidateWrittenComponents(position, dst.mask);
        } else if (isTemporary) {
          TransformComponents& temporary = temporaryTransforms[uint32_t(dstRegister)];
          if (!recordDp4(temporary, dst, ins))
            invalidateWrittenComponents(temporary, dst.mask);
          invalidateOrigins(temporaryOrigins[uint32_t(dstRegister)], dst.mask);
        }
        continue;
      }

      // Most optimized SM5 vertex shaders calculate clip position into a
      // temporary and end with `mov oN, rM`. Propagate the four proven dp4
      // components through that exact move, including scalar write masks and
      // source swizzles. No arithmetic or dynamic indexing is guessed.
      if (ins.op == DxbcOpcode::Mov && ins.dstCount == 1 && ins.srcCount == 1) {
        TransformComponents* destinationTransform = isPositionOutput
          ? &position
          : (isTemporary ? &temporaryTransforms[uint32_t(dstRegister)] : nullptr);

        bool copiedTransform = false;
        int32_t sourceRegister = -1;
        if (destinationTransform != nullptr
         && ins.src[0].type == DxbcOperandType::Temp
         && ins.src[0].modifiers.isClear()
         && staticRegisterIndex(ins.src[0], 0, sourceRegister)) {
          const auto source = temporaryTransforms.find(uint32_t(sourceRegister));
          if (source != temporaryTransforms.end()) {
            for (uint32_t component = 0; component < 4; ++component) {
              if (!dst.mask[component])
                continue;
              const uint32_t sourceComponent = ins.src[0].swizzle[component];
              destinationTransform->cbSlots[component] = source->second.cbSlots[sourceComponent];
              destinationTransform->cbRegisters[component] = source->second.cbRegisters[sourceComponent];
              destinationTransform->prefixCounts[component] = source->second.prefixCounts[sourceComponent];
              destinationTransform->prefixes[component] = source->second.prefixes[sourceComponent];
            }
            copiedTransform = true;
          }
        }
        if (destinationTransform != nullptr && !copiedTransform)
          invalidateWrittenComponents(*destinationTransform, dst.mask);

        if (isTemporary) {
          PositionOrigins& origins = temporaryOrigins[uint32_t(dstRegister)];
          for (uint32_t component = 0; component < 4; ++component) {
            if (!dst.mask[component])
              continue;
            origins[component] = sourceOrigin(ins.src[0], component);
            if (component == 3 && origins[component] == PositionOrigin::One) {
              TransformComponents& temporary = temporaryTransforms[uint32_t(dstRegister)];
              temporary.cbSlots[3] = kSyntheticAffineRow;
              temporary.cbRegisters[3] = kSyntheticAffineRow;
              temporary.prefixCounts[3] = 0;
            }
          }
        }
        continue;
      }

      if (isPositionOutput)
        invalidateWrittenComponents(position, dst.mask);
      if (isTemporary) {
        invalidateWrittenComponents(temporaryTransforms[uint32_t(dstRegister)], dst.mask);
        invalidateOrigins(temporaryOrigins[uint32_t(dstRegister)], dst.mask);
      }
    }

    collapseTransform(position, result);
    return result;
  }

  static D3D11ConstantBufferDependencyProfile findConstantBufferDependencies(
      const DxbcModule& module) {
    D3D11ConstantBufferDependencyProfile result;
    result.complete = true;

    DxbcCodeSlice code = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!code.atEnd()) {
      decoder.decodeInstruction(code);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      for (uint32_t sourceIndex = 0; sourceIndex < ins.srcCount; ++sourceIndex) {
        const DxbcRegister& source = ins.src[sourceIndex];
        if (source.type != DxbcOperandType::ConstantBuffer)
          continue;

        if (source.idxDim < 1 || source.idx[0].relReg != nullptr
         || source.idx[0].offset < 0) {
          // A dynamically selected cbuffer slot cannot be represented by a
          // bounded slot profile. Retain the conservative all-bound fallback.
          result.complete = false;
          result.dependencies.clear();
          return result;
        }

        D3D11ConstantBufferDependency dependency;
        dependency.slot = uint32_t(source.idx[0].offset);
        dependency.wholeBuffer = source.idxDim < 2
          || source.idx[1].relReg != nullptr
          || source.idx[1].offset < 0;
        dependency.constantRegister = dependency.wholeBuffer
          ? 0u : uint32_t(source.idx[1].offset);

        auto sameDependency = [&](const D3D11ConstantBufferDependency& existing) {
          if (existing.slot != dependency.slot)
            return false;
          return existing.wholeBuffer || dependency.wholeBuffer
            || existing.constantRegister == dependency.constantRegister;
        };
        auto existing = std::find_if(
          result.dependencies.begin(), result.dependencies.end(), sameDependency);
        if (existing != result.dependencies.end()) {
          if (dependency.wholeBuffer) {
            result.dependencies.erase(
              std::remove_if(result.dependencies.begin(), result.dependencies.end(),
                [&](const D3D11ConstantBufferDependency& candidate) {
                  return candidate.slot == dependency.slot;
                }),
              result.dependencies.end());
            result.dependencies.push_back(dependency);
          }
          continue;
        }
        result.dependencies.push_back(dependency);
      }
    }

    std::sort(result.dependencies.begin(), result.dependencies.end(),
      [](const D3D11ConstantBufferDependency& a,
         const D3D11ConstantBufferDependency& b) {
        if (a.slot != b.slot)
          return a.slot < b.slot;
        if (a.wholeBuffer != b.wholeBuffer)
          return a.wholeBuffer > b.wholeBuffer;
        return a.constantRegister < b.constantRegister;
      });
    return result;
  }

  enum class D3D11CaptureExpressionKind : uint8_t {
    Unknown,
    Identity,
    MatrixRow,
  };

  // Proves an exact dataflow relationship between a named non-system VS output
  // and SV_Position. Both outputs may be direct views of the same temporary, or
  // may be four-row constant-buffer transforms of it. Optimized SM5 shaders may
  // also calculate the dp4 rows in a temporary and MOV them to an output. This
  // deliberately does not infer position space from semantic names.
  //
  // Result layout is specific to position capture:
  //   one matrix:  matrices[0] maps captured output directly to clip
  //   two matrices: matrices[0] maps a shared base to captured output and
  //                 matrices[1] maps that same base to clip
  // At draw time the second form is factored as clipFromBase *
  // inverse(captureFromBase). Exact component versions prevent a reused temp
  // register or an intervening partial write from creating a false proof.
  static D3D11PositionTransformBinding findOutputToClipTransformBinding(
      const DxbcModule& module,
      const std::string& captureSemanticName,
      uint32_t captureSemanticIndex,
      std::string* rejectionReason) {
    D3D11PositionTransformBinding result;
    auto reject = [&](const std::string& reason) {
      if (rejectionReason != nullptr)
        *rejectionReason = reason;
      return result;
    };
    const Rc<DxbcIsgn> outputSignature = module.osgn();
    if (outputSignature == nullptr || captureSemanticName.empty())
      return reject("missing output signature or capture semantic");

    auto upper = [](std::string value) {
      std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return char(std::toupper(c)); });
      return value;
    };

    const std::string wantedSemantic = upper(captureSemanticName);
    uint32_t clipRegister = UINT32_MAX;
    uint32_t captureRegister = UINT32_MAX;
    for (const DxbcSgnEntry& entry : *outputSignature) {
      const std::string semantic = upper(entry.semanticName);
      if (entry.systemValue == DxbcSystemValue::Position
       || semantic == "SV_POSITION") {
        clipRegister = entry.registerId;
      }
      if (entry.systemValue == DxbcSystemValue::None
       && semantic == wantedSemantic
       && entry.semanticIndex == captureSemanticIndex) {
        captureRegister = entry.registerId;
      }
    }
    if (clipRegister == UINT32_MAX || captureRegister == UINT32_MAX
     || clipRegister == captureRegister)
      return reject(str::format(
        "output signature did not provide distinct clip/capture registers: clip=",
        clipRegister, " capture=", captureRegister));

    struct ComponentExpression {
      D3D11CaptureExpressionKind kind = D3D11CaptureExpressionKind::Unknown;
      int32_t baseTemp = -1;
      std::array<uint32_t, 4> baseVersions = { 0, 0, 0, 0 };
      uint32_t sourceComponent = 0;
      int32_t cbSlot = -1;
      int32_t cbRegister = -1;
    };
    using VectorExpression = std::array<ComponentExpression, 4>;

    VectorExpression clip;
    VectorExpression capture;
    std::unordered_map<uint32_t, VectorExpression> temporaryExpressions;
    std::unordered_map<uint32_t, std::array<uint32_t, 4>> tempVersions;

    auto staticRegisterIndex = [](const DxbcRegister& reg, uint32_t dimension, int32_t& index) {
      if (reg.idxDim <= dimension || reg.idx[dimension].relReg != nullptr)
        return false;
      index = reg.idx[dimension].offset;
      return index >= 0;
    };
    auto versionsFor = [&](uint32_t temp) -> std::array<uint32_t, 4>& {
      return tempVersions[temp];
    };
    auto invalidate = [](VectorExpression& expression, const DxbcRegMask& mask) {
      for (uint32_t component = 0; component < 4; ++component) {
        if (mask[component])
          expression[component] = ComponentExpression();
      }
    };

    auto recordIdentity = [&](ComponentExpression& destination,
                              uint32_t sourceTemp,
                              uint32_t sourceComponent) {
      destination = {};
      destination.kind = D3D11CaptureExpressionKind::Identity;
      destination.baseTemp = int32_t(sourceTemp);
      destination.baseVersions = versionsFor(sourceTemp);
      destination.sourceComponent = sourceComponent;
    };

    auto recordDp4 = [&](ComponentExpression& destination,
                         const DxbcShaderInstruction& ins) {
      const DxbcRegister* cb = nullptr;
      const DxbcRegister* vector = nullptr;
      if (ins.src[0].type == DxbcOperandType::ConstantBuffer) {
        cb = &ins.src[0];
        vector = &ins.src[1];
      } else if (ins.src[1].type == DxbcOperandType::ConstantBuffer) {
        cb = &ins.src[1];
        vector = &ins.src[0];
      }

      int32_t vectorTemp = -1;
      int32_t cbSlot = -1;
      int32_t cbRegister = -1;
      if (cb == nullptr || vector == nullptr
       || !cb->modifiers.isClear() || !vector->modifiers.isClear()
       || cb->swizzle != DxbcRegSwizzle(0, 1, 2, 3)
       || vector->type != DxbcOperandType::Temp
       || vector->swizzle != DxbcRegSwizzle(0, 1, 2, 3)
       || !staticRegisterIndex(*vector, 0, vectorTemp)
       || !staticRegisterIndex(*cb, 0, cbSlot)
       || !staticRegisterIndex(*cb, 1, cbRegister)) {
        destination = {};
        return false;
      }

      destination = {};
      destination.kind = D3D11CaptureExpressionKind::MatrixRow;
      destination.baseTemp = vectorTemp;
      destination.baseVersions = versionsFor(uint32_t(vectorTemp));
      destination.cbSlot = cbSlot;
      destination.cbRegister = cbRegister;
      return true;
    };

    DxbcCodeSlice code = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!code.atEnd()) {
      decoder.decodeInstruction(code);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      if (ins.dstCount == 0)
        continue;

      const DxbcRegister& dst = ins.dst[0];
      int32_t dstRegister = -1;
      const bool isClipOutput = dst.type == DxbcOperandType::Output
        && staticRegisterIndex(dst, 0, dstRegister)
        && uint32_t(dstRegister) == clipRegister;
      const bool isCaptureOutput = dst.type == DxbcOperandType::Output
        && staticRegisterIndex(dst, 0, dstRegister)
        && uint32_t(dstRegister) == captureRegister;
      const bool isTemporary = dst.type == DxbcOperandType::Temp
        && staticRegisterIndex(dst, 0, dstRegister);

      VectorExpression* destination = isClipOutput
        ? &clip
        : (isCaptureOutput
          ? &capture
          : (isTemporary
            ? &temporaryExpressions[uint32_t(dstRegister)]
            : nullptr));

      bool handledWrite = false;
      if (destination != nullptr
       && ins.op == DxbcOpcode::Dp4
       && ins.dstCount == 1 && ins.srcCount == 2
       && dst.mask.popCount() == 1) {
        handledWrite = recordDp4((*destination)[dst.mask.firstSet()], ins);
      }

      if (destination != nullptr
       && ins.op == DxbcOpcode::Mov
       && ins.dstCount == 1 && ins.srcCount == 1
       && ins.src[0].type == DxbcOperandType::Temp
       && ins.src[0].modifiers.isClear()) {
        int32_t sourceTemp = -1;
        if (staticRegisterIndex(ins.src[0], 0, sourceTemp)) {
          const auto sourceExpression = temporaryExpressions.find(uint32_t(sourceTemp));
          for (uint32_t component = 0; component < 4; ++component) {
            if (!dst.mask[component])
              continue;
            const uint32_t sourceComponent = ins.src[0].swizzle[component];

            // A capture MOV defines the captured temporary itself. Do not
            // chase older arithmetic that happened to produce individual
            // components of that temporary. Clip-output MOVs, however, must
            // propagate dp4 rows calculated in an optimized temporary.
            if (!isCaptureOutput
             && sourceExpression != temporaryExpressions.end()
             && sourceExpression->second[sourceComponent].kind != D3D11CaptureExpressionKind::Unknown) {
              (*destination)[component] = sourceExpression->second[sourceComponent];
            } else {
              recordIdentity((*destination)[component],
                uint32_t(sourceTemp), sourceComponent);
            }
          }
          handledWrite = true;
        }
      }

      if (destination != nullptr && !handledWrite)
        invalidate(*destination, dst.mask);

      // Advance component versions after all source operands for this
      // instruction have been observed.
      if (isTemporary) {
        auto& versions = versionsFor(uint32_t(dstRegister));
        for (uint32_t component = 0; component < 4; ++component) {
          if (dst.mask[component])
            ++versions[component];
        }
      }
    }

    struct CollapsedExpression {
      bool identity = false;
      int32_t baseTemp = -1;
      std::array<uint32_t, 4> baseVersions = { 0, 0, 0, 0 };
      D3D11PositionTransformMatrixBinding matrix;
    };
    auto collapse = [](const VectorExpression& expression,
                       CollapsedExpression& collapsed) {
      const D3D11CaptureExpressionKind kind = expression[0].kind;
      if (kind == D3D11CaptureExpressionKind::Unknown)
        return false;

      collapsed = {};
      collapsed.identity = kind == D3D11CaptureExpressionKind::Identity;
      collapsed.baseTemp = expression[0].baseTemp;
      collapsed.baseVersions = expression[0].baseVersions;
      const int32_t cbSlot = expression[0].cbSlot;
      if (!collapsed.identity) {
        if (kind != D3D11CaptureExpressionKind::MatrixRow || cbSlot < 0)
          return false;
        collapsed.matrix.constantBufferSlot = uint32_t(cbSlot);
      }

      for (uint32_t component = 0; component < 4; ++component) {
        const ComponentExpression& source = expression[component];
        if (source.kind != kind
         || source.baseTemp != collapsed.baseTemp
         || source.baseVersions != collapsed.baseVersions)
          return false;
        if (collapsed.identity) {
          if (source.sourceComponent != component)
            return false;
        } else {
          if (source.cbSlot != cbSlot || source.cbRegister < 0)
            return false;
          collapsed.matrix.constantRegisters[component] = uint32_t(source.cbRegister);
        }
      }
      return collapsed.baseTemp >= 0;
    };

    CollapsedExpression capturedExpression;
    CollapsedExpression clipExpression;
    if (!collapse(capture, capturedExpression))
      return reject(str::format(
        "capture output o", captureRegister,
        " was not a complete identity/DP4 transform of one temporary"));
    if (!collapse(clip, clipExpression))
      return reject(str::format(
        "SV_Position o", clipRegister,
        " was not a complete DP4 transform (including MOV propagation)"));
    if (capturedExpression.baseTemp != clipExpression.baseTemp
     || capturedExpression.baseVersions != clipExpression.baseVersions)
      return reject(str::format(
        "capture and clip outputs do not consume the same temporary version: captureTemp=",
        capturedExpression.baseTemp, " clipTemp=", clipExpression.baseTemp));
    if (clipExpression.identity)
      return reject("SV_Position is already the captured vector; clip-space geometry is unsafe");

    result.valid = true;
    if (capturedExpression.identity) {
      result.matrixCount = 1;
      result.matrices[0] = clipExpression.matrix;
    } else {
      result.matrixCount = 2;
      result.matrices[0] = capturedExpression.matrix;
      result.matrices[1] = clipExpression.matrix;
    }
    return result;
  }

  // DX11_V277_REAL_SHADER_MODEL: parse the shader model version from the raw
  // DXBC container. Layout: 'DXBC' magic (4) + checksum (16) + one (4) +
  // totalSize (4) + chunkCount (4) + chunkCount x uint32 chunk offsets; each
  // chunk = fourCC (4) + size (4) + data. The SHDR (SM4) or SHEX (SM5) chunk's
  // first DWORD is the version token: bits [3:0] = minor, [7:4] = major.
  // Fully bounds-checked; returns false (caller keeps the 4.0 default) on any
  // malformed input.
  static bool parseDxbcShaderModel(
    const void* pBytecode,
    size_t      length,
    uint32_t&   outMajor,
    uint32_t&   outMinor) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pBytecode);
    if (bytes == nullptr || length < 0x20)
      return false;

    auto readU32 = [&](size_t offset) -> uint32_t {
      uint32_t v = 0;
      std::memcpy(&v, bytes + offset, sizeof(v));
      return v;
    };

    // 'DXBC' magic
    if (readU32(0) != 0x43425844u)
      return false;

    const uint32_t chunkCount = readU32(0x1C);
    if (chunkCount == 0 || chunkCount > 64)
      return false;
    if (0x20 + size_t(chunkCount) * 4 > length)
      return false;

    constexpr uint32_t kFourCcShdr = 0x52444853u; // 'SHDR'
    constexpr uint32_t kFourCcShex = 0x58454853u; // 'SHEX'

    for (uint32_t i = 0; i < chunkCount; ++i) {
      const uint32_t chunkOffset = readU32(0x20 + size_t(i) * 4);
      // Chunk header (fourCC + size) plus the version DWORD must fit.
      if (size_t(chunkOffset) + 12 > length)
        continue;

      const uint32_t fourCc = readU32(chunkOffset);
      if (fourCc != kFourCcShdr && fourCc != kFourCcShex)
        continue;

      const uint32_t versionToken = readU32(size_t(chunkOffset) + 8);
      const uint32_t minor = versionToken & 0xFu;
      const uint32_t major = (versionToken >> 4) & 0xFu;
      // D3D11 shader models are 4.0 - 5.1; reject garbage tokens.
      if (major < 4 || major > 6 || minor > 1)
        return false;

      outMajor = major;
      outMinor = minor;
      return true;
    }

    return false;
  }

  // DX11_V281_FIXED_FUNCTION: walk the SHDR/SHEX instruction stream for the
  // discard opcode (13 - covers both discard_z and discard_nz, i.e. HLSL
  // clip() and explicit discard). D3D10+ removed the fixed-function alpha
  // test; a pixel shader that discards IS this API generation's alpha test,
  // so the capture layer needs to know. Instruction skipping uses the
  // per-instruction DWORD length in OpcodeToken0 bits [30:24]; custom-data
  // blocks (opcode 53) carry their full DWORD count in the following token
  // instead. Fully bounds-checked with a hard iteration cap; returns false
  // on any malformed input.
  static bool parseDxbcUsesDiscard(
    const void* pBytecode,
    size_t      length) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pBytecode);
    if (bytes == nullptr || length < 0x20)
      return false;

    auto readU32 = [&](size_t offset) -> uint32_t {
      uint32_t v = 0;
      std::memcpy(&v, bytes + offset, sizeof(v));
      return v;
    };

    if (readU32(0) != 0x43425844u) // 'DXBC'
      return false;

    const uint32_t chunkCount = readU32(0x1C);
    if (chunkCount == 0 || chunkCount > 64)
      return false;
    if (0x20 + size_t(chunkCount) * 4 > length)
      return false;

    constexpr uint32_t kFourCcShdr = 0x52444853u; // 'SHDR'
    constexpr uint32_t kFourCcShex = 0x58454853u; // 'SHEX'

    for (uint32_t i = 0; i < chunkCount; ++i) {
      const uint32_t chunkOffset = readU32(0x20 + size_t(i) * 4);
      if (size_t(chunkOffset) + 16 > length)
        continue;

      const uint32_t fourCc = readU32(chunkOffset);
      if (fourCc != kFourCcShdr && fourCc != kFourCcShex)
        continue;

      const uint32_t chunkSize = readU32(size_t(chunkOffset) + 4);
      const size_t dataStart = size_t(chunkOffset) + 8;
      if (dataStart + chunkSize > length || chunkSize < 8)
        return false;

      // Program header: version token, then total program length in DWORDs
      // (including these two tokens). Instructions follow.
      const uint32_t programLength = readU32(dataStart + 4);
      const size_t programEnd = std::min(
        dataStart + size_t(programLength) * 4,
        dataStart + chunkSize);

      size_t pos = dataStart + 8;
      uint32_t iterations = 0;
      while (pos + 4 <= programEnd && ++iterations < (1u << 20)) {
        const uint32_t token0 = readU32(pos);
        const uint32_t opcode = token0 & 0x7FFu;

        if (opcode == 13u) // discard
          return true;

        size_t instrDwords;
        if (opcode == 53u) { // custom data: next token holds the full length
          if (pos + 8 > programEnd)
            return false;
          instrDwords = readU32(pos + 4);
          if (instrDwords < 2)
            return false;
        } else {
          instrDwords = (token0 >> 24) & 0x7Fu;
          if (instrDwords == 0)
            return false;
        }
        pos += instrDwords * 4;
      }
      return false;
    }

    return false;
  }

  // Recover only the narrow, compiler-stable cutout pattern used by foliage:
  //   sample rN, uv, tA, sA
  //   add    rN.w, rN.w, -cbC[R].x
  //   lt     rM.x, rN.w, 0
  //   discard_nz rM.x
  // The dataflow intentionally rejects every other expression. In particular,
  // an alpha-like sample combined with screen position, a control texture, or
  // branch-dependent data never becomes a legacy alpha test.
  static D3D11OpacityCutoutProfile parseDxbcOpacityCutoutProfile(
    const DxbcModule& module) {
    enum class ValueKind : uint8_t {
      Unknown,
      SampleAlpha,
      Threshold,
      AlphaMinusThreshold,
      Predicate,
      Zero,
    };

    struct Value {
      Value() noexcept
      : kind(ValueKind::Unknown)
      , alphaResourceSlot(0u)
      , thresholdConstantBufferSlot(0u)
      , thresholdConstantRegister(0u)
      , thresholdConstantComponent(0u) { }

      ValueKind kind = ValueKind::Unknown;
      uint32_t alphaResourceSlot = 0;
      uint32_t thresholdConstantBufferSlot = 0;
      uint32_t thresholdConstantRegister = 0;
      uint32_t thresholdConstantComponent = 0;
    };
    using Components = std::array<Value, 4>;

    std::unordered_map<uint32_t, Components> tempValues;

    auto staticRegisterId = [](const DxbcRegister& reg, uint32_t& outId) {
      if (reg.idxDim == 0 || reg.idx[0].relReg != nullptr || reg.idx[0].offset < 0)
        return false;
      outId = uint32_t(reg.idx[0].offset);
      return true;
    };

    auto sourceValue = [&](const DxbcRegister& reg, uint32_t destinationComponent) -> Value {
      if (reg.modifiers.test(DxbcRegModifier::Abs))
        return Value {};
      const uint32_t sourceComponent = reg.swizzle[destinationComponent];
      if (reg.type == DxbcOperandType::Temp) {
        uint32_t registerId = 0;
        if (reg.modifiers.isClear() && staticRegisterId(reg, registerId)) {
          const auto entry = tempValues.find(registerId);
          if (entry != tempValues.end())
            return entry->second[sourceComponent];
        }
        return Value {};
      }
      if (reg.type == DxbcOperandType::ConstantBuffer) {
        if (reg.idxDim != 2 || reg.idx[0].relReg != nullptr || reg.idx[1].relReg != nullptr
         || reg.idx[0].offset < 0 || reg.idx[1].offset < 0)
          return Value {};
        Value value;
        value.kind = ValueKind::Threshold;
        value.thresholdConstantBufferSlot = uint32_t(reg.idx[0].offset);
        value.thresholdConstantRegister = uint32_t(reg.idx[1].offset);
        value.thresholdConstantComponent = sourceComponent;
        return value;
      }
      if (reg.type == DxbcOperandType::Imm32) {
        if (reg.imm.u32_4[sourceComponent] == 0u) {
          Value value;
          value.kind = ValueKind::Zero;
          return value;
        }
      }
      return Value {};
    };

    auto writeValue = [&](const DxbcRegister& dst, uint32_t component, const Value& value) {
      uint32_t registerId = 0;
      if (dst.type == DxbcOperandType::Temp && staticRegisterId(dst, registerId))
        tempValues[registerId][component] = value;
    };

    D3D11OpacityCutoutProfile profile;
    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();

      if (ins.op == DxbcOpcode::Discard && ins.srcCount == 1u
       && ins.controls.zeroTest() == DxbcZeroTest::TestNz) {
        const Value condition = sourceValue(ins.src[0], 0u);
        if (condition.kind == ValueKind::Predicate) {
          const D3D11OpacityCutoutProfile candidate = {
            true,
            condition.alphaResourceSlot,
            condition.thresholdConstantBufferSlot,
            condition.thresholdConstantRegister,
            condition.thresholdConstantComponent,
          };
          if (!profile.valid)
            profile = candidate;
          else if (profile.alphaResourceSlot != candidate.alphaResourceSlot
                 || profile.thresholdConstantBufferSlot != candidate.thresholdConstantBufferSlot
                 || profile.thresholdConstantRegister != candidate.thresholdConstantRegister
                 || profile.thresholdConstantComponent != candidate.thresholdConstantComponent)
            return {};
        }
      }

      const bool samplesTexture = ins.opClass == DxbcInstClass::TextureSample
        && ins.srcCount >= 3u && ins.src[1].type == DxbcOperandType::Resource;
      uint32_t sampledResourceSlot = 0;
      const bool hasStaticSampledResource = samplesTexture
        && staticRegisterId(ins.src[1], sampledResourceSlot);

      for (uint32_t destination = 0; destination < ins.dstCount; ++destination) {
        const DxbcRegister& dst = ins.dst[destination];
        if (dst.type != DxbcOperandType::Temp)
          continue;
        for (uint32_t component = 0; component < 4u; ++component) {
          if (!dst.mask[component])
            continue;

          Value value;
          if (hasStaticSampledResource) {
            if (component == 3u) {
              value.kind = ValueKind::SampleAlpha;
              value.alphaResourceSlot = sampledResourceSlot;
            }
          } else if (ins.op == DxbcOpcode::Mov && ins.srcCount == 1u
                  && ins.src[0].modifiers.isClear()) {
            value = sourceValue(ins.src[0], component);
          } else if (ins.op == DxbcOpcode::Add && ins.srcCount == 2u) {
            const Value a = sourceValue(ins.src[0], component);
            const Value b = sourceValue(ins.src[1], component);
            const bool aNegated = ins.src[0].modifiers.test(DxbcRegModifier::Neg);
            const bool bNegated = ins.src[1].modifiers.test(DxbcRegModifier::Neg);
            const Value* sample = nullptr;
            const Value* threshold = nullptr;
            if (a.kind == ValueKind::SampleAlpha && !aNegated
             && b.kind == ValueKind::Threshold && bNegated) {
              sample = &a;
              threshold = &b;
            } else if (b.kind == ValueKind::SampleAlpha && !bNegated
                    && a.kind == ValueKind::Threshold && aNegated) {
              sample = &b;
              threshold = &a;
            }
            if (sample != nullptr) {
              value.kind = ValueKind::AlphaMinusThreshold;
              value.alphaResourceSlot = sample->alphaResourceSlot;
              value.thresholdConstantBufferSlot = threshold->thresholdConstantBufferSlot;
              value.thresholdConstantRegister = threshold->thresholdConstantRegister;
              value.thresholdConstantComponent = threshold->thresholdConstantComponent;
            }
          } else if (ins.op == DxbcOpcode::Lt && ins.srcCount == 2u
                  && ins.src[0].modifiers.isClear() && ins.src[1].modifiers.isClear()) {
            const Value a = sourceValue(ins.src[0], component);
            const Value b = sourceValue(ins.src[1], component);
            if (a.kind == ValueKind::AlphaMinusThreshold && b.kind == ValueKind::Zero) {
              value.kind = ValueKind::Predicate;
              value.alphaResourceSlot = a.alphaResourceSlot;
              value.thresholdConstantBufferSlot = a.thresholdConstantBufferSlot;
              value.thresholdConstantRegister = a.thresholdConstantRegister;
              value.thresholdConstantComponent = a.thresholdConstantComponent;
            }
          }
          writeValue(dst, component, value);
        }
      }
    }
    return profile;
  }

  // DX11_V758_KENSHI_TERRAIN_BLEND_MASK: which blendMap channels a multi-biome
  // terrain pixel shader actually consumes, read off its own instruction stream.
  //
  // Kenshi compiles one terrain pixel shader per SUBSET of neighbouring biomes,
  // and only the blendMap channels a tile uses survive that compilation. The
  // numbered diffuseMapsN arrays consume the surviving channels from high to
  // low (w,z,y,x) and the unnumbered base set takes `1 - sum(active)`.
  //
  // REFLECTION CANNOT TELL WHICH SURVIVED: the constant and resource names are
  // identical whichever subset was kept. V528 therefore carried a hand-built
  // table of 25 shader hashes, and a permutation missing from it returned 0 and
  // kept a hard, unblended biome boundary. That is a snapshot of one shader
  // cache, and it went stale - `FS_71378a88...` (channels 2|3) compiles in the
  // shipped game and was never listed.
  //
  // The bytecode says it outright. After the blend map is sampled into a temp,
  // the surviving channels are exactly the components of that temp the shader
  // goes on to READ while they are still live; the compiler reuses the dead
  // lanes for unrelated work, which is why liveness has to be tracked rather
  // than simply collecting every component ever referenced. Verified offline
  // against all 26 multi-biome terrain shaders in the cache: it reproduces all
  // 25 table entries exactly and derives 0xc for the unlisted one.
  //
  // Returns 0 for any shader that is not a multi-biome terrain permutation, and
  // for one whose shape this does not recognise - i.e. the pre-V758 behaviour
  // for an unlisted shader, a hard boundary rather than a guessed blend.
  static uint32_t parseKenshiTerrainBlendChannelMask(
    const DxbcModule& module,
    const DxbcRdef*   reflection) {
    if (reflection == nullptr || !reflection->isValid())
      return 0u;

    auto declaredTexture = [reflection](const char* needle) -> const DxbcResourceBinding* {
      for (const DxbcResourceBinding& binding : reflection->resourceBindings()) {
        if (binding.kind != DxbcResourceKind::Texture)
          continue;
        std::string lower;
        lower.reserve(binding.name.size());
        for (const char c : binding.name)
          lower += char(::tolower(static_cast<unsigned char>(c)));
        if (lower.find(needle) != std::string::npos)
          return &binding;
      }
      return nullptr;
    };

    // A blend map alone is not enough: the single-biome ground and rock shaders
    // declare none, and nothing else in the game pairs one with a NUMBERED
    // layer stack. Both together is the multi-biome terrain family and only it.
    const DxbcResourceBinding* blendMap = declaredTexture("blendmap");
    if (blendMap == nullptr || declaredTexture("diffusemaps1") == nullptr)
      return 0u;
    const uint32_t blendMapSlot = blendMap->bindPoint;

    auto staticRegisterId = [](const DxbcRegister& reg, uint32_t& outId) {
      if (reg.idxDim == 0 || reg.idx[0].relReg != nullptr || reg.idx[0].offset < 0)
        return false;
      outId = uint32_t(reg.idx[0].offset);
      return true;
    };

    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;

    bool     sampled        = false;
    uint32_t blendRegister  = 0u;
    uint32_t liveComponents = 0u;
    uint32_t usedComponents = 0u;

    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();

      if (!sampled) {
        if (ins.opClass != DxbcInstClass::TextureSample
         || ins.dstCount != 1u || ins.srcCount < 3u
         || ins.src[1].type != DxbcOperandType::Resource)
          continue;
        uint32_t resourceSlot = 0u;
        if (!staticRegisterId(ins.src[1], resourceSlot) || resourceSlot != blendMapSlot)
          continue;
        const DxbcRegister& dst = ins.dst[0];
        if (dst.type != DxbcOperandType::Temp || !staticRegisterId(dst, blendRegister))
          return 0u;
        for (uint32_t component = 0u; component < 4u; ++component)
          if (dst.mask[component])
            liveComponents |= 1u << component;
        sampled = true;
        continue;
      }

      // Nothing in this family branches between the sample and the last read of
      // it, and liveness across a branch is not tracked, so a jump means a shape
      // this does not understand. `ret` just ends the scan.
      if (ins.opClass == DxbcInstClass::ControlFlow)
        return ins.op == DxbcOpcode::Ret ? (usedComponents & 0xfu) : 0u;

      // Reads first: a source and a destination naming the same register in one
      // instruction still reads the OLD value.
      for (uint32_t source = 0u; source < ins.srcCount; ++source) {
        const DxbcRegister& src = ins.src[source];
        uint32_t registerId = 0u;
        if (src.type != DxbcOperandType::Temp
         || !staticRegisterId(src, registerId) || registerId != blendRegister)
          continue;
        // Only component-wise instructions map destination component N to
        // source swizzle entry N. A dot product or a sample reading a live
        // blend channel is a shape this does not understand.
        if ((ins.opClass != DxbcInstClass::VectorAlu
          && ins.opClass != DxbcInstClass::VectorCmp
          && ins.opClass != DxbcInstClass::VectorCmov)
         || ins.dstCount < 1u)
          return 0u;
        const DxbcRegister& dst = ins.dst[0];
        for (uint32_t component = 0u; component < 4u; ++component) {
          if (!dst.mask[component])
            continue;
          const uint32_t sourceComponent = src.swizzle[component];
          if ((liveComponents >> sourceComponent) & 1u)
            usedComponents |= 1u << sourceComponent;
        }
      }

      for (uint32_t destination = 0u; destination < ins.dstCount; ++destination) {
        const DxbcRegister& dst = ins.dst[destination];
        uint32_t registerId = 0u;
        if (dst.type != DxbcOperandType::Temp
         || !staticRegisterId(dst, registerId) || registerId != blendRegister)
          continue;
        for (uint32_t component = 0u; component < 4u; ++component)
          if (dst.mask[component])
            liveComponents &= ~(1u << component);
      }

      if (liveComponents == 0u)
        break;
    }

    return sampled ? (usedComponents & 0xfu) : 0u;
  }

  // DX11_V280_TEXCOORD_CAPTURE: scan the DXBC OUTPUT signature chunk
  // (OSGN = SM4/5, OSG5 = SM5 with streams, OSG1 = SM5.1) for a texcoord-like
  // element the stream-out capture can read back. Engine-agnostic on purpose:
  // semantic names in DXBC signatures are free-form strings chosen by each
  // engine's HLSL ("TEXCOORD", "UV", "TexUV", ...), so this matches by
  // substring preference rather than any fixed per-engine table. Requirements
  // are structural: not a system value, float components, at least .xy
  // written, stream 0. Fully bounds-checked; returns false on any malformed
  // input (caller simply skips capture support for that shader).
  static bool parseDxbcOutputTexcoord(
    const void*  pBytecode,
    size_t       length,
    std::string& outName,
    uint32_t&    outIndex,
    std::vector<D3D11TexcoordSemantic>* outSemantics = nullptr) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pBytecode);
    if (bytes == nullptr || length < 0x20)
      return false;

    auto readU32 = [&](size_t offset) -> uint32_t {
      uint32_t v = 0;
      std::memcpy(&v, bytes + offset, sizeof(v));
      return v;
    };

    // 'DXBC' magic
    if (readU32(0) != 0x43425844u)
      return false;

    const uint32_t chunkCount = readU32(0x1C);
    if (chunkCount == 0 || chunkCount > 64)
      return false;
    if (0x20 + size_t(chunkCount) * 4 > length)
      return false;

    constexpr uint32_t kFourCcOsgn = 0x4E47534Fu; // 'OSGN'
    constexpr uint32_t kFourCcOsg5 = 0x3547534Fu; // 'OSG5'
    constexpr uint32_t kFourCcOsg1 = 0x3147534Fu; // 'OSG1'

    for (uint32_t i = 0; i < chunkCount; ++i) {
      const uint32_t chunkOffset = readU32(0x20 + size_t(i) * 4);
      if (size_t(chunkOffset) + 16 > length)
        continue;

      const uint32_t fourCc = readU32(chunkOffset);
      if (fourCc != kFourCcOsgn && fourCc != kFourCcOsg5 && fourCc != kFourCcOsg1)
        continue;

      const uint32_t chunkSize = readU32(size_t(chunkOffset) + 4);
      const size_t dataStart = size_t(chunkOffset) + 8;
      if (dataStart + chunkSize > length || chunkSize < 8)
        return false;

      const uint32_t elementCount = readU32(dataStart);
      if (elementCount == 0 || elementCount > 64)
        return false;

      // OSG5/OSG1 elements lead with a uint32 stream id; OSG1 trails a
      // uint32 min-precision field. The shared fields sit at the same
      // relative offsets once the leading stream id is skipped.
      const size_t elemSize     = (fourCc == kFourCcOsgn) ? 24 : (fourCc == kFourCcOsg5 ? 28 : 32);
      const size_t nameFieldOff = (fourCc == kFourCcOsgn) ? 0 : 4;
      const size_t tableStart   = dataStart + 8;
      if (8 + size_t(elementCount) * elemSize > chunkSize)
        return false;

      bool found = false;
      int bestScore = 0;
      uint32_t bestIndex = 0;
      std::string bestName;

      for (uint32_t e = 0; e < elementCount; ++e) {
        const size_t el = tableStart + size_t(e) * elemSize;

        if (fourCc != kFourCcOsgn && readU32(el) != 0)
          continue; // only stream 0 is capturable here

        const uint32_t nameOffset    = readU32(el + nameFieldOff + 0);
        const uint32_t semanticIdx   = readU32(el + nameFieldOff + 4);
        const uint32_t systemValue   = readU32(el + nameFieldOff + 8);
        const uint32_t componentType = readU32(el + nameFieldOff + 12);
        const uint8_t  mask          = bytes[el + nameFieldOff + 20];

        if (systemValue != 0)   // skip SV_Position & friends
          continue;
        if (componentType != 3) // D3D_REGISTER_COMPONENT_FLOAT32
          continue;
        if ((mask & 0x3u) != 0x3u) // needs at least .xy written
          continue;

        if (size_t(nameOffset) >= chunkSize)
          continue;
        const char* name = reinterpret_cast<const char*>(bytes + dataStart + nameOffset);
        const size_t maxLen = chunkSize - nameOffset;
        size_t n = 0;
        while (n < maxLen && name[n] != '\0')
          ++n;
        if (n == 0 || n >= maxLen || n > 63)
          continue;

        std::string upper(name, n);
        for (auto& c : upper)
          c = char(::toupper(static_cast<unsigned char>(c)));

        if (outSemantics != nullptr) {
          D3D11TexcoordSemantic semantic;
          semantic.semanticName.assign(name, n);
          semantic.semanticIndex = semanticIdx;
          outSemantics->push_back(std::move(semantic));
        }

        int score = 0;
        if (upper.find("TEXCOORD") != std::string::npos)
          score = 3;
        else if (upper.compare(0, 2, "UV") == 0)
          score = 2;
        else if (upper.find("TEX") != std::string::npos)
          score = 1;
        if (score == 0)
          continue;

        // Prefer the strongest name match, then the lowest semantic index
        // (TEXCOORD0/UV0 is the diffuse UV set in every engine convention).
        if (!found || score > bestScore || (score == bestScore && semanticIdx < bestIndex)) {
          found = true;
          bestScore = score;
          bestIndex = semanticIdx;
          bestName.assign(name, n);
        }
      }

      if (found) {
        outName = std::move(bestName);
        outIndex = bestIndex;
        return true;
      }
      return false; // signature present, nothing texcoord-like in it
    }

    return false;
  }

  // Follow pixel-shader dataflow from texture sample coordinates back to the
  // declared input register. Optimizing HLSL compilers routinely place the
  // diffuse UV in TEXCOORD1/2 while TEXCOORD0 carries fog, lighting, or world
  // data, so selecting the lowest VS output cannot be correct across engines.
  // The analysis is deliberately conservative: a temporary carries the union
  // of input registers that feed it, and the most frequently sampled matching
  // float input wins for each texture resource slot.
  // DX11_V392_WORLD_PROJECTED_UV. Recognises the world-projected map contract
  // from REFLECTION rather than from dataflow, and corroborates it
  // structurally. The affine itself (`uv = (worldPos.xz + worldOffset.xz -
  // map.xy) * map.zw`) is the shader's, and recovering an arbitrary affine
  // symbolically is not tractable here: its coefficients are PRODUCTS of
  // constant-buffer values, so a linear-form tracker cannot represent them
  // without an expression tree. Naming the contract is the honest, bounded
  // alternative, and it was verified unambiguous: exactly one of the 254
  // cached Kenshi shaders declares `$distantColour`, and its disassembly is
  // this formula.
  //
  // The corroboration matters more than the name: the slot must be sampled
  // with a coordinate the sampled-semantic parser could NOT resolve to an
  // input register, which is precisely the "computed in the shader" case this
  // exists for. A shader that samples the same-named texture directly from a
  // TEXCOORD keeps its ordinary path.
  static D3D11WorldProjectedUvProfile parseWorldProjectedUvProfile(
      const DxbcRdef* reflection,
      const std::array<D3D11SampledTexcoordSemantic,
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>& sampledSemantics,
      const std::array<bool,
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>& sampledResources) {
    D3D11WorldProjectedUvProfile profile;
    if (reflection == nullptr || !reflection->isValid())
      return profile;

    auto lower = [](std::string value) {
      for (char& c : value)
        c = char(::tolower(static_cast<unsigned char>(c)));
      return value;
    };

    bool foundTexture = false;
    for (const auto& binding : reflection->resourceBindings()) {
      if (binding.kind != DxbcResourceKind::Texture)
        continue;
      const std::string name = lower(binding.name);
      if (name.find("distantcolour") == std::string::npos
       && name.find("distantcolor") == std::string::npos)
        continue;
      if (binding.bindPoint >= sampledSemantics.size())
        return profile;
      // Must be sampled, and its coordinate must NOT be a plain input.
      if (!sampledResources[binding.bindPoint]
       || sampledSemantics[binding.bindPoint].valid)
        return profile;
      if (foundTexture)
        return profile;  // ambiguous: two candidates in one shader
      profile.resourceSlot = binding.bindPoint;
      foundTexture = true;
    }

    if (!foundTexture)
      return profile;

    const DxbcResourceBinding* cbufferBinding =
      reflection->findBinding(DxbcResourceKind::CBuffer, 0u);
    if (cbufferBinding == nullptr)
      return profile;

    bool foundRect = false;
    for (const auto& cbuffer : reflection->constantBuffers()) {
      if (cbuffer.name != cbufferBinding->name)
        continue;
      for (const auto& variable : cbuffer.variables) {
        const std::string name = lower(variable.name);
        if (name == "map" && variable.size == 16u) {
          profile.rectByteOffset = variable.offset;
          foundRect = true;
        } else if (name == "worldoffset" && variable.size >= 12u) {
          profile.offsetByteOffset = variable.offset;
          profile.hasWorldOffset = true;
        }
      }
      break;
    }

    profile.valid = foundRect;
    return profile;
  }

  static void parseDxbcSampledTexcoords(
    const DxbcModule& module,
    std::array<D3D11SampledTexcoordSemantic,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>& outSemantics,
    std::array<bool,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>& outSampledResources,
    std::array<D3D11SampledSamplerSlot,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>& outSamplerSlots,
    bool& outResourceProfileComplete) {
    outResourceProfileComplete = true;
    const Rc<DxbcIsgn> inputSignature = module.isgn();

    static constexpr uint32_t kMaxTrackedInputs = 64u;
    struct InputOrigin {
      int32_t registerId = -1;
      int32_t component = -1;

      bool valid() const {
        return registerId >= 0 && component >= 0;
      }
    };
    using ComponentOrigins = std::array<InputOrigin, 4>;
    std::unordered_map<uint32_t, ComponentOrigins> tempOrigins;
    std::array<std::array<std::array<uint16_t, 3>, kMaxTrackedInputs>,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> directSampleCounts = {};

    auto originFor = [&](const DxbcRegister& reg,
                         uint32_t destinationComponent) -> InputOrigin {
      if (reg.idxDim == 0 || !reg.modifiers.isClear())
        return {};
      const uint32_t sourceComponent = reg.swizzle[destinationComponent];
      const int32_t registerId = reg.idx[0].offset;
      if (registerId < 0)
        return {};
      if (reg.type == DxbcOperandType::Input
       && uint32_t(registerId) < kMaxTrackedInputs)
        return { registerId, int32_t(sourceComponent) };
      if (reg.type == DxbcOperandType::Temp) {
        const auto entry = tempOrigins.find(uint32_t(registerId));
        if (entry != tempOrigins.end())
          return entry->second[sourceComponent];
      }
      return {};
    };

    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();

      const bool samplesTexture =
           ins.opClass == DxbcInstClass::TextureSample
        || ins.opClass == DxbcInstClass::TextureGather;
      if (samplesTexture && ins.srcCount >= 3u
       && ins.src[1].type == DxbcOperandType::Resource) {
        if (ins.src[1].idxDim == 0
         || ins.src[1].idx[0].relReg != nullptr
         || ins.src[1].idx[0].offset < 0) {
          outResourceProfileComplete = false;
        } else {
          const uint32_t resourceSlot = uint32_t(ins.src[1].idx[0].offset);
          if (resourceSlot < outSampledResources.size()) {
            outSampledResources[resourceSlot] = true;

            // DXBC texture and sampler registers are independent. Record the
            // exact sampler used by this resource instead of assuming tN uses
            // sN; the latter breaks atlases whenever a shader uses a different
            // binding layout. A material can carry only one sampler per
            // texture, so mixed/dynamic sampler use remains conservatively
            // unresolved and falls back to the legacy same-slot behavior.
            D3D11SampledSamplerSlot& sampledSampler =
              outSamplerSlots[resourceSlot];
            const bool hasStaticSampler =
                 ins.src[2].type == DxbcOperandType::Sampler
              && ins.src[2].idxDim != 0
              && ins.src[2].idx[0].relReg == nullptr
              && ins.src[2].idx[0].offset >= 0;
            if (!hasStaticSampler) {
              sampledSampler.valid = false;
              sampledSampler.ambiguous = true;
            } else if (!sampledSampler.ambiguous) {
              const uint32_t samplerSlot = uint32_t(ins.src[2].idx[0].offset);
              if (!sampledSampler.valid) {
                sampledSampler.samplerSlot = samplerSlot;
                sampledSampler.valid = true;
              } else if (sampledSampler.samplerSlot != samplerSlot) {
                sampledSampler.valid = false;
                sampledSampler.ambiguous = true;
              }
            }

            // Only a direct input or a chain of plain MOVs is an exact Remix
            // UV contract. Arithmetic in the pixel shader (atlas scale/bias,
            // projection, animation) cannot be reproduced by capturing the
            // original VS output and must fall back to an untextured material.
            const InputOrigin u = originFor(ins.src[0], 0u);
            const InputOrigin v = originFor(ins.src[0], 1u);
            if (u.valid() && v.valid() && u.registerId == v.registerId
             && inputSignature != nullptr) {
              const auto* input = inputSignature->find("TEXCOORD", 0u, 0u);
              if (input != nullptr && input->registerId == uint32_t(u.registerId)) {
                const uint8_t axis = u.component == 1 && v.component == 2 ? 1u
                  : u.component == 0 && v.component == 2 ? 2u
                  : u.component == 0 && v.component == 1 ? 4u : 0u;
                outSemantics[resourceSlot].projectionAxes |= axis;
              }
            }
            if (u.valid() && v.valid()
             && u.registerId == v.registerId
             && v.component == u.component + 1
             && u.component >= 0 && u.component <= 2
             && directSampleCounts[resourceSlot][uint32_t(u.registerId)]
                  [uint32_t(u.component)] != UINT16_MAX) {
              ++directSampleCounts[resourceSlot][uint32_t(u.registerId)]
                  [uint32_t(u.component)];
            }
          }
        }
      }

      for (uint32_t destination = 0; destination < ins.dstCount; ++destination) {
        const DxbcRegister& dst = ins.dst[destination];
        if (dst.type != DxbcOperandType::Temp || dst.idxDim == 0)
          continue;
        const uint32_t registerId = uint32_t(dst.idx[0].offset);
        ComponentOrigins& origins = tempOrigins[registerId];
        const bool exactMove = ins.op == DxbcOpcode::Mov
          && ins.srcCount == 1u && ins.src[0].modifiers.isClear();
        for (uint32_t component = 0; component < 4u; ++component) {
          if (!dst.mask[component])
            continue;
          origins[component] = exactMove
            ? originFor(ins.src[0], component)
            : InputOrigin();
        }
      }
    }

    if (inputSignature == nullptr)
      return;

    for (uint32_t resourceSlot = 0; resourceSlot < directSampleCounts.size(); ++resourceSlot) {
      const DxbcSgnEntry* best = nullptr;
      uint16_t bestCount = 0;
      int bestNameScore = -1;
      uint32_t bestComponent = 0;
      for (uint32_t input = 0; input < kMaxTrackedInputs; ++input) {
        for (uint32_t component = 0; component <= 2u; ++component) {
          const uint16_t count = directSampleCounts[resourceSlot][input][component];
          if (count == 0)
            continue;
          const DxbcSgnEntry* candidate = inputSignature->findByRegister(input);
          if (candidate == nullptr
           || candidate->systemValue != DxbcSystemValue::None
           || candidate->componentType != DxbcScalarType::Float32
           || !candidate->componentMask[component]
           || !candidate->componentMask[component + 1u])
            continue;

          std::string upper = candidate->semanticName;
          for (auto& c : upper)
            c = char(::toupper(static_cast<unsigned char>(c)));
          const int nameScore = upper.find("TEXCOORD") != std::string::npos ? 3
            : (upper.compare(0, 2, "UV") == 0 ? 2
            : (upper.find("TEX") != std::string::npos ? 1 : 0));
          if (best == nullptr || count > bestCount
           || (count == bestCount && nameScore > bestNameScore)) {
            best = candidate;
            bestCount = count;
            bestNameScore = nameScore;
            bestComponent = component;
          }
        }
      }
      if (best != nullptr) {
        outSemantics[resourceSlot].semanticName = best->semanticName;
        outSemantics[resourceSlot].semanticIndex = best->semanticIndex;
        outSemantics[resourceSlot].componentIndex = bestComponent;
        outSemantics[resourceSlot].valid = true;
      }
    }
  }

  static uint8_t parseKenshiProjection(const DxbcModule& module) {
    const auto reflection = module.rdef();
    const auto inputs = module.isgn();
    const auto outputs = module.osgn();
    if (reflection == nullptr || inputs == nullptr || outputs == nullptr)
      return 0u;
    const auto* binding = reflection->findBinding(DxbcResourceKind::CBuffer, 0u);
    if (binding == nullptr) return 0u;
    auto parameter = [&](const char* name, uint32_t size) -> const DxbcConstantVariable* {
      for (const auto& buffer : reflection->constantBuffers()) {
        if (buffer.name != binding->name) continue;
        for (const auto& variable : buffer.variables)
          if (variable.name == name && variable.used && variable.size == size)
            return &variable;
      }
      return nullptr;
    };
    const auto* uvOut = outputs->find("TEXCOORD", 0u, 0u);
    const auto* uvIn = inputs->find("TEXCOORD", 0u, 0u);
    const auto* blend = outputs->find("TEXCOORD", 2u, 0u);
    const auto* tiling = parameter("tiling", 8u);
    if (uvOut == nullptr) return 0u;
    if (uvIn == nullptr && blend != nullptr
     && parameter("worldOffset", 12u) && parameter("worldMatrix", 64u)) {
      if (tiling && uvOut->componentMask.popCount() == 3u
       && blend->componentMask.popCount() == 3u) return 2u;
      if (parameter("overlayData", 16u) && parameter("distortion0", 16u)
       && blend->componentMask.popCount() == 2u) return 3u;
    }
    if (tiling == nullptr || uvIn == nullptr || uvOut->componentMask.popCount() != 2u)
      return 0u;
    // Prove the direct native objects.hlsl multiplication, including packed
    // constant offsets (.xy or .zw). Reject any other write to this output.
    bool proven[2] = {};
    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const auto& ins = decoder.getInstruction();
      for (uint32_t d = 0; d < ins.dstCount; ++d) {
        const auto& dst = ins.dst[d];
        if (dst.type != DxbcOperandType::Output || dst.idxDim != 1u
         || dst.idx[0].offset != int32_t(uvOut->registerId)) continue;
        for (uint32_t c = 0; c < 2u; ++c) {
          if (!dst.mask[c]) continue;
          proven[c] = false;
          if (ins.op != DxbcOpcode::Mul || ins.srcCount != 2u || ins.modifiers.saturate) continue;
          for (uint32_t order = 0; order < 2u; ++order) {
            const auto& input = ins.src[order];
            const auto& constant = ins.src[1u - order];
            if (input.type == DxbcOperandType::Input && input.idxDim == 1u
             && input.idx[0].relReg == nullptr
             && input.idx[0].offset == int32_t(uvIn->registerId)
             && input.swizzle[c] == c && input.modifiers.isClear()
             && constant.type == DxbcOperandType::ConstantBuffer && constant.idxDim == 2u
             && constant.idx[0].offset == 0 && constant.idx[0].relReg == nullptr
             && constant.idx[1].relReg == nullptr && constant.modifiers.isClear()
             && uint32_t(constant.idx[1].offset) * 16u + constant.swizzle[c] * 4u
                  == tiling->offset + c * 4u)
              proven[c] = true;
          }
        }
      }
    }
    return proven[0] && proven[1] ? 1u : 0u;
  }

  D3D11CommonShader:: D3D11CommonShader() { }
  D3D11CommonShader::~D3D11CommonShader() { }


  D3D11CommonShader::D3D11CommonShader(
          D3D11Device*    pDevice,
    const DxvkShaderKey*  pShaderKey,
    const DxbcModuleInfo* pDxbcModuleInfo,
    const void*           pShaderBytecode,
          size_t          BytecodeLength) {
    const std::string name = pShaderKey->toString();
    Logger::debug(str::format("Compiling shader ", name));

    // DX11_V277_REAL_SHADER_MODEL: record the true shader model for this
    // shader so draw capture reports it instead of a hardcoded 4.0.
    parseDxbcShaderModel(pShaderBytecode, BytecodeLength,
                         m_shaderModelMajor, m_shaderModelMinor);

    // Cache the bytecode hash once so per-draw RTX lookups (shader-keyed material
    // identity, camera diagnostics) never rehash the container.
    if (pShaderBytecode != nullptr && BytecodeLength != 0)
      m_bytecodeHash = XXH3_64bits(pShaderBytecode, BytecodeLength);

    // DX11_V281_FIXED_FUNCTION: pixel shaders that discard are this API
    // generation's alpha test; parse once so draw capture can mark cutout
    // geometry (FillMaterialData).
    if (pShaderKey->type() == VK_SHADER_STAGE_FRAGMENT_BIT)
      m_usesDiscard = parseDxbcUsesDiscard(pShaderBytecode, BytecodeLength);
    
    DxbcReader reader(
      reinterpret_cast<const char*>(pShaderBytecode),
      BytecodeLength);
    
    DxbcModule module(reader);

    // Retain the reflection chunk (resource / constant-buffer names) so the RTX
    // capture layer can tell material inputs apart from engine-wide ones without
    // reparsing the container per draw. Null when the shader shipped stripped.
    m_reflection = module.rdef();
    if (pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT)
      m_kenshiProjection = parseKenshiProjection(module);

    if (pShaderKey->type() == VK_SHADER_STAGE_FRAGMENT_BIT
     || pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT)
      parseDxbcSampledTexcoords(
        module, m_sampledTexcoordSemantics, m_sampledResourceSlots,
        m_sampledSamplerSlots,
        m_sampledResourceProfileComplete);

    if (pShaderKey->type() == VK_SHADER_STAGE_FRAGMENT_BIT) {
      m_opacityCutoutProfile = parseDxbcOpacityCutoutProfile(module);
      // DX11_V758_KENSHI_TERRAIN_BLEND_MASK: derived once here, per unique
      // shader, and only for one that declares both a blend map and a numbered
      // layer stack - every other pixel shader pays two reflection scans.
      m_kenshiTerrainBlendChannelMask = uint8_t(
        parseKenshiTerrainBlendChannelMask(module, m_reflection.ptr()));
      m_worldProjectedUv = parseWorldProjectedUvProfile(
        m_reflection.ptr(), m_sampledTexcoordSemantics, m_sampledResourceSlots);
      if (m_worldProjectedUv.valid) {
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[Remix-DX11] V392: world-projected UV profile recovered: ps=", name,
          " t", m_worldProjectedUv.resourceSlot,
          " rect@", m_worldProjectedUv.rectByteOffset,
          " worldOffset@",
          m_worldProjectedUv.hasWorldOffset
            ? std::to_string(m_worldProjectedUv.offsetByteOffset)
            : std::string("none")));
      }

      // DX11_V546_COLOR0_TINT: record which COLOR0 components this pixel shader
      // actually reads. The capture layer needs this because vertex-colour
      // modulation was being decided by whether a COLOR0 BUFFER existed, not by
      // whether the shader consumed it - in Kenshi only 40 of 180 pixel shaders
      // do, so 140 shaders' draws could be tinted by a stream the game ignores.
      //
      // Same defect class as V543's readNamedConstant: a declared input is not a
      // read input, and the mask that says so was already in the chunk.
      const Rc<DxbcIsgn> psInputSignature = module.isgn();
      const DxbcSgnEntry* color0Entry = psInputSignature != nullptr
        ? psInputSignature->find("COLOR", 0, 0)
        : nullptr;
      if (color0Entry != nullptr) {
        m_color0UsedMask = uint8_t(
          (color0Entry->usedMask[0] ? 0x1u : 0u) |
          (color0Entry->usedMask[1] ? 0x2u : 0u) |
          (color0Entry->usedMask[2] ? 0x4u : 0u) |
          (color0Entry->usedMask[3] ? 0x8u : 0u));
      }
    }

    if (pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT) {
      const Rc<DxbcIsgn> signature = module.isgn();
      const auto usedMask = [&](const char* semantic) -> uint8_t {
        const auto* entry = signature != nullptr ? signature->find(semantic, 0, 0) : nullptr;
        if (entry == nullptr) return 0;
        return uint8_t((entry->usedMask[0] ? 1u : 0u) |
          (entry->usedMask[1] ? 2u : 0u) | (entry->usedMask[2] ? 4u : 0u) |
          (entry->usedMask[3] ? 8u : 0u));
      };
      m_skinWeightUsedMask = usedMask("BLENDWEIGHT");
      m_skinIndexUsedMask = usedMask("BLENDINDICES");
      m_positionTransform = findPositionTransformBinding(module);
      m_constantBufferDependencies = findConstantBufferDependencies(module);
    }
    
    // If requested by the user, dump both the raw DXBC
    // shader and the compiled SPIR-V module to a file.
    std::string dumpPath = kenshi_telemetry::fileOutputEnabled()
      ? env::getEnvVar("DXVK_SHADER_DUMP_PATH") : std::string();
    // Steam often launches the actual game through an already-running client,
    // so per-launch environment variables never reach the game process. Allow
    // an explicit marker beside the executable to enable the same raw DXBC/SPV
    // dump path without a registry or global environment mutation. The marker
    // is opt-in and has zero runtime cost after this creation-time check.
    if (kenshi_telemetry::fileOutputEnabled() && dumpPath.empty()
     && pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT
     && std::filesystem::exists("dx11-camera-shader-dump.flag")) {
      dumpPath = "rtx-remix/logs/dx11-camera-shaders";
      std::error_code createError;
      std::filesystem::create_directories(dumpPath, createError);
      if (createError) {
        Logger::warn(str::format(
          "[Remix-DX11] Could not create camera shader dump directory: ",
          createError.message()));
        dumpPath.clear();
      }
    }
    
    if (dumpPath.size() != 0) {
      reader.store(std::ofstream(str::tows(str::format(dumpPath, "/", name, ".dxbc").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc));
    }
    
    // Decide whether we need to create a pass-through
    // geometry shader for vertex shader stream output
    bool passthroughShader = pDxbcModuleInfo->xfb != nullptr
      && (module.programInfo().type() == DxbcProgramType::VertexShader
       || module.programInfo().type() == DxbcProgramType::DomainShader);

    if (module.programInfo().shaderStage() != pShaderKey->type() && !passthroughShader)
      throw DxvkError("Mismatching shader type.");

    m_shader = passthroughShader
      ? module.compilePassthroughShader(*pDxbcModuleInfo, name)
      : module.compile                 (*pDxbcModuleInfo, name);
    m_shader->setShaderKey(*pShaderKey);

    // Preserve only the game's original shader bytecode. Stream-output
    // capture variants are generated by the compatibility layer and must not
    // be replayed as application shaders on the next launch.
    if (pDxbcModuleInfo->xfb == nullptr)
      persistGameShaderBytecode(
        *pShaderKey, pShaderBytecode, BytecodeLength);
    
    if (dumpPath.size() != 0) {
      std::ofstream dumpStream(
        str::tows(str::format(dumpPath, "/", name, ".spv").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc);
      
      m_shader->dump(dumpStream);
    }

    // DX11_V539_SHADER_SIZE_MAP: make an Aftermath crash dump resolvable to a
    // GAME shader.
    //
    // A dump names the faulting shader only as `fragment_NN` - an Aftermath
    // placeholder, not a DXVK name - plus an opaque hash it computes itself and
    // a SIZE. The Aftermath SDK headers that would let us reproduce that hash
    // are not in this tree, and the `.nvdbg` debug info the bridge already
    // writes is compressed, so neither route identifies the shader.
    //
    // The SIZE does: it is simply the SPIR-V byte count. Logging it here, where
    // the game's own FS_/VS_ identity is still in scope, turns
    // "Shader size 11776" in a dump into a name by lookup. Emitted once per
    // unique shader (a few hundred a session), so the cost is irrelevant.
    {
      std::ostringstream spirvCounter;
      m_shader->dump(spirvCounter);
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][shader-map] ", name,
        " spirvBytes=", int64_t(spirvCounter.tellp()),
        " dxvkHash=0x", std::hex, m_shader->getHash(), std::dec));
    }
    
    // Create shader constant buffer if necessary
    if (m_shader->shaderConstants().data() != nullptr) {
      DxvkBufferCreateInfo info;
      info.size   = m_shader->shaderConstants().sizeInBytes();
      info.usage  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.stages = util::pipelineStages(m_shader->stage());
      info.access = VK_ACCESS_UNIFORM_READ_BIT;
      
      VkMemoryPropertyFlags memFlags
        = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      
      m_buffer = pDevice->GetDXVKDevice()->createBuffer(info, memFlags, DxvkMemoryStats::Category::AppBuffer, "d3d11 shader constants");

      std::memcpy(m_buffer->mapPtr(0),
        m_shader->shaderConstants().data(),
        m_shader->shaderConstants().sizeInBytes());
    }

    pDevice->GetDXVKDevice()->registerShader(m_shader);

    // DX11_V280_TEXCOORD_CAPTURE / DX11_V290_POST_VS_POSITION_CAPTURE: for
    // plain vertex shaders with recoverable output streams, retain the bytecode
    // and compile options so stream-output capture GSes can be built on demand.
    // Bounded: only VS, only when a candidate output exists, and oversized
    // blobs are skipped; the bytecode is released after the (single) build.
    if (pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT
     && pDxbcModuleInfo->xfb == nullptr
     && BytecodeLength <= (1u << 20)) {
      std::string semanticName;
      uint32_t semanticIndex = 0;
      std::vector<D3D11TexcoordSemantic> outputSemantics;
      if (parseDxbcOutputTexcoord(pShaderBytecode, BytecodeLength,
                                  semanticName, semanticIndex,
                                  &outputSemantics)) {
        m_texcoordCapture = std::make_shared<D3D11TexcoordCaptureState>();
        m_texcoordCapture->bytecode.assign(
          reinterpret_cast<const char*>(pShaderBytecode),
          reinterpret_cast<const char*>(pShaderBytecode) + BytecodeLength);
        m_texcoordCapture->options = pDxbcModuleInfo->options;
        m_texcoordCapture->semanticName = semanticName;
        m_texcoordCapture->semanticIndex = semanticIndex;
      }

      // Every graphics VS must produce SV_Position, and it is the only output
      // guaranteed to include the engine's complete skinning, morphing,
      // instancing and object/view transforms. Capture its homogeneous xyzw and
      // unproject it in the RT interleaver. This removes semantic-name/profile
      // guesses from geometry reconstruction while leaving executable profiles
      // available for material/camera policy.
      // Clean-capture-verified Kenshi/OGRE profiles whose VS exports the exact
      // world-space position as a TEXCOORD output (DXBC disassembly proven,
      // see analysis/captures/renderdoc-draw-census):
      //  - 40717da8 / e2ba1ed9: hardware-instanced world meshes; world
      //    position from per-instance transform rows, exported at TEXCOORD5.
      //  - ff800353 / 5673e857: ordinary world meshes; world position =
      //    worldMatrix * POSITION, exported at TEXCOORD5.
      //  - fe3e9f09: terrain chunks (POSITION+NORMAL layout); world position =
      //    worldMatrix * POSITION, exported at TEXCOORD4.
      struct KenshiOgreWorldProfile {
        const char* shaderName;
        uint32_t texcoordIndex;
      };
      // DISABLED pending repair of the world-space capture path.
      //
      // A per-family admission trace settled this empirically. In one frame:
      //   visible  40717da8/e2ba1ed9/6f30e40a  exactPos=0 (not captured)
      //   visible  ad6b4ffc/1eaa6ebf           exactPos=1 stride=24 (SV_Position,
      //                                        objectToWorld = camera position)
      //   INVISIBLE ff800353/5673e857/fe3e9f09 exactPos=1 stride=20 (world
      //                                        profile, objectToWorld = identity)
      // Every family on the world-position profile was invisible; every family
      // on the default homogeneous SV_Position capture or on ordinary
      // submission rendered. The defect is the world-space capture path, not
      // these shaders. Restoring them to SV_Position capture is expected to
      // bring buildings back at the cost of reintroducing camera-dependent
      // capture; the world path can be re-enabled per shader once repaired.
      static constexpr KenshiOgreWorldProfile kKenshiOgreWorldProfiles[] = {
        // ff800353 was re-enabled TEMPORARILY to drive the world-capture probe
        // in d3d11_rtx.cpp, and then never turned back off. The probe has been
        // dormant since (`s_worldCaptureProbesRemaining = 0`), so for the rest
        // of the project the largest building family has been running on the
        // very path the trace above proved makes geometry invisible - while
        // 5673e857 and fe3e9f09 were correctly restored to SV_Position capture.
        // A recent family trace shows the damage plainly: 33 draws of ff800353
        // at exactPos=1 stride=20 (world profile) against 20 draws of 5673e857
        // at stride=24 (SV_Position), i.e. buildings split across a working and
        // a broken path at the same time. That is a direct explanation for
        // "some buildings render unstably while others are never seen at all",
        // and for flicker that appears group-wise rather than uniformly.
        { "VS_ff800353fcd20376db6c4849be4d0f1504fbe79f", 5u },
      };
      // Master switch, so the table survives for the eventual repair without
      // any entry being live by accident. Re-arming a probe must also set this.
      static constexpr bool kEnableKenshiOgreWorldProfiles = false;
      bool kenshiOgreWorldPositionProfile = false;
      uint32_t kenshiOgreWorldTexcoordIndex = 0u;
      for (const auto& profile : kKenshiOgreWorldProfiles) {
        if (!kEnableKenshiOgreWorldProfiles)
          break;
        if (name != profile.shaderName)
          continue;
        // The profile is honored only when the compiled output signature
        // actually exposes the expected TEXCOORD register.
        for (const auto& outputSemantic : outputSemantics) {
          if (outputSemantic.semanticName == "TEXCOORD"
           && outputSemantic.semanticIndex == profile.texcoordIndex) {
            kenshiOgreWorldPositionProfile = true;
            kenshiOgreWorldTexcoordIndex = profile.texcoordIndex;
            break;
          }
        }
        break;
      }

      m_positionCapture = std::make_shared<D3D11PositionCaptureState>();
      m_positionCapture->bytecode.assign(
        reinterpret_cast<const char*>(pShaderBytecode),
        reinterpret_cast<const char*>(pShaderBytecode) + BytecodeLength);
      m_positionCapture->options = pDxbcModuleInfo->options;
      m_positionCapture->shaderName = name;
      m_positionCapture->semanticName = kenshiOgreWorldPositionProfile
        ? "TEXCOORD" : "SV_Position";
      m_positionCapture->semanticIndex = kenshiOgreWorldPositionProfile
        ? kenshiOgreWorldTexcoordIndex : 0u;
      m_positionCapture->positionSpace = kenshiOgreWorldPositionProfile
        ? D3D11CapturedPositionSpace::World
        : D3D11CapturedPositionSpace::View;
      m_positionCapture->homogeneousClipSpace = !kenshiOgreWorldPositionProfile;
      m_positionCapture->loadedFromProfile = kenshiOgreWorldPositionProfile;
      m_positionCapture->texcoordSemantics = std::move(outputSemantics);
      if (m_texcoordCapture != nullptr) {
        m_positionCapture->texcoordSemanticName = std::move(semanticName);
        m_positionCapture->texcoordSemanticIndex = semanticIndex;
      }
    }
  }

  bool D3D11CommonShader::GetSampledTexcoordSemantic(
      uint32_t resourceSlot,
      std::string& semanticName,
      uint32_t& semanticIndex,
      uint32_t& componentIndex) const {
    if (resourceSlot >= m_sampledTexcoordSemantics.size()
     || !m_sampledTexcoordSemantics[resourceSlot].valid)
      return false;
    semanticName = m_sampledTexcoordSemantics[resourceSlot].semanticName;
    semanticIndex = m_sampledTexcoordSemantics[resourceSlot].semanticIndex;
    componentIndex = m_sampledTexcoordSemantics[resourceSlot].componentIndex;
    return true;
  }

  bool D3D11CommonShader::ResolvePositionCaptureTexcoord(
      const std::string& requestedName,
      uint32_t requestedIndex,
      uint32_t requestedComponent,
      std::string& semanticName,
      uint32_t& semanticIndex,
      uint32_t& componentIndex) const {
    if (m_positionCapture == nullptr)
      return false;

    auto namesEqual = [](const std::string& a, const std::string& b) {
      if (a.size() != b.size())
        return false;
      for (size_t i = 0; i < a.size(); ++i) {
        if (::toupper(static_cast<unsigned char>(a[i]))
         != ::toupper(static_cast<unsigned char>(b[i])))
          return false;
      }
      return true;
    };

    if (!requestedName.empty()) {
      for (const auto& candidate : m_positionCapture->texcoordSemantics) {
        if (candidate.semanticIndex == requestedIndex
         && namesEqual(candidate.semanticName, requestedName)) {
          semanticName = candidate.semanticName;
          semanticIndex = candidate.semanticIndex;
          componentIndex = requestedComponent;
          return true;
        }
      }

      // The pixel shader identified an exact input semantic for this sampled
      // resource slot. Substituting the vertex shader's generic "best" UV
      // output when that semantic is absent associates the texture/hash with
      // unrelated geometry data (a common TEXCOORD0-vs-TEXCOORD4 Unreal
      // mismatch). An explicit request is therefore exact-or-fail.
      return false;
    }

    // No pixel-shader contract was available. Keep the generic fallback only
    // for the legacy standalone UV-capture path, which has no resource slot to
    // match and never claims that the fallback belongs to a specific hash.
    if (m_positionCapture->texcoordSemanticName.empty())
      return false;
    semanticName = m_positionCapture->texcoordSemanticName;
    semanticIndex = m_positionCapture->texcoordSemanticIndex;
    componentIndex = 0u;
    return true;
  }


  Rc<DxvkShader> D3D11CommonShader::GetTexcoordCaptureShader(
      const std::string& semanticName,
      uint32_t semanticIndex,
      uint32_t componentIndex,
      uint32_t componentCount) const {
    const std::shared_ptr<D3D11TexcoordCaptureState>& state = m_texcoordCapture;
    if (state == nullptr)
      return nullptr;

    // An empty request means "whatever this output signature nominates",
    // which is the historical behaviour and the only option for a draw whose
    // pixel shader proves no semantic for its sampled albedo slot.
    const std::string& captureName =
      semanticName.empty() ? state->semanticName : semanticName;
    const uint32_t captureIndex =
      semanticName.empty() ? state->semanticIndex : semanticIndex;
    const uint32_t captureComponent = semanticName.empty() ? 0u : componentIndex;

    std::lock_guard<dxvk::mutex> lock(state->mutex);
    uint64_t variantKey = XXH3_64bits(captureName.data(), captureName.size());
    variantKey = XXH3_64bits_withSeed(
      &captureIndex, sizeof(captureIndex), variantKey);
    variantKey = XXH3_64bits_withSeed(
      &captureComponent, sizeof(captureComponent), variantKey);
    variantKey = XXH3_64bits_withSeed(
      &componentCount, sizeof(componentCount), variantKey);
    D3D11TexcoordCaptureVariant& variant = state->variants[variantKey];
    if (variant.attempted)
      return variant.shader;
    variant.attempted = true;

    try {
      DxbcReader reader(state->bytecode.data(), state->bytecode.size());
      DxbcModule module(reader);

      // Single xfb entry: the requested VS output's two components, into
      // buffer 0 at offset 0 with stride 8. rasterizedStream = -1 turns the
      // replay pipeline into a pure capture pass: dxvk keys rasterizer discard
      // off the GS xfb stream, so the replay can never touch color or depth.
      DxbcXfbInfo xfb = {};
      xfb.entryCount = 1;
      xfb.entries[0].semanticName   = captureName.c_str();
      xfb.entries[0].semanticIndex  = captureIndex;
      xfb.entries[0].componentIndex = captureComponent;
      xfb.entries[0].componentCount = componentCount;
      xfb.entries[0].streamId       = 0;
      xfb.entries[0].bufferId       = 0;
      xfb.entries[0].offset         = 0;
      xfb.strides[0] = componentCount * 4u;
      xfb.rasterizedStream = -1;

      DxbcModuleInfo info;
      info.options = state->options;
      info.tess = nullptr;
      info.xfb = &xfb;

      Rc<DxvkShader> gs = module.compilePassthroughShader(info, "dx11_texcoord_capture_gs");
      const std::string captureContract = str::format(
        "dx11-texcoord-capture-v3:", captureName, ":", captureIndex, ":",
        captureComponent, ":", componentCount);
      const Sha1Data shaderKeyData[] = {
        { state->bytecode.data(), state->bytecode.size() },
        { captureContract.data(), captureContract.size() },
      };
      gs->setShaderKey(DxvkShaderKey(VK_SHADER_STAGE_GEOMETRY_BIT,
        Sha1Hash::compute(2, shaderKeyData)));
      variant.shader = gs;

      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11] V280: texcoord capture GS built (semantic=",
        captureName, captureIndex,
        " components=", captureComponent, "-",
        captureComponent + componentCount - 1u,
        ", default=", state->semanticName, state->semanticIndex, ")"));
    } catch (const DxvkError& e) {
      Logger::warn(str::format(
        "[Remix-DX11] V280: texcoord capture GS compile failed: ", e.message()));
    }

    // The bytecode is retained: a shader can need one variant per semantic its
    // pixel shaders sample the albedo with (the position capture path keeps it
    // for the same reason).
    return variant.shader;
  }


  Rc<DxvkShader> D3D11CommonShader::GetPositionCaptureShader(
      const std::string& texcoordSemanticName,
      uint32_t texcoordSemanticIndex,
      uint32_t texcoordComponentIndex) const {
    const std::shared_ptr<D3D11PositionCaptureState>& state = m_positionCapture;
    if (state == nullptr)
      return nullptr;

    std::lock_guard<dxvk::mutex> lock(state->mutex);
    uint64_t variantKey = XXH3_64bits(
      texcoordSemanticName.data(), texcoordSemanticName.size());
    variantKey = XXH3_64bits_withSeed(
      &texcoordSemanticIndex, sizeof(texcoordSemanticIndex), variantKey);
    variantKey = XXH3_64bits_withSeed(
      &texcoordComponentIndex, sizeof(texcoordComponentIndex), variantKey);
    D3D11PositionCaptureVariant& variant = state->variants[variantKey];
    if (variant.attempted)
      return variant.shader;
    variant.attempted = true;

    try {
      DxbcReader reader(state->bytecode.data(), state->bytecode.size());
      DxbcModule module(reader);

      DxbcXfbInfo xfb = {};
      const uint32_t positionBytes = state->homogeneousClipSpace ? 16u : 12u;
      const bool captureTexcoord = !texcoordSemanticName.empty();
      xfb.entryCount = captureTexcoord ? 2 : 1;
      xfb.entries[0].semanticName   = state->semanticName.c_str();
      xfb.entries[0].semanticIndex  = state->semanticIndex;
      xfb.entries[0].componentIndex = 0;
      xfb.entries[0].componentCount = state->homogeneousClipSpace ? 4 : 3;
      xfb.entries[0].streamId       = 0;
      xfb.entries[0].bufferId       = 0;
      xfb.entries[0].offset         = 0;
      if (captureTexcoord) {
        xfb.entries[1].semanticName   = texcoordSemanticName.c_str();
        xfb.entries[1].semanticIndex  = texcoordSemanticIndex;
        xfb.entries[1].componentIndex = texcoordComponentIndex;
        xfb.entries[1].componentCount = 2;
        xfb.entries[1].streamId       = 0;
        xfb.entries[1].bufferId       = 0;
        xfb.entries[1].offset         = positionBytes;
      }
      xfb.strides[0] = positionBytes + (captureTexcoord ? 8u : 0u);
      xfb.rasterizedStream = -1;

      DxbcModuleInfo info;
      info.options = state->options;
      info.tess = nullptr;
      info.xfb = &xfb;

      Rc<DxvkShader> gs = module.compilePassthroughShader(
        info, "dx11_position_capture_gs", true);
      const std::string captureContract = str::format(
        "dx11-position-texcoord-capture-system-value-v3:",
        texcoordSemanticName, ":", texcoordSemanticIndex, ":",
        texcoordComponentIndex);
      const Sha1Data shaderKeyData[] = {
        { state->bytecode.data(), state->bytecode.size() },
        { captureContract.data(), captureContract.size() },
      };
      gs->setShaderKey(DxvkShaderKey(VK_SHADER_STAGE_GEOMETRY_BIT,
        Sha1Hash::compute(2, shaderKeyData)));
      variant.shader = gs;

      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11] V290: post-VS position capture GS built (vs=",
        state->shaderName, ", semantic=",
        state->semanticName, state->semanticIndex,
        ", space=", state->positionSpace == D3D11CapturedPositionSpace::World
          ? "world" : "view",
        ", source=", state->homogeneousClipSpace
          ? "exact-sv-position"
          : (state->loadedFromProfile ? "profile" : "auto-discovery"),
        ", texcoord=", captureTexcoord
          ? str::format(texcoordSemanticName, texcoordSemanticIndex)
          : "none", ")"));
    } catch (const DxvkError& e) {
      Logger::warn(str::format(
        "[Remix-DX11] V290: position capture GS compile failed: ", e.message()));
    }

    return variant.shader;
  }


  D3D11ShaderModuleSet:: D3D11ShaderModuleSet() { }
  D3D11ShaderModuleSet::~D3D11ShaderModuleSet() { }
  
  
  HRESULT D3D11ShaderModuleSet::GetShaderModule(
          D3D11Device*        pDevice,
    const DxvkShaderKey*      pShaderKey,
    const DxbcModuleInfo*     pDxbcModuleInfo,
    const void*               pShaderBytecode,
          size_t              BytecodeLength,
          D3D11CommonShader*  pShader) {
    // Use the shader's unique key for the lookup
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      
      auto entry = m_modules.find(*pShaderKey);
      if (entry != m_modules.end()) {
        *pShader = entry->second;
        return S_OK;
      }
    }
    
    // This shader has not been compiled yet, so we have to create a
    // new module. This takes a while, so we won't lock the structure.
    D3D11CommonShader module;
    
    try {
      module = D3D11CommonShader(pDevice, pShaderKey,
        pDxbcModuleInfo, pShaderBytecode, BytecodeLength);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_INVALIDARG;
    }
    
    // Insert the new module into the lookup table. If another thread
    // has compiled the same shader in the meantime, we should return
    // that object instead and discard the newly created module.
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      
      auto status = m_modules.insert({ *pShaderKey, module });
      if (!status.second) {
        *pShader = status.first->second;
        return S_OK;
      }
    }
    
    *pShader = std::move(module);
    return S_OK;
  }
  
}
