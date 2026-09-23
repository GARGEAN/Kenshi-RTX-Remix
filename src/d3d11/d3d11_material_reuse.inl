// Included inside namespace dxvk before FillMaterialData. No renderer layout changes.
namespace material_reuse {
  using prepared_terrain::Key;
  struct TextureRegistration {
    XXH64_hash_t hash;
    Rc<DxvkImageView> view;
    uint32_t flags;
  };
  static thread_local std::vector<TextureRegistration>* recording = nullptr;
  static void registerTexture(XXH64_hash_t hash, const Rc<DxvkImageView>& view, uint32_t flags) {
    ImGUI::AddTexture(hash, view, flags);
    if (recording) recording->push_back({ hash, view, flags });
  }
  struct RecordScope {
    std::vector<TextureRegistration>* previous;
    explicit RecordScope(std::vector<TextureRegistration>& out) : previous(recording) { recording = &out; }
    ~RecordScope() { recording = previous; }
  };
  // Exact named reads in FillMaterialData, except wetness/waterHeightRel, which only affect frame
  // publication and are re-read on every hit. Water falls back.
  // Build-progress quantisation, shared by the reuse key, the value stored on the material and the
  // material identity (KenshiMaterialConstants); all three must agree. The game advances
  // constructionState ~8 times a second (~200 distinct values over a typical build). Each step costs
  // a reuse miss and an entry in the 512-slot LRU, so 128 steps balances smoothness against LRU
  // pressure when several buildings are under construction.
  static constexpr uint32_t kKenshiConstructionSteps = 128u;
  static inline float quantizeConstructionState(float progress) {
    return float(int(std::lround(std::clamp(progress, 0.0f, 1.0f)
      * float(kKenshiConstructionSteps)))) / float(kKenshiConstructionSteps);
  }
  // Every per-draw constant FillMaterialData recovers must be listed here: the whole function is gated
  // behind a cache keyed on exactly these names, so a missing constant freezes at its first-draw value
  // and is shared by every draw with the same shader, textures and constants.
  // `Layout::constants` below is sized from this array and must match.
  static constexpr const char* names[] = {
    "color1", "color2", "colour1", "colour2", "glossMult", "dustColour", "dustAmount",
    "diffuseChannel", "alphaChannel", "color", "skintone", "hairColor", "hairMult",
    "hairAlpha", "beardAlpha", "muscleBlend"
  };
  static D3D11OpacityCutoutProfile cutout(const D3D11CommonShader* ps) {
    if (const auto* p = ps->GetOpacityCutoutProfile()) return *p;
    const auto& n = ps->GetName();
    if (n == "FS_027a2542c3659325cbd7f2e8d581ae024ecd8826") return { true, 0u, 0u, 0u };
    if (n == "FS_a0ba17effa11409f912efbdbd90cbb7100cddae1") return { true, 1u, 0u, 0u };
    if (n == "FS_b74b3b7abd9b6e32d4d3dca19b1bfd979c2e9de5") return { true, 1u, 0u, 1u };
    if (n == "FS_7794040e4aa6f4c9873cec57c47517201591a4eb") return { true, 0u, 0u, 3u };
    return {};
  }
  static void constantBytes(Key& k, const D3D11ConstantBufferBinding& cb, size_t offset, size_t bytes) {
    const auto mapped = cb.buffer.ptr() ? cb.buffer->GetMappedSlice() : DxvkBufferSliceHandle {};
    const size_t size = cb.buffer.ptr() ? cb.buffer->Desc()->ByteWidth : 0;
    const size_t base = size_t(cb.constantOffset) * 16;
    const size_t end = cb.constantCount ? std::min(size, base + size_t(cb.constantCount) * 16) : size;
    const size_t available = mapped.mapPtr && base <= end && offset <= end - base
      ? std::min(bytes, end - base - offset) : 0;
    // Include partial readability: color.xyz can be readable when color.w is not.
    k.add(available);
    if (available) k.data(static_cast<const uint8_t*>(mapped.mapPtr) + base + offset, available);
  }
  // Shader metadata is immutable. Retain its owner, never mapped constant-buffer values.
  struct Layout {
    Com<D3D11PixelShader> owner;
    std::array<D3D11NamedConstantProfile::Location,
               sizeof(names) / sizeof(names[0])> constants {};
    D3D11OpacityCutoutProfile alpha;
    // Build progress. Not in `names`: that loop hashes raw bytes and progress ramps continuously, so it
    // would mint a cache entry every frame. Quantised in `input` below instead.
    D3D11NamedConstantProfile::Location construction {};
    std::vector<uint32_t> slots;
    uint64_t lastUsed = 0;
  };
  static thread_local std::unordered_map<const D3D11CommonShader*, Layout> layouts;
  static thread_local uint64_t layoutSerial = 0;
  static const Layout& layout(const D3D11ContextState& s) {
    const auto* ps = s.ps.shader->GetCommonShader();
    auto found = layouts.find(ps);
    if (found != layouts.end()) {
      found->second.lastUsed = ++layoutSerial;
      return found->second;
    }
    Layout value; value.owner = s.ps.shader; value.lastUsed = ++layoutSerial;
    const auto& profile = getNamedConstantProfile(ps->GetReflection());
    for (size_t i = 0; i < value.constants.size(); ++i)
      if (const auto* v = profile.find(names[i])) value.constants[i] = *v;
    if (const auto* v = profile.find("constructionState")) value.construction = *v;
    value.alpha = cutout(ps);
    for (uint32_t i = 0; i < s.ps.shaderResources.views.size(); ++i)
      if (ps->SamplesResourceSlot(i) || ps->DeclaresTextureBinding(i)
       || (value.alpha.valid && i == value.alpha.alphaResourceSlot)) value.slots.push_back(i);
    if (layouts.size() >= 128) {
      auto oldest = layouts.begin();
      for (auto i = layouts.begin(); i != layouts.end(); ++i)
        if (i->second.lastUsed < oldest->second.lastUsed) oldest = i;
      layouts.erase(oldest);
    }
    return layouts.emplace(ps, std::move(value)).first->second;
  }
  static bool input(Key& k, const D3D11ContextState& s) {
    const auto* ps = s.ps.shader.ptr() ? s.ps.shader->GetCommonShader() : nullptr;
    const auto* vs = s.vs.shader.ptr() ? s.vs.shader->GetCommonShader() : nullptr;
    if (!ps || !vs || !ps->GetReflection() || !ps->GetReflection()->isValid()
     || !ps->HasCompleteSampledResourceProfile() || kenshiWaterColourSlot(ps) != UINT32_MAX) return false;
    k.bytes.reserve(1024);
    k.add(ps); k.add(vs);
    const auto& metadata = layout(s);
    for (const auto& v : metadata.constants) {
      k.add(v.used);
      if (v.used) constantBytes(k, s.ps.constantBuffers[0], v.offset, std::min(v.size, 16u));
    }
    // Build progress. Without it a building under construction is a permanent cache hit: the entry keeps
    // the progress captured at placement, and other buildings of the same type inherit it. Quantised to
    // one entry per visible step; 0xFFFFFFFF means "no progress constant", distinct from step 0.
    {
      uint32_t constructionStep = 0xFFFFFFFFu;
      if (metadata.construction.used && metadata.construction.size >= sizeof(float)) {
        const auto& cb = s.ps.constantBuffers[0];
        const auto mapped = cb.buffer.ptr() ? cb.buffer->GetMappedSlice() : DxvkBufferSliceHandle {};
        const size_t size = cb.buffer.ptr() ? cb.buffer->Desc()->ByteWidth : 0;
        const size_t base = size_t(cb.constantOffset) * 16;
        const size_t end = cb.constantCount ? std::min(size, base + size_t(cb.constantCount) * 16) : size;
        const size_t offset = base + size_t(metadata.construction.offset);
        if (mapped.mapPtr && base <= end && offset + sizeof(float) <= end) {
          float progress = 0.0f;
          std::memcpy(&progress, static_cast<const uint8_t*>(mapped.mapPtr) + offset, sizeof(float));
          if (std::isfinite(progress))
            constructionStep = uint32_t(std::lround(
              std::clamp(progress, 0.0f, 1.0f) * float(kKenshiConstructionSteps)));
        }
      }
      k.add(constructionStep);
    }

    const auto& alpha = metadata.alpha;
    if (alpha.valid && alpha.thresholdConstantBufferSlot < s.ps.constantBuffers.size())
      constantBytes(k, s.ps.constantBuffers[alpha.thresholdConstantBufferSlot],
        size_t(alpha.thresholdConstantRegister) * 16 + size_t(alpha.thresholdConstantComponent) * 4, 4);
    for (uint32_t i : metadata.slots) {
      auto* srv = s.ps.shaderResources.views[i].ptr(); k.add(i); k.add(srv);
      if (srv) {
        const auto view = srv->GetImageView();
        if (!view.ptr() || !view->image().ptr()) return false;
        k.add(view->cookie()); k.add(view->image()->getHash());
      }
    }
    for (auto* sampler : s.ps.samplers) k.add(sampler);
    for (const auto& rt : s.om.renderTargetViews) k.add(rt.ptr());
    k.add(s.om.cbState); k.add(s.om.blendFactor);
    return true;
  }
  struct Entry {
    Key key;
    LegacyMaterialData material;
    std::vector<TextureRegistration> registrations;
    Com<D3D11VertexShader> vs;
    Com<D3D11PixelShader> ps;
    Com<D3D11BlendState> blend;
    std::vector<Com<D3D11ShaderResourceView>> views;
    std::vector<Com<D3D11SamplerState>> samplers;
    std::vector<Com<D3D11RenderTargetView>> targets;
    uint32_t lastUsed = 0;
    uint64_t lastAccess = 0;
    void retain(const D3D11ContextState& s) {
      vs = s.vs.shader; ps = s.ps.shader; blend = s.om.cbState;
      for (uint32_t i : layout(s).slots) {
        const auto& v = s.ps.shaderResources.views[i];
        if (v.ptr()) views.emplace_back(v.ptr());
      }
      for (auto* v : s.ps.samplers) if (v) samplers.emplace_back(v);
      for (const auto& v : s.om.renderTargetViews) if (v.ptr()) targets.emplace_back(v.ptr());
    }
  };
  struct Cache {
    std::unordered_map<uint64_t, Entry> entries;
    const void* owner = nullptr;
    uint64_t epoch = 0;
    uint32_t frame = 0, frames = 0, remaining = 0;
    uint64_t considered = 0, hits = 0, misses = 0, fallback = 0, verified = 0, mismatches = 0;
    uint64_t accessSerial = 0, capacityEvictions = 0;
    bool enabled = true;
  };
  static thread_local Cache cache;
  static bool sameOutput(const LegacyMaterialData& a, const LegacyMaterialData& b);
  static void publish(const D3D11ContextState& s, const Entry& e, uint32_t frame) {
    probeMaterialInterior(s);
    for (const auto& t : e.registrations) ImGUI::AddTexture(t.hash, t.view, t.flags);
    const auto* ps = s.ps.shader->GetCommonShader();
    const auto& dust = e.material.kenshiDustColour;
    float amount[3] {};
    if (dust.w > 0 && readNamedConstant(s.ps.constantBuffers[0], ps, "dustAmount", amount, 3)) {
      SceneManager::registerKenshiDust(Vector3(dust.x, dust.y, dust.z), amount[0], amount[1], amount[2]);
      if (e.material.kenshiDustNoiseTexture.isValid()) ++s_kenshiDustDraws;
    }
    terrain_dependencies::globals(s, frame);
  }
  template<class Fill>
  static void fill(const void* owner, const D3D11ContextState& state, uint32_t frame,
                   LegacyMaterialData& material, Fill&& original) {
    auto& c = cache;
    const uint64_t epoch = prepared_terrain::epoch.load(std::memory_order_relaxed);
    if (c.owner != owner || c.epoch != epoch || frame < c.frame) {
      c.entries.clear(); c.owner = owner; c.epoch = epoch; c.accessSerial = 0;
      layouts.clear(); layoutSerial = 0;
    }
    if (c.frame != frame) {
      for (auto i = c.entries.begin(); i != c.entries.end(); )
        if (frame - i->second.lastUsed > 600) i = c.entries.erase(i); else ++i;
      c.frame = frame;
    }
    ++c.considered;
    Key key;
    if (!c.enabled || !input(key, state)) { ++c.fallback; original(material); return; }
    const uint64_t hash = XXH3_64bits(key.bytes.data(), key.bytes.size());
    auto it = c.entries.find(hash);
    if (it != c.entries.end() && it->second.key == key) {
      auto& e = it->second; e.lastUsed = frame; e.lastAccess = ++c.accessSerial; ++c.hits;
      if (kenshi_telemetry::enabled() && c.remaining) {
        --c.remaining; ++c.verified;
        std::vector<TextureRegistration> registrations;
        { RecordScope record(registrations); original(material); }
        bool equal = sameOutput(material, e.material) && registrations.size() == e.registrations.size();
        for (size_t i = 0; equal && i < registrations.size(); ++i)
          equal = registrations[i].hash == e.registrations[i].hash
            && registrations[i].view == e.registrations[i].view && registrations[i].flags == e.registrations[i].flags;
        if (!equal) {
          ++c.mismatches; c.enabled = false; c.remaining = 0;
          Logger::err("[MaterialReuse V708] verification mismatch; reuse disabled; original material retained");
        }
        if (!c.remaining) KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialReuse V708] verification complete checks=",
          c.verified, " mismatches=", c.mismatches, " enabled=", c.enabled));
      } else { material = e.material; publish(state, e, frame); }
      return;
    }
    ++c.misses;
    Entry e; e.key = std::move(key); e.lastUsed = frame; e.lastAccess = ++c.accessSerial;
    { RecordScope record(e.registrations); original(material); }
    e.material = material; e.retain(state);
    if (it == c.entries.end() && c.entries.size() >= 512) {
      // Recency within a frame matters too: don't displace a just-used draw.
      auto oldest = c.entries.begin();
      for (auto i = c.entries.begin(); i != c.entries.end(); ++i)
        if (i->second.lastAccess < oldest->second.lastAccess) oldest = i;
      c.entries.erase(oldest); ++c.capacityEvictions;
    }
    c.entries.insert_or_assign(hash, std::move(e));
  }
  static void frame() {
    auto& c = cache;
    if (++c.frames < 600) return;
    if (terrain_profile::diagnosticsEnabled()) KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialReuse V708] frames=", c.frames, " enabled=", c.enabled,
      " considered=", c.considered, " hits=", c.hits, " misses=", c.misses, " fallback=", c.fallback,
      " entries=", c.entries.size(), " capacityEvictions=", c.capacityEvictions,
      " verified=", c.verified, " mismatches=", c.mismatches,
      " verifyRemaining=", c.remaining));
    c.frames = 0; c.considered = c.hits = c.misses = c.fallback = c.capacityEvictions = 0;
  }
}
