// Included inside namespace dxvk, after shader/constant reflection helpers.
namespace material_probe_front {
  static uint32_t ticket = 0, rows = 0, suppressed = 0;
  static bool armed = false, beginQueued = false, pending = false;
  static uint64_t exportBytes = 0;
  static std::unordered_set<const DxvkImage*> exported;
  static thread_local const char* uvFailure = "not-attempted";

  static void endFrame() {
    if (!kenshi_telemetry::enabled()) { armed = beginQueued = pending = false; exported.clear(); return; }
    if (armed) {
      KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] frontend-end ticket=", ticket,
        " records=", rows, " suppressed=", suppressed,
        " textureExports=", exported.size(), " exportUpperBytes=", exportBytes,
        " began=", beginQueued));
      armed = false;
    }
    // Print Screen or Ctrl+Alt+O arrives before EndFrame. Capture the
    // following draw frame, after the current frame has finished submitting.
    if (std::exchange(pending, false)) {
      ++ticket;
      if (!ticket) ++ticket;
      armed = true;
      beginQueued = false;
      rows = suppressed = 0;
      exportBytes = 0;
      exported.clear();
      KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] armed ticket=", ticket,
        " trigger=screenshot-or-main-capture nextDrawFrame=1 normalDefault=off detailLimit=256 sourceExportLimit=4/128MiB",
        " scope=far-colour/UV-fallback/terrain-feature; albedo+world-position+object-picking captured if PT runs"));
    }
  }

  template<typename Emit>
  static void beginDraw(Emit emit) {
    if (!kenshi_telemetry::enabled() || !armed || beginQueued) return;
    beginQueued = true;
    const uint32_t id = ticket;
    emit([id](DxvkContext* ctx) {
      const uint32_t f = ctx->getDevice()->getCurrentFrameId();
      kenshi_material_probe::begin(id, f);
      KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] begin ticket=", id, " frame=", f));
    });
  }

  static std::string matrix(const Matrix4& m) {
    std::string result;
    for (uint32_t c = 0; c < 4; ++c)
      for (uint32_t r = 0; r < 4; ++r)
        result += str::format(c || r ? "," : "", m[c][r]);
    return result;
  }

  static bool farShader(const D3D11CommonShader* ps) {
    return ps && ps->GetName() == "FS_3b550a21753d37e9b42e92496c4e03804032e506";
  }

  template<typename Emit>
  static void record(Emit emit, const D3D11ContextState& state,
                     const DrawCallState& draw, const char* reason) {
    if (!kenshi_telemetry::enabled() || !armed || !beginQueued) return;
    const auto* ps = state.ps.shader.ptr() ? state.ps.shader->GetCommonShader() : nullptr;
    const auto* vs = state.vs.shader.ptr() ? state.vs.shader->GetCommonShader() : nullptr;
    const bool isFarTerrain = farShader(ps);
    // V744: startup terrain-params expires before the selected rock is reached.
    // Use the native family even when the translated terrain marker is missing.
    const bool isTerrainFeature = vs && vs->GetKenshiProjection() == 3u;
    if (!reason && !isFarTerrain && !isTerrainFeature) return;
    if (rows >= 256u) { ++suppressed; return; }
    ++rows;
    const auto& mat = draw.getMaterialData();
    const auto& geo = draw.getGeometryData();
    const auto& tr = draw.getTransformData();
    KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] draw ticket=", ticket,
      " id=", draw.drawCallID, " reason=", reason ? reason : (isTerrainFeature ? "feature-final" : "far-final"),
      " vs=", vs ? vs->GetName() : "none", " ps=", ps ? ps->GetName() : "none",
      " vertices=", geo.vertexCount, " indices=", geo.indexCount,
      " uvBuffer=", geo.texcoordBuffer.defined(), " texgen=", uint32_t(tr.texgenMode),
      " textured=", mat.usesTexture(), " colorSource=", uint32_t(mat.colorSource),
      " terrain=", uint32_t(mat.kenshiTerrainBlend), " albedoSlot=", mat.getColorTextureSlot(0),
      " albedoKey=", mat.getColorTexture().getUniqueKey(),
      " materialHash=", mat.getHash(), " uvFailure=", uvFailure,
      " o2wColumns=[", matrix(tr.objectToWorld), "] uvColumns=[", matrix(tr.textureTransform), "]"));

    // Actual bound bytes, including readability. No fixed cbuffer offsets.
    const char* names[] = { "map", "worldOffset", "overlayData", "biomeData",
      "distortion0", "distortion1", "textureFade", "brightnessFix", "overlayMult", "tiling",
      "scalesA", "scalesB", "scalesC", "slopeMin", "slopeMax", "slopeBlend",
      "worldMatrix", "worldViewProj", "cameraPos", "wetness", "waterHeightRel" };
    for (uint32_t stage = 0; stage < 2; ++stage) {
      const auto* shader = stage ? ps : vs;
      if (!shader) continue;
      const auto& cb = stage ? state.ps.constantBuffers[0] : state.vs.constantBuffers[0];
      for (const char* name : names) {
        float value[16] = {};
        const uint32_t count = std::strcmp(name, "worldMatrix") == 0 || std::strcmp(name, "worldViewProj") == 0 ? 16u
          : std::strcmp(name, "worldOffset") == 0 || std::strcmp(name, "cameraPos") == 0 ? 3u
          : std::strcmp(name, "tiling") == 0 ? 2u
          : std::strcmp(name, "wetness") == 0 || std::strcmp(name, "waterHeightRel") == 0 ? 1u : 4u;
        const bool readable = readNamedConstant(cb, shader, name, value, count);
        std::string values;
        for (uint32_t i = 0; i < count; ++i) values += str::format(i ? "," : "", value[i]);
        KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] constant ticket=", ticket,
          " id=", draw.drawCallID, " stage=", stage ? "PS" : "VS", " name=", name,
          " readable=", readable, " value=[", values, "]"));
      }
    }
    if (isTerrainFeature) {
      auto vec4Text = [](const Vector4& v) { return str::format(v.x, ",", v.y, ",", v.z, ",", v.w); };
      KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] translated-feature ticket=", ticket, " id=", draw.drawCallID,
        " detailScale=[", mat.kenshiTerrainDetailScale.x, ",", mat.kenshiTerrainDetailScale.y,
        "] detailOffset=[", mat.kenshiTerrainDetailOffset.x, ",", mat.kenshiTerrainDetailOffset.y,
        "] slopeMin=[", vec4Text(mat.kenshiTerrainSlopeMin), "] slopeMax=[", vec4Text(mat.kenshiTerrainSlopeMax),
        "] slopeBlend=[", vec4Text(mat.kenshiTerrainSlopeBlend), "] overlayMult=[", vec4Text(mat.kenshiTerrainOverlayMult),
        "] brightness=", mat.kenshiTerrainBrightnessFix, " heightOffset=", mat.kenshiTerrainHeightOffset,
        " heightWarp=[", mat.kenshiTerrainHeightWarp.x, ",", mat.kenshiTerrainHeightWarp.y,
        "] overlayKey=", mat.kenshiTerrainOverlay.getUniqueKey()));
      for (uint32_t layer = 0; layer < LegacyMaterialData::kKenshiTerrainLayerCount; ++layer)
        KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] translated-layer ticket=", ticket, " id=", draw.drawCallID,
          " layer=", layer, " key=", mat.kenshiTerrainLayers[layer].getUniqueKey(),
          " scale=[", mat.kenshiTerrainLayerScales[layer].x, ",", mat.kenshiTerrainLayerScales[layer].y, "]"));
    }
    for (uint32_t slot = 0; ps && slot < textureResourceSlotCount(ps); ++slot) {
      if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) break;
      const auto& name = getTextureResourceName(ps, slot);
      if (name.empty()) continue;
      const auto* srv = state.ps.shaderResources.views[slot].ptr();
      const Rc<DxvkImageView> view = srv ? srv->GetImageView() : nullptr;
      if (view == nullptr || view->image() == nullptr) {
        KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] texture ticket=", ticket,
          " id=", draw.drawCallID, " slot=", slot, " name=", name, " bound=0"));
        continue;
      }
      const auto& info = view->image()->info();
      const auto& vi = view->info();
      uint32_t samplerSlot = slot;
      const bool samplerKnown = ps->GetSampledSamplerSlot(slot, samplerSlot);
      D3D11_SAMPLER_DESC sampler = {};
      const bool samplerBound = samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
        && state.ps.samplers[samplerSlot] != nullptr;
      if (samplerBound) state.ps.samplers[samplerSlot]->GetDesc(&sampler);
      std::string semantic;
      uint32_t semanticIndex = 0, component = 0;
      const bool uvKnown = ps->GetSampledTexcoordSemantic(slot, semantic, semanticIndex, component);
      KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] texture ticket=", ticket,
        " id=", draw.drawCallID, " slot=", slot, " name=", name,
        " key=", TextureRef(view).getUniqueKey(), " image=", view->image().ptr(),
        " size=", info.extent.width, "x", info.extent.height, " format=", uint32_t(vi.format),
        " mips=", info.mipLevels, " baseMip=", vi.minLevel, " viewMips=", vi.numLevels,
        " baseLayer=", vi.minLayer, " viewLayers=", vi.numLayers,
        " uvKnown=", uvKnown, " semantic=", semantic, semanticIndex, ".", component,
        " samplerKnown=", samplerKnown, " samplerBound=", samplerBound, " sampler=", samplerSlot,
        " address=", uint32_t(sampler.AddressU), ",", uint32_t(sampler.AddressV),
        " filter=", uint32_t(sampler.Filter), " bias=", sampler.MipLODBias,
        " minLod=", sampler.MinLOD, " maxLod=", sampler.MaxLOD));

      // Export only colour/overlay maps needed to reproduce this tint lookup.
      // Preserve all mips and keep the existing one-shot count/byte budgets.
      const bool featureMap = isTerrainFeature
        && (name == "colourMap" || name == "overlayMap");
      if (kenshi_telemetry::fileOutputEnabled()
       && ((isFarTerrain && slot == 0u) || featureMap) && !exported.count(view->image().ptr())) {
        const bool rgba8 = info.format == VK_FORMAT_B8G8R8A8_UNORM || info.format == VK_FORMAT_B8G8R8A8_SRGB
          || info.format == VK_FORMAT_R8G8B8A8_UNORM || info.format == VK_FORMAT_R8G8B8A8_SRGB;
        // 8 bytes/base texel bounds a full RGBA8 mip chain, including 1D images.
        const uint64_t upperBytes = uint64_t(info.extent.width) * info.extent.height
          * std::max(info.extent.depth, 1u) * std::max(info.numLayers, 1u) * (rgba8 ? 8u : 32u);
        if (exported.size() < 4u && upperBytes <= (128ull << 20) - exportBytes
         && info.sampleCount == VK_SAMPLE_COUNT_1_BIT) {
          exported.insert(view->image().ptr());
          exportBytes += upperBytes;
          const std::string label = str::format("materialProbe-", ticket,
            "-source-", TextureRef(view).getUniqueKey(), "-pid", ::GetCurrentProcessId());
          const auto image = view->image();
          emit([image, label](DxvkContext* ctx) {
            std::string path = env::getEnvVar("DXVK_SCREENSHOT_PATH");
            if (path.empty()) path = "./Screenshots/";
            else if (path.back() != '/') path += '/';
            const std::string name = str::format(label, "-f", ctx->getDevice()->getCurrentFrameId(), ".dds");
            KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] source-export path=", path, name));
            ctx->getDevice()->getCommon()->metaExporter().dumpImageToFile(ctx, path, name, image);
          });
        } else {
          KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] source-export-skipped ticket=", ticket,
            " key=", TextureRef(view).getUniqueKey(), " reason=budget-or-MSAA"));
        }
      }
    }
  }
}
