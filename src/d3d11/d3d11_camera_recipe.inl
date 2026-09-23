// V715: a recipe never stores a camera or object transform. It only avoids
// generic projection discovery when fresh draw matrices prove the already
// established CURRENT-frame OGRE camera. Original OGRE decomposition still runs.
namespace camera_recipe {
  struct Key {
    // The current 5-stage schema is 1476 bytes. Keep exact serialized bytes,
    // including float bit patterns, without per-draw allocation. Overflow uses
    // the original vector path, so a future binding-schema expansion stays safe.
    std::array<uint8_t, 2048> fixedBytes {};
    std::vector<uint8_t> legacyBytes;
    size_t used = 0;
    bool fixed = true;
    const uint8_t* data() const { return fixed ? fixedBytes.data() : legacyBytes.data(); }
    size_t size() const { return fixed ? used : legacyBytes.size(); }
    template<typename T> void add(const T& value) {
      if (fixed && sizeof(T) <= fixedBytes.size() - used) {
        std::memcpy(fixedBytes.data() + used, &value, sizeof(T)); used += sizeof(T);
      } else {
        if (fixed) { legacyBytes.assign(fixedBytes.data(), fixedBytes.data() + used); fixed = false; }
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        legacyBytes.insert(legacyBytes.end(), p, p + sizeof(T));
      }
    }
    bool operator==(const Key& other) const {
      return size() == other.size() && (size() == 0 || std::memcmp(data(), other.data(), size()) == 0);
    }
  };
  struct Recipe { Key shape; KenshiOgreB0Layout layout; bool columnMajor; bool worldFirst; };
  static std::unordered_map<uint64_t, Recipe> entries;
  static const void* owner = nullptr;
  static uint64_t epoch = 0;
  static bool enabled = true, retrying = false;
  static Key shape(const D3D11ContextState& s, bool fixed = city_cpu::enabled() && city_cpu::keyValid, bool verify = true) {
    terrain_profile::Scope scope(terrain_profile::Stage::CameraKey);
    Key k; k.fixed = fixed;
    if (!fixed) k.legacyBytes.reserve(1024);
    const D3D11CommonShader* shaders[] = {
      s.vs.shader.ptr() ? s.vs.shader->GetCommonShader() : nullptr,
      s.hs.shader.ptr() ? s.hs.shader->GetCommonShader() : nullptr,
      s.gs.shader.ptr() ? s.gs.shader->GetCommonShader() : nullptr,
      s.ds.shader.ptr() ? s.ds.shader->GetCommonShader() : nullptr,
      s.ps.shader.ptr() ? s.ps.shader->GetCommonShader() : nullptr };
    const D3D11ConstantBufferBindings* stages[] = { &s.vs.constantBuffers, &s.hs.constantBuffers,
      &s.gs.constantBuffers, &s.ds.constantBuffers, &s.ps.constantBuffers };
    for (uint32_t stage = 0; stage < 5; ++stage) {
      k.add(shaders[stage] ? shaders[stage]->GetBytecodeHash() : 0ull);
      for (const auto& cb : *stages[stage]) {
        k.add(cb.buffer.ptr()); k.add(cb.constantOffset); k.add(cb.constantCount);
        k.add(cb.buffer.ptr() ? cb.buffer->Desc()->ByteWidth : 0u);
      }
    }
    k.add(s.rs.numViewports);
    if (s.rs.numViewports == 1) k.add(s.rs.viewports[0]);
    k.add(s.om.renderTargetViews[0].ptr());
    if (kenshi_telemetry::enabled() && verify && city_cpu::keyRemaining) {
      --city_cpu::keyRemaining; ++city_cpu::keyVerified;
      const Key reference = shape(s, !fixed, false);
      if (!(k == reference)) {
        ++city_cpu::keyMismatches; city_cpu::keyValid = false;
        Logger::err("[CityCPU V716] camera key mismatch; original vector path retained");
        if (fixed) return reference;
      }
    }
    return k;
  }
  static bool provesCurrentCamera(const Recipe& r, const D3D11ContextState& s,
                                  const Matrix4& reference) {
    const auto& cb = s.vs.constantBuffers[0];
    if (!cb.buffer.ptr()) return false;
    const auto slice = cb.buffer->GetMappedSlice();
    if (!slice.mapPtr) return false;
    const size_t size = cb.buffer->Desc()->ByteWidth, base = size_t(cb.constantOffset) * 16;
    if (base >= size) return false;
    const size_t end = std::min(base + 8192u, cb.constantCount
      ? std::min(size, base + size_t(cb.constantCount) * 16) : size);
    auto readable = [&](size_t offset, size_t bytes) {
      return offset <= end - base && bytes <= end - base - offset;
    };
    if (!readable(r.layout.worldViewProjOffset, 64)
      || (r.layout.worldMatrixOffset != SIZE_MAX && !readable(r.layout.worldMatrixOffset, 64))
      || (r.layout.worldOffsetVecOffset != SIZE_MAX && !readable(r.layout.worldOffsetVecOffset, 12))) return false;
    const auto* ptr = static_cast<const uint8_t*>(slice.mapPtr);
    auto read = [&](size_t offset) {
      Matrix4 m = readCbMatrix(ptr, base + offset, size);
      return r.columnMajor ? transpose(m) : m;
    };
    const Matrix4 wvp = read(r.layout.worldViewProjOffset);
    Matrix4 world = r.layout.worldMatrixOffset == SIZE_MAX ? Matrix4() : read(r.layout.worldMatrixOffset);
    if (r.layout.worldOffsetVecOffset != SIZE_MAX) {
      float xyz[3]; std::memcpy(xyz, ptr + base + r.layout.worldOffsetVecOffset, sizeof(xyz));
      for (uint32_t i = 0; i < 3; ++i) world[3][i] += xyz[i];
    }
    if (!isFiniteMatrix(wvp) || !isAffineMatrix(world)) return false;
    const Matrix4 inv = inverseAffine(world);
    Matrix4 combined = r.worldFirst ? inv * wvp : wvp * inv;
    if (!isFiniteMatrix(combined)) return false;
    const float length = std::sqrt(combined[0][3]*combined[0][3]
      + combined[1][3]*combined[1][3] + combined[2][3]*combined[2][3]);
    if (!std::isfinite(length) || length < 1.e-5f) return false;
    combined = combined * (1.0f / length);
    // Tighter than existing recomposition tolerance; invalid proofs use the full path.
    for (uint32_t col = 0; col < 4; ++col)
      for (uint32_t row = 0; row < 4; ++row)
        if (!std::isfinite(reference[col][row]) || std::abs(combined[col][row] - reference[col][row])
          > 0.0001f * std::max(1.0f, std::abs(reference[col][row]))) return false;
    return true;
  }
}
