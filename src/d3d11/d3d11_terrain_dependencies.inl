// Included inside dxvk after readNamedConstant, before attribute capture.
namespace terrain_dependencies {
  using prepared_terrain::Key;
  static bool ground(const D3D11CommonShader* vs) {
    return vs && (vs->GetName() == "VS_7507b1ef7b8fa1e2114493eaacd2a3cf653fae73"
               || vs->GetName() == "VS_96854ea3bb6b1014c5bae3a2f72f27667b393bea");
  }
  static uint32_t vertexSlots(const D3D11ContextState& s) {
    uint32_t mask = 0;
    if (s.ia.inputLayout.ptr())
      for (const auto& a : s.ia.inputLayout->GetRtxSemantics())
        if (a.inputSlot < 32) mask |= 1u << a.inputSlot;
    return mask;
  }
  static bool source(Key& k, D3D11Buffer* b) {
    k.add(b);
    if (!b) return true;
    const auto& d = *b->Desc();
    if ((d.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT))
     || (d.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
                      | D3D11_RESOURCE_MISC_SHARED_NTHANDLE))) return false;
    const auto revision = prepared_terrain::revision(b);
    if (revision == UINT64_MAX) return false;
    k.add(revision); k.add(b->GetVertexShadowRevision());
    return true;
  }
  static bool vertices(Key& k, const D3D11ContextState& s) {
    const auto mask = vertexSlots(s);
    if (!mask) return false;
    k.add(s.ia.inputLayout.ptr()); k.add(mask);
    for (uint32_t i = 0; i < 32; ++i) if (mask & (1u << i)) {
      const auto& vb = s.ia.vertexBuffers[i];
      if (!source(k, vb.buffer.ptr())) return false;
      k.add(vb.offset); k.add(vb.stride);
    }
    return true;
  }
  static bool named(Key& k, const D3D11ContextState& s, const char* name, uint32_t count) {
    float value[16] {};
    const bool have = readNamedConstant(s.vs.constantBuffers[0], s.vs.shader->GetCommonShader(), name, value, count);
    k.add(have);
    if (have) k.data(value, count * sizeof(float));
    return have;
  }
  static bool frameConstant(const std::string& name) {
    return name == "viewport" || name == "farClip" || name == "cameraPos"
        || name == "waterHeightRel" || name == "wetness";
  }
  static bool materialInput(Key& k, const D3D11ContextState& s, float heightOffset) {
    const auto* vs = s.vs.shader.ptr() ? s.vs.shader->GetCommonShader() : nullptr;
    const auto* ps = s.ps.shader.ptr() ? s.ps.shader->GetCommonShader() : nullptr;
    if (!ground(vs) || !ps || !ps->GetReflection() || !ps->GetReflection()->isValid()) return false;
    k.add(vs); k.add(ps);
    const auto* reflection = ps->GetReflection();
    const auto* binding = reflection->findBinding(DxbcResourceKind::CBuffer, 0);
    if (!binding) return false;
    for (const auto& b : reflection->resourceBindings())
      if (b.kind == DxbcResourceKind::CBuffer && (b.bindPoint != 0 || b.bindCount != 1)) return false;
    const auto& cb = s.ps.constantBuffers[0];
    if (!cb.buffer.ptr()) return false;
    const size_t size = cb.buffer->Desc()->ByteWidth, base = size_t(cb.constantOffset) * 16;
    const size_t end = cb.constantCount ? std::min(size, base + size_t(cb.constantCount) * 16) : size;
    const auto slice = cb.buffer->GetMappedSlice();
    if (!slice.mapPtr || base >= end) return false;
    bool found = false;
    for (const auto& block : reflection->constantBuffers()) if (block.name == binding->name) {
      found = true;
      for (const auto& v : block.variables) {
        if (frameConstant(v.name)) continue;
        if (v.offset > end - base || v.size > end - base - v.offset) return false;
        k.data(static_cast<const uint8_t*>(slice.mapPtr) + base + v.offset, v.size);
      }
      break;
    }
    if (!found) return false;
    // These fields are read by the bridge even in shader permutations that do not use them.
    named(k, s, "overlayData", 4); named(k, s, "biomeData", 4); named(k, s, "distortion0", 4);
    k.add(heightOffset);
    for (uint32_t i = 0; i < s.ps.shaderResources.views.size(); ++i) {
      if (!ps->DeclaresTextureBinding(i)) continue;
      const auto& srv = s.ps.shaderResources.views[i]; k.add(i); k.add(srv.ptr());
      if (srv.ptr()) {
        auto view = srv->GetImageView();
        if (!view.ptr() || !view->image().ptr()) return false;
        k.add(view->cookie()); k.add(view->image()->getHash());
      }
    }
    for (uint32_t i = 0; i < s.ps.samplers.size(); ++i)
      if (reflection->findBinding(DxbcResourceKind::Sampler, i)) { k.add(i); k.add(s.ps.samplers[i]); }
    k.add(s.om.cbState); k.add(s.om.dsState); k.add(s.om.blendFactor);
    k.add(s.om.sampleMask); k.add(s.om.stencilRef);
    return true;
  }
  // DX11_V752_KENSHI_WETNESS_LATCH: publish the shared weather wetness at most
  // once per frame.
  //
  // `wetness` and `waterHeightRel` are Ogre SHARED params (common.program,
  // block SharedWaterParams) - one value for the whole frame. Ogre materialises
  // shared params into EACH program's own constant buffer, and the bridge reads
  // them back off b0 of whatever pixel shader the current draw happens to bind.
  // That read happens from two sites: FillMaterialData on a material cache MISS,
  // and globals() here on the cache HIT path, i.e. nearly every draw. Both
  // simply stored the value, so this was last-writer-wins across every
  // qualifying draw and the value the shader saw depended on draw order.
  //
  // The shader gates the entire wetness term on `cb.kenshiWetness > 0.0f`
  // (opaque_surface_material_interaction.slangh), a branch Kenshi itself does
  // not have - every deferred shader there calls makeWet unconditionally. So a
  // frame whose last writer published a different value - 0 in particular, which
  // the `>= 0 && <= 1` guard accepts from a b0 that never received the param -
  // stepped every surface's albedo by up to 20%. Measured as a uniform 0.818x
  // two-level flicker on all path-traced surfaces, on every light including
  // local ones, absent from the rasterised sky matte, immune to the denoiser,
  // exposure, RTXDI and ReSTIR GI, and gone entirely with the option off.
  //
  // First qualifying read of a frame wins; later reads are ignored. That removes
  // the draw-order dependence without inventing a value. The temporary V753
  // first-300-frame disagreement experiment has been retired.
  static uint32_t wetnessFrame = 0xffffffffu;
  static void publishWetness(float wetness, float height, uint32_t frameId) {
    if (frameId != wetnessFrame) {
      wetnessFrame = frameId;
      SceneManager::registerKenshiWetness(wetness, height);
    }
  }

  static void globals(const D3D11ContextState& s, uint32_t frameId) {
    float wetness[1] {}, height[1] {};
    auto* ps = s.ps.shader.ptr() ? s.ps.shader->GetCommonShader() : nullptr;
    if (ps && readNamedConstant(s.ps.constantBuffers[0], ps, "wetness", wetness, 1)
     && readNamedConstant(s.ps.constantBuffers[0], ps, "waterHeightRel", height, 1)
     && wetness[0] >= 0 && wetness[0] <= 1) publishWetness(wetness[0], height[0], frameId);
  }
  struct Attribute {
    Key input;
    Rc<DxvkBuffer> output;
    std::vector<Com<D3D11Buffer>> sources;
    Com<D3D11VertexShader> shader;
    Com<D3D11InputLayout> layout;
    uint64_t revision = 0;
    uint32_t lastUsed = 0;
  };
  static thread_local std::unordered_map<uint64_t, Attribute> attributes;
  static thread_local uint64_t attributeEpoch = 0;
  static thread_local uint64_t uvHits = 0, normalHits = 0, uvUpdates = 0, normalUpdates = 0;
  static thread_local uint64_t lastUvKey = 0, lastNormalKey = 0;
  static void touchAttribute(uint64_t key, uint32_t frame) {
    const auto i = attributes.find(key);
    if (i != attributes.end()) i->second.lastUsed = frame;
  }
  static bool attributeInput(Key& k, const D3D11ContextState& s, bool normals,
      const std::string& semantic, uint32_t index, uint32_t component, uint32_t first, uint32_t count) {
    auto* vs = s.vs.shader.ptr() ? s.vs.shader->GetCommonShader() : nullptr;
    if (!prepared_terrain::enabled.load(std::memory_order_relaxed) || !ground(vs)
     || semantic != "TEXCOORD" || component != 0 || (normals ? index != 0 : (index != 2 && index != 3))) return false;
    if (!vertices(k, s)) return false;
    k.add(vs); k.add(first); k.add(count); k.add(normals);
    if (normals) {
      float world[16] {};
      if (!named(k, s, "morph", 1)
       || !readNamedConstant(s.vs.constantBuffers[0], vs, "worldMatrix", world, 16)) return false;
      for (uint32_t c = 0; c < 3; ++c) for (uint32_t r = 0; r < 3; ++r) k.add(world[c*4+r]);
    } else if (index == 3 && !named(k, s, "overlayData", 4)) return false;
    return true;
  }
  static void saveAttribute(uint64_t key, Key input, const D3D11ContextState& s,
      const Rc<DxvkBuffer>& output, uint32_t frame) {
    if (attributes.size() >= 4096) attributes.clear();
    Attribute a; a.input = std::move(input); a.output = output; a.lastUsed = frame;
    a.revision = prepared_terrain::revision(output.ptr()); a.shader = s.vs.shader; a.layout = s.ia.inputLayout;
    const auto mask = vertexSlots(s);
    for (uint32_t i = 0; i < 32; ++i) if (mask & (1u << i)) a.sources.push_back(s.ia.vertexBuffers[i].buffer);
    attributes.insert_or_assign(key, std::move(a));
  }
}
