// V715: included inside dxvk. Cache only Legacy -> Opaque conversion; all
// replacement selection, character extensions and per-instance blood stay live.
namespace opaque_preparation {
  using prepared_terrain::Key;
  static void vectorKey(Key& k, const Vector3& v) { k.add(v.x); k.add(v.y); k.add(v.z); }
  static Key inputKey(const LegacyMaterialData& m) {
    Key k; k.bytes.reserve(256);
    k.add(LegacyMaterialDefaults::anisotropy());
    k.add(LegacyMaterialDefaults::emissiveIntensity());
    vectorKey(k, LegacyMaterialDefaults::albedoConstant());
    k.add(LegacyMaterialDefaults::opacityConstant());
    k.add(LegacyMaterialDefaults::roughnessConstant());
    k.add(LegacyMaterialDefaults::metallicConstant());
    vectorKey(k, LegacyMaterialDefaults::emissiveColorConstant());
    k.add(LegacyMaterialDefaults::enableEmissive());
    k.add(LegacyMaterialDefaults::enableThinFilm());
    k.add(LegacyMaterialDefaults::alphaIsThinFilmThickness());
    k.add(LegacyMaterialDefaults::thinFilmThicknessConstant());
    k.add(LegacyMaterialDefaults::useAlbedoTextureIfPresent());
    k.add(LegacyMaterialDefaults::ignoreAlphaChannel());
    k.add(KenshiOptions::kenshiGameMetalness());
    k.add(KenshiOptions::kenshiObjectGlossMultiplier());
    k.add(lookupHash(RtxOptions::ignoreAlphaOnTextures(), m.getHash()));
    for (const TextureRef* t : { &m.getColorTexture(), &m.getColorTexture2(),
                               &m.kenshiNormalTexture, &m.kenshiMetalTexture }) {
      k.add(t->isValid()); k.add(t->getImageView()); k.add(t->getImageHash());
      k.add(t->getManagedTexture().ptr());
      if (t->isValid()) k.add(t->getUniqueKey());
    }
    k.add(m.getSampler().ptr());
    k.add(m.hasConstantAlbedo); vectorKey(k, Vector3(m.constantAlbedo.x, m.constantAlbedo.y, m.constantAlbedo.z));
    k.add(m.kenshiGlossMult); k.add(m.dx11Material.Power);
    return k;
  }
  static bool sameOutput(const OpaqueMaterialData& a, const OpaqueMaterialData& b) {
    bool equal = a.getHash() == b.getHash() && a.getSamplerOverride() == b.getSamplerOverride()
      && a.getIgnoreAlphaChannel() == b.getIgnoreAlphaChannel();
    // Compare every public opaque field; generated from the material parameter lists.
    equal = equal && a.getAnisotropyConstant() == b.getAnisotropyConstant();
    equal = equal && a.getEmissiveIntensity() == b.getEmissiveIntensity();
    equal = equal && a.getAlbedoConstant() == b.getAlbedoConstant();
    equal = equal && a.getOpacityConstant() == b.getOpacityConstant();
    equal = equal && a.getRoughnessConstant() == b.getRoughnessConstant();
    equal = equal && a.getMetallicConstant() == b.getMetallicConstant();
    equal = equal && a.getEmissiveColorConstant() == b.getEmissiveColorConstant();
    equal = equal && a.getEnableEmission() == b.getEnableEmission();
    equal = equal && a.getSpriteSheetRows() == b.getSpriteSheetRows();
    equal = equal && a.getSpriteSheetCols() == b.getSpriteSheetCols();
    equal = equal && a.getSpriteSheetFPS() == b.getSpriteSheetFPS();
    equal = equal && a.getEnableThinFilm() == b.getEnableThinFilm();
    equal = equal && a.getAlphaIsThinFilmThickness() == b.getAlphaIsThinFilmThickness();
    equal = equal && a.getThinFilmThicknessConstant() == b.getThinFilmThicknessConstant();
    equal = equal && a.getUseLegacyAlphaState() == b.getUseLegacyAlphaState();
    equal = equal && a.getBlendEnabled() == b.getBlendEnabled();
    equal = equal && a.getBlendType() == b.getBlendType();
    equal = equal && a.getInvertedBlend() == b.getInvertedBlend();
    equal = equal && a.getAlphaTestType() == b.getAlphaTestType();
    equal = equal && a.getAlphaTestReferenceValue() == b.getAlphaTestReferenceValue();
    equal = equal && a.getDisplaceIn() == b.getDisplaceIn();
    equal = equal && a.getDisplaceOut() == b.getDisplaceOut();
    equal = equal && a.getSubsurfaceTransmittanceColor() == b.getSubsurfaceTransmittanceColor();
    equal = equal && a.getSubsurfaceMeasurementDistance() == b.getSubsurfaceMeasurementDistance();
    equal = equal && a.getSubsurfaceSingleScatteringAlbedo() == b.getSubsurfaceSingleScatteringAlbedo();
    equal = equal && a.getSubsurfaceVolumetricAnisotropy() == b.getSubsurfaceVolumetricAnisotropy();
    equal = equal && a.getSubsurfaceDiffusionProfile() == b.getSubsurfaceDiffusionProfile();
    equal = equal && a.getSubsurfaceRadius() == b.getSubsurfaceRadius();
    equal = equal && a.getSubsurfaceRadiusScale() == b.getSubsurfaceRadiusScale();
    equal = equal && a.getSubsurfaceMaxSampleRadius() == b.getSubsurfaceMaxSampleRadius();
    equal = equal && a.getFilterMode() == b.getFilterMode();
    equal = equal && a.getWrapModeU() == b.getWrapModeU();
    equal = equal && a.getWrapModeV() == b.getWrapModeV();
    equal = equal && a.getAlbedoOpacityTexture().isValid() == b.getAlbedoOpacityTexture().isValid()
      && a.getAlbedoOpacityTexture().getImageView() == b.getAlbedoOpacityTexture().getImageView()
      && a.getAlbedoOpacityTexture().getManagedTexture() == b.getAlbedoOpacityTexture().getManagedTexture()
      && a.getAlbedoOpacityTexture().getImageHash() == b.getAlbedoOpacityTexture().getImageHash();
    equal = equal && a.getNormalTexture().isValid() == b.getNormalTexture().isValid()
      && a.getNormalTexture().getImageView() == b.getNormalTexture().getImageView()
      && a.getNormalTexture().getManagedTexture() == b.getNormalTexture().getManagedTexture()
      && a.getNormalTexture().getImageHash() == b.getNormalTexture().getImageHash();
    equal = equal && a.getTangentTexture().isValid() == b.getTangentTexture().isValid()
      && a.getTangentTexture().getImageView() == b.getTangentTexture().getImageView()
      && a.getTangentTexture().getManagedTexture() == b.getTangentTexture().getManagedTexture()
      && a.getTangentTexture().getImageHash() == b.getTangentTexture().getImageHash();
    equal = equal && a.getHeightTexture().isValid() == b.getHeightTexture().isValid()
      && a.getHeightTexture().getImageView() == b.getHeightTexture().getImageView()
      && a.getHeightTexture().getManagedTexture() == b.getHeightTexture().getManagedTexture()
      && a.getHeightTexture().getImageHash() == b.getHeightTexture().getImageHash();
    equal = equal && a.getRoughnessTexture().isValid() == b.getRoughnessTexture().isValid()
      && a.getRoughnessTexture().getImageView() == b.getRoughnessTexture().getImageView()
      && a.getRoughnessTexture().getManagedTexture() == b.getRoughnessTexture().getManagedTexture()
      && a.getRoughnessTexture().getImageHash() == b.getRoughnessTexture().getImageHash();
    equal = equal && a.getMetallicTexture().isValid() == b.getMetallicTexture().isValid()
      && a.getMetallicTexture().getImageView() == b.getMetallicTexture().getImageView()
      && a.getMetallicTexture().getManagedTexture() == b.getMetallicTexture().getManagedTexture()
      && a.getMetallicTexture().getImageHash() == b.getMetallicTexture().getImageHash();
    equal = equal && a.getEmissiveColorTexture().isValid() == b.getEmissiveColorTexture().isValid()
      && a.getEmissiveColorTexture().getImageView() == b.getEmissiveColorTexture().getImageView()
      && a.getEmissiveColorTexture().getManagedTexture() == b.getEmissiveColorTexture().getManagedTexture()
      && a.getEmissiveColorTexture().getImageHash() == b.getEmissiveColorTexture().getImageHash();
    equal = equal && a.getSubsurfaceTransmittanceTexture().isValid() == b.getSubsurfaceTransmittanceTexture().isValid()
      && a.getSubsurfaceTransmittanceTexture().getImageView() == b.getSubsurfaceTransmittanceTexture().getImageView()
      && a.getSubsurfaceTransmittanceTexture().getManagedTexture() == b.getSubsurfaceTransmittanceTexture().getManagedTexture()
      && a.getSubsurfaceTransmittanceTexture().getImageHash() == b.getSubsurfaceTransmittanceTexture().getImageHash();
    equal = equal && a.getSubsurfaceThicknessTexture().isValid() == b.getSubsurfaceThicknessTexture().isValid()
      && a.getSubsurfaceThicknessTexture().getImageView() == b.getSubsurfaceThicknessTexture().getImageView()
      && a.getSubsurfaceThicknessTexture().getManagedTexture() == b.getSubsurfaceThicknessTexture().getManagedTexture()
      && a.getSubsurfaceThicknessTexture().getImageHash() == b.getSubsurfaceThicknessTexture().getImageHash();
    equal = equal && a.getSubsurfaceSingleScatteringAlbedoTexture().isValid() == b.getSubsurfaceSingleScatteringAlbedoTexture().isValid()
      && a.getSubsurfaceSingleScatteringAlbedoTexture().getImageView() == b.getSubsurfaceSingleScatteringAlbedoTexture().getImageView()
      && a.getSubsurfaceSingleScatteringAlbedoTexture().getManagedTexture() == b.getSubsurfaceSingleScatteringAlbedoTexture().getManagedTexture()
      && a.getSubsurfaceSingleScatteringAlbedoTexture().getImageHash() == b.getSubsurfaceSingleScatteringAlbedoTexture().getImageHash();
    equal = equal && a.getSubsurfaceRadiusTexture().isValid() == b.getSubsurfaceRadiusTexture().isValid()
      && a.getSubsurfaceRadiusTexture().getImageView() == b.getSubsurfaceRadiusTexture().getImageView()
      && a.getSubsurfaceRadiusTexture().getManagedTexture() == b.getSubsurfaceRadiusTexture().getManagedTexture()
      && a.getSubsurfaceRadiusTexture().getImageHash() == b.getSubsurfaceRadiusTexture().getImageHash();
    equal = equal && a.getSecondaryTexture().isValid() == b.getSecondaryTexture().isValid()
      && a.getSecondaryTexture().getImageView() == b.getSecondaryTexture().getImageView()
      && a.getSecondaryTexture().getManagedTexture() == b.getSecondaryTexture().getManagedTexture()
      && a.getSecondaryTexture().getImageHash() == b.getSecondaryTexture().getImageHash();
    return equal;
  }
  struct Entry { Key key; OpaqueMaterialData material; uint32_t lastUsed = 0, sweepSlot = 0; };
  struct Cache {
    uint64_t epoch = 0; bool enabled = true;
    std::unordered_map<uint64_t, Entry> entries;
    MaintenanceKeys sweep;
    uint64_t passes = 0, scanned = 0, released = 0, totalUs = 0;
    uint32_t maxUs = 0;
    void clear() { entries.clear(); sweep.clear(); }
    void erase(uint64_t key) {
      const auto it = entries.find(key);
      const uint32_t slot = it->second.sweepSlot;
      const uint64_t moved = sweep.remove(slot);
      entries.erase(it);
      if (slot < sweep.keys.size()) entries.at(moved).sweepSlot = slot;
    }
  };
  static std::mutex mutex;
  static std::unordered_map<const void*, Cache> caches;
  static void forget(const void* owner) { std::lock_guard<std::mutex> lock(mutex); caches.erase(owner); }
  static OpaqueMaterialData prepare(const void* owner, const LegacyMaterialData& m, uint32_t frame) {
    terrain_profile::Scope scope(terrain_profile::Stage::OpaquePreparation);
    std::lock_guard<std::mutex> lock(mutex);
    auto& c = caches[owner];
    const uint64_t epoch = prepared_terrain::epoch.load(std::memory_order_relaxed);
    if (c.epoch != epoch) { c.clear(); c.epoch = epoch; }
    const Key key = inputKey(m);
    const uint64_t h = XXH3_64bits(key.bytes.data(), key.bytes.size());
    auto it = c.entries.find(h);
    if (c.enabled && it != c.entries.end() && it->second.key == key) {
      it->second.lastUsed = frame;
      terrain_profile::count(terrain_profile::OpaquePreparationHit);
      if (kenshi_telemetry::enabled() && terrain_profile::cityVerifyRemaining) {
        --terrain_profile::cityVerifyRemaining; ++terrain_profile::cityVerified;
        const auto reference = m.as<OpaqueMaterialData>();
        if (!sameOutput(reference, it->second.material)) {
          ++terrain_profile::cityMismatches; c.enabled = false; c.clear();
          Logger::err("[CityPreparation V715] opaque mismatch; original conversion restored");
          return reference;
        }
      }
      return it->second.material;
    }
    terrain_profile::count(terrain_profile::OpaquePreparationMiss);
    auto material = m.as<OpaqueMaterialData>();
    if (c.enabled) {
      // Preserve the numeric maintenance slot when replacing a hash collision.
      auto existing = c.entries.find(h);
      if (existing != c.entries.end()) {
        existing->second = Entry { key, material, frame, existing->second.sweepSlot };
      } else {
        if (c.entries.size() >= 1024) c.erase(c.entries.begin()->first);
        const uint32_t slot = c.sweep.add(h);
        c.entries.emplace(h, Entry { key, material, frame, slot });
      }
    }
    return material;
  }

  static void collect(const void* owner, uint32_t frame) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::microseconds(100);
    std::lock_guard<std::mutex> lock(mutex);
    auto found = caches.find(owner);
    if (found == caches.end()) return;
    auto& c = found->second;
    const uint32_t limit = std::min<uint32_t>(64u, uint32_t(c.entries.size()));
    uint32_t scanned = 0, released = 0;
    while (scanned < limit && released < 8u && !c.sweep.keys.empty()) {
      if (scanned && scanned % 8u == 0u && std::chrono::steady_clock::now() >= deadline) break;
      const uint64_t key = c.sweep.keys[c.sweep.next()];
      const auto& entry = c.entries.at(key);
      ++scanned;
      // Only release this cache's copy. Live materials/commands keep their own
      // references; a later conversion miss rebuilds before registering indices.
      if (frame >= entry.lastUsed && frame - entry.lastUsed > 600u) {
        c.erase(key);
        ++released;
      }
    }
    if (!kenshi_telemetry::enabled()) return;
    const uint32_t us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count());
    ++c.passes; c.scanned += scanned; c.released += released; c.totalUs += us;
    c.maxUs = std::max(c.maxUs, us);
    if (frame % 600u == 15u)
      KENSHI_DIAGNOSTIC_INFO(str::format("[CacheMaintenance V780] frame=", frame,
        " phase=opaque passes=", c.passes, " scanned=", c.scanned, " released=", c.released,
        " avgUs=", c.totalUs / c.passes, " maxUs=", c.maxUs,
        " entries=", c.entries.size(), " cursor=", c.sweep.cursor,
        " scanDeferred=", c.entries.size() > scanned ? c.entries.size() - scanned : 0u));
  }

  static void reportTextureOwners(const void* owner, uint32_t frame) {
    if (!kenshi_telemetry::enabled()) return;
    const auto start = std::chrono::steady_clock::now();
    TextureOwnerCensus census;
    uint32_t entries = 0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto cache = caches.find(owner);
      if (cache == caches.end()) return;
      // The production cache is bounded to 1024 entries; 12 texture fields each.
      for (const auto& item : cache->second.entries) {
        if (entries == 1024u) break;
        ++entries;
        const auto& m = item.second.material;
        const bool known = frame >= item.second.lastUsed;
        const bool recent = known && frame - item.second.lastUsed <= 600u;
        for (const TextureRef* t : { &m.getAlbedoOpacityTexture(), &m.getNormalTexture(),
          &m.getTangentTexture(), &m.getHeightTexture(), &m.getRoughnessTexture(),
          &m.getMetallicTexture(), &m.getEmissiveColorTexture(), &m.getSubsurfaceTransmittanceTexture(),
          &m.getSubsurfaceThicknessTexture(), &m.getSubsurfaceSingleScatteringAlbedoTexture(),
          &m.getSubsurfaceRadiusTexture(), &m.getSecondaryTexture() }) {
          if (!t->isValid() || t->getManagedTexture() != nullptr) continue;
          const auto* view = t->getImageView();
          if (view) census.add(view->image().ptr(), view->image()->memSize(), 4u, recent, known);
        }
      }
    }
    const auto row = census.summarize()[4];
    const auto sampleUs = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count();
    KENSHI_DIAGNOSTIC_INFO(str::format("[TextureOwners V779] frame=", frame, " source=opaque mask=4 entries=", entries,
      " images=", row.images, " backingKiB=", row.bytes >> 10, " cold600KiB=", row.coldBytes >> 10,
      " unknownAgeKiB=", row.unknownBytes >> 10, " sampleUs=", sampleUs));
  }
}
