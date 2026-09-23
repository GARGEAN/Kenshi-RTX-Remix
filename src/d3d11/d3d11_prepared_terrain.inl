// Included inside namespace dxvk, immediately before SubmitDraw. No class layout changes.
namespace prepared_terrain_detail {
  using prepared_terrain::Key;
  // V701: first failing guard per draw; one descriptor sample per reason/window.
  enum Reason {
    Disabled, Candidate, RenderDoc, Emulator, SkinFuture, SmallDraw, Trace,
    Significance, SkyTags, FallbackCamera, RelativeCamera, ExactCamera, Offscreen,
    IdentityView, NoPreviousScene, PsReflection, ExtraPsCb, GeometryShader,
    HullShader, DomainShader, Predicate, StreamOutput, PsUav, VsSrv,
    Bones, BonesPerVertex, PostVsHash, PostVsIdentity, ColorStream, Instances,
    NativeBufferPolicy, RevisionCapacity, CbMissing, CbOffset, CbExtent, CbUnmapped,
    PsNonImageSrv, ReasonCount
  };
  static constexpr const char* reasonNames[] = {
    "disabled", "candidate", "renderDoc", "emulator", "skinFuture", "smallDraw", "trace",
    "significance", "skyTags", "fallbackCamera", "relativeCamera", "exactCamera", "offscreen",
    "identityView", "noPreviousScene", "psReflection", "extraPsCb", "geometryShader",
    "hullShader", "domainShader", "predicate", "streamOutput", "psUav", "vsSrv",
    "bones", "bonesPerVertex", "postVsHash", "postVsIdentity", "colorStream", "instances",
    "nativeBufferPolicy", "revisionCapacity", "cbMissing", "cbOffset", "cbExtent", "cbUnmapped",
    "psNonImageSrv"
  };
  static_assert(sizeof(reasonNames) / sizeof(reasonNames[0]) == ReasonCount);
  struct Rejection {
    uint64_t count = 0, slot = UINT32_MAX, a = 0, b = 0, c = 0, d = 0, e = 0;
  };
  static thread_local std::array<Rejection, ReasonCount> rejections {};
  static bool reject(Reason reason, uint64_t slot = UINT32_MAX,
      uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0) {
    auto& r = rejections[reason];
    if (r.count++ == 0) { r.slot = slot; r.a = a; r.b = b; r.c = c; r.d = d; r.e = e; }
    return false;
  }
  struct Entry {
    Key input;
    D3D11ContextState retainedState;
    DrawCallState draw;
    DrawParameters params {};
    uint64_t normalRevision = 0, uvRevision = 0;
    uint64_t normalKey = 0, uvKey = 0;
    uint32_t lastUsed = 0;
  };
  struct Cache {
    const void* owner = nullptr;
    uint64_t epoch = 0;
    uint32_t frame = 0, windows = 0;
    uint64_t considered = 0, hits = 0, misses = 0, seeds = 0, fallback = 0;
    uint64_t verified = 0, mismatch = 0;
    uint64_t materialHits = 0, materialSeeds = 0, movingDraws = 0, movingHits = 0, movingMaterialHits = 0;
    uint64_t ownerResets = 0, epochResets = 0, frameResets = 0;
    uint64_t fullExpired = 0, materialExpired = 0, attributeExpired = 0;
    uint64_t lookupAbsent = 0, lookupInput = 0, lookupAge = 0, lookupOutput = 0;
    uint32_t cameraFrame = ~0u;
    Matrix4 lastView, lastProjection;
    bool haveCamera = false, moving = false;
    struct Material {
      Key input;
      D3D11ContextState retainedState;
      LegacyMaterialData base, terrain;
      uint32_t lastUsed = 0;
    };
    std::unordered_map<uint64_t, Material> materials;
    std::unordered_map<uint64_t, Entry> entries;
  };
  static thread_local Cache cache;
  struct VerificationGuard {
    bool pending = false;
    ~VerificationGuard() {
      if (!pending) return;
      ++cache.mismatch;
      prepared_terrain::enabled.store(false, std::memory_order_relaxed);
      prepared_terrain::verifyRemaining.store(0, std::memory_order_relaxed);
      cache.entries.clear();
      Logger::err("[PreparedTerrain V700] reference draw did not reach comparison; reuse disabled.");
    }
  };

  static void transformKey(Key& k, const DrawCallTransforms& t) {
    k.add(t.objectToWorld); k.add(t.objectToView); k.add(t.worldToView);
    k.add(t.viewToProjection); k.add(t.textureTransform); k.add(t.enableClipPlane);
    k.add(t.clipPlane); k.add(t.texgenMode); k.add(t.usedViewportFallbackProjection);
    k.add(t.cameraRelativeView); k.add(t.exactReplacementCamera); k.add(t.offscreenRenderTarget);
  }
  static void bufferKey(Key& k, const RasterBuffer& b) {
    k.add(b.defined());
    if (!b.defined()) return;
    k.add(b.buffer().ptr()); k.add(b.offset()); k.add(b.length());
    k.add(b.offsetFromSlice()); k.add(b.stride()); k.add(b.vertexFormat());
  }
  static void geometryKey(Key& k, const RasterGeometry& g) {
    k.add(g.vertexCount); k.add(g.indexCount); k.add(g.topology); k.add(g.cullMode);
    k.add(g.frontFace); k.add(g.forceCullBit); k.add(g.indexRangeUnvalidated);
    bufferKey(k, g.positionBuffer); bufferKey(k, g.indexBuffer);
    bufferKey(k, g.normalBuffer); bufferKey(k, g.texcoordBuffer); bufferKey(k, g.color0Buffer);
  }
  static bool constantKey(Key& k, const D3D11ConstantBufferBinding& cb, uint32_t stage) {
    k.add(cb.buffer.ptr()); k.add(cb.constantOffset); k.add(cb.constantCount); k.add(cb.constantBound);
    if (!cb.buffer.ptr()) return reject(CbMissing, stage);
    const size_t size = cb.buffer->Desc()->ByteWidth;
    const size_t base = size_t(cb.constantOffset) * 16;
    if (base >= size) return reject(CbOffset, stage, size, base, cb.constantCount);
    const size_t end = cb.constantCount ? std::min(size, base + size_t(cb.constantCount) * 16) : size;
    if (end <= base || end - base > 4096) return reject(CbExtent, stage, size, base, end);
    const auto slice = cb.buffer->GetMappedSlice();
    if (!slice.mapPtr) return reject(CbUnmapped, stage, size, base, end);
    k.add(end - base);
    k.data(static_cast<const uint8_t*>(slice.mapPtr) + base, end - base);
    return true;
  }
  static bool inputKey(Key& k, const D3D11ContextState& s, const DrawCallState& d,
                       bool indexed, UINT count, UINT start, INT base) {
    // Ground's two known VS families use b0 only. Keep the complete PS b0 bytes
    // and decline shaders with other CB bindings, instead of guessing dependencies.
    const auto* ps = s.ps.shader.ptr() ? s.ps.shader->GetCommonShader() : nullptr;
    if (!ps || !ps->GetReflection() || !ps->GetReflection()->isValid()) return reject(PsReflection);
    for (uint32_t slot = 1; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot)
      if (ps->GetReflection()->findBinding(DxbcResourceKind::CBuffer, slot)) return reject(ExtraPsCb, slot);
    if (s.gs.shader.ptr()) return reject(GeometryShader);
    if (s.hs.shader.ptr()) return reject(HullShader);
    if (s.ds.shader.ptr()) return reject(DomainShader);
    if (s.pr.predicateObject.ptr()) return reject(Predicate);
    for (uint32_t i = 0; i < s.so.targets.size(); ++i)
      if (s.so.targets[i].buffer.ptr()) return reject(StreamOutput, i);
    for (uint32_t i = 0; i < s.ps.unorderedAccessViews.size(); ++i)
      if (s.ps.unorderedAccessViews[i].ptr()) return reject(PsUav, i);
    // Known ground VS has no SRV inputs; stale bound views cannot affect its outputs.
    if (d.getSkinningState().numBones) return reject(Bones, UINT32_MAX, d.getSkinningState().numBones);
    if (d.getGeometryData().numBonesPerVertex) return reject(BonesPerVertex, UINT32_MAX, d.getGeometryData().numBonesPerVertex);
    if (d.getGeometryData().hasPostVsPositionHashSeed) return reject(PostVsHash);
    if (d.getGeometryData().postVsCaptureIdentity) return reject(PostVsIdentity);
    if (d.getGeometryData().color0Buffer.defined()) return reject(ColorStream);
    if (d.getTransformData().instancesToObject) return reject(Instances);
    k.bytes.reserve(4096);
    k.add(indexed); k.add(count); k.add(start); k.add(base);
    k.add(s.vs.shader.ptr()); k.add(s.ps.shader.ptr()); k.add(s.ia.inputLayout.ptr());
    k.add(s.ia.primitiveTopology);
    auto nativeBuffer = [&](D3D11Buffer* b, uint32_t slot) {
      k.add(b);
      if (!b) return true;
      const auto* desc = b->Desc();
      // Writable Map, CopyBuffer and UpdateBuffer have version hooks. Opaque GPU/external writes do not.
      if ((desc->BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT))
       || (desc->MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
                            | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)))
        return reject(NativeBufferPolicy, slot, desc->Usage, desc->CPUAccessFlags,
          desc->BindFlags, desc->MiscFlags, desc->ByteWidth);
      const auto rev = prepared_terrain::revision(b);
      if (rev == UINT64_MAX) return reject(RevisionCapacity, slot);
      k.add(rev); k.add(b->GetVertexShadowRevision());
      return true;
    };
    const auto slots = terrain_dependencies::vertexSlots(s);
    for (uint32_t slot = 0; slot < s.ia.vertexBuffers.size(); ++slot) {
      if (!(slots & (1u << slot))) continue;
      const auto& vb = s.ia.vertexBuffers[slot];
      if (!nativeBuffer(vb.buffer.ptr(), slot)) return false;
      k.add(vb.offset); k.add(vb.stride);
    }
    if (!nativeBuffer(s.ia.indexBuffer.buffer.ptr(), UINT32_MAX)) return false;
    k.add(s.ia.indexBuffer.offset); k.add(s.ia.indexBuffer.format);
    // Material dependencies prefix this key. Only morph and world3x3 additionally
    // affect captured ground attributes; current view/projection is patched on hits.
    if (!terrain_dependencies::named(k, s, "morph", 1)) return reject(CbMissing, 0);
    float world[16] {};
    if (!readNamedConstant(s.vs.constantBuffers[0], s.vs.shader->GetCommonShader(), "worldMatrix", world, 16))
      return reject(CbMissing, 0);
    for (uint32_t c = 0; c < 3; ++c) for (uint32_t r = 0; r < 3; ++r) k.add(world[c*4+r]);
    for (const auto& rt : s.om.renderTargetViews) k.add(rt.ptr());
    k.add(s.om.depthStencilView.ptr()); k.add(s.om.cbState); k.add(s.om.dsState);
    k.add(s.om.blendFactor); k.add(s.om.sampleMask); k.add(s.om.stencilRef);
    k.add(s.rs.state); k.add(s.rs.numViewports); k.add(s.rs.numScissors);
    for (uint32_t i = 0; i < s.rs.numViewports; ++i) k.add(s.rs.viewports[i]);
    for (uint32_t i = 0; i < s.rs.numScissors; ++i) k.add(s.rs.scissors[i]);
    geometryKey(k, d.getGeometryData());
    k.add(d.zEnable); k.add(d.zWriteEnable); k.add(d.stencilEnabled);
    k.add(d.minZ); k.add(d.maxZ); k.add(d.allowMainCameraUpdate);
    return true;
  }
  static bool streamsUnchanged(const Entry& e) {
    const auto& g = e.draw.getGeometryData();
    return (!g.normalBuffer.defined() || e.normalRevision == prepared_terrain::revision(g.normalBuffer.buffer().ptr()))
        && (!g.texcoordBuffer.defined() || e.uvRevision == prepared_terrain::revision(g.texcoordBuffer.buffer().ptr()));
  }
  // Finite validation compares resolved output, not a second submission to Remix.
  static void textureKey(Key& k, const TextureRef& t) {
    k.add(t.isValid());
    if (t.isValid()) { k.add(t.getUniqueKey()); k.add(t.getImageHash()); }
    k.add(t.getImageView());
  }
  static void materialKey(Key& k, const LegacyMaterialData& m) {
    // Terrain's resolved values and resource identities, without object padding or Rc counters.
    k.add(m.getHash()); k.add(m.getTextureSetAndShaderHash());
    textureKey(k, m.getColorTexture()); textureKey(k, m.getColorTexture2());
    k.add(m.getSampler().ptr()); k.add(m.getSampler2().ptr());
    k.add(m.getColorTextureSlot(0)); k.add(m.getColorTextureSlot(1));
    k.add(m.alphaTestEnabled); k.add(m.alphaTestReferenceValue); k.add(m.alphaTestCompareOp);
    k.add(m.blendMode.enableBlending); k.add(m.blendMode.colorSrcFactor);
    k.add(m.blendMode.colorDstFactor); k.add(m.blendMode.colorBlendOp);
    k.add(m.blendMode.alphaSrcFactor); k.add(m.blendMode.alphaDstFactor);
    k.add(m.blendMode.alphaBlendOp); k.add(m.blendMode.writeMask);
    k.add(m.colorSource); k.add(m.alphaSource); k.add(m.useSecondaryTextureForOpacity);
    k.add(m.kenshiTerrainBlend); k.add(m.kenshiRain); k.add(m.kenshiCharacterHead);
    k.add(m.kenshiDualTextureSet); k.add(m.kenshiColorMask); k.add(m.kenshiCharacterVest);
    k.add(m.kenshiBloodMode); k.add(m.kenshiNormalEncoding); k.add(m.kenshiGlossMult);
    // DX11_V766: the green flip and the muscle blend. Both must be here as well
    // as in the reuse key - this is what the V708 verifier compares, and a
    // difference it cannot see becomes a reported MISMATCH, which sets
    // `c.enabled = false` and disables material reuse for the whole process.
    k.add(m.kenshiNormalFlipGreen); k.add(m.kenshiMuscleBlend);
    textureKey(k, m.kenshiNormalTexture); textureKey(k, m.kenshiMetalTexture);
    textureKey(k, m.kenshiCharacterBlendNormalTexture);
    k.add(m.kenshiTerrainDetailScale); k.add(m.kenshiTerrainDetailOffset);
    for (const auto& t : m.kenshiTerrainLayers) textureKey(k, t);
    k.add(m.kenshiTerrainLayerScales); textureKey(k, m.kenshiTerrainOverlay);
    k.add(m.kenshiTerrainSlopeMin); k.add(m.kenshiTerrainSlopeMax);
    k.add(m.kenshiTerrainSlopeBlend); k.add(m.kenshiTerrainOverlayMult);
    k.add(m.kenshiTerrainBrightnessFix); k.add(m.kenshiTerrainHeightOffset);
    k.add(m.kenshiTerrainHeightWarp); k.add(m.kenshiTerrainBiomeKey);
    textureKey(k, m.kenshiTerrainBiomeBlend);
    k.add(m.kenshiTerrainBiomeScale); k.add(m.kenshiTerrainBiomeOffset);
    k.add(m.kenshiTerrainBlendChannelMask); k.add(m.modulateVertexColor); k.add(m.modulateVertexAlpha);
    k.add(m.colorTextureChannel); k.add(m.opacityTextureChannel); k.add(m.blendConstant);
    k.add(m.dx11Material); k.add(m.isTextureFactorBlend); k.add(m.isVertexColorBakedLighting);
    k.add(m.colorTextureIsSrgb); k.add(m.constantAlbedo); k.add(m.hasConstantAlbedo);
    textureKey(k, m.kenshiDustNoiseTexture); k.add(m.kenshiDustColour);
  }
  static bool sameOutput(const DrawCallState& a, const DrawCallState& b) {
    Key ka, kb;
    geometryKey(ka, a.getGeometryData()); geometryKey(kb, b.getGeometryData());
    transformKey(ka, a.getTransformData()); transformKey(kb, b.getTransformData());
    ka.add(a.getGeometryData().hashes); kb.add(b.getGeometryData().hashes);
    ka.add(a.getCategoryFlags().raw()); kb.add(b.getCategoryFlags().raw());
    ka.add(a.zEnable); kb.add(b.zEnable); ka.add(a.zWriteEnable); kb.add(b.zWriteEnable);
    materialKey(ka, a.getMaterialData()); materialKey(kb, b.getMaterialData());
    ka.add(a.allowMainCameraUpdate); kb.add(b.allowMainCameraUpdate);
    ka.add(a.isDrawingToRaytracedRenderTarget); kb.add(b.isDrawingToRaytracedRenderTarget);
    ka.add(a.isUsingRaytracedRenderTarget); kb.add(b.isUsingRaytracedRenderTarget);
    ka.add(a.getFogState()); kb.add(b.getFogState());
    return ka == kb;
  }
}
