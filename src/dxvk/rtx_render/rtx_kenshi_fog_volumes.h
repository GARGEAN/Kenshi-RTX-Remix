#pragma once

// Per-frame transport for Kenshi's local fog volumes. The usual bridge->renderer transport (an
// RTX_OPTION written with setDeferred) cannot carry an array, so this is a small fixed-size store,
// written by d3d11_rtx.cpp as the draws arrive and snapshotted by rtx_composite.cpp when it fills
// CompositeArgs (both in d3d11.dll). Fixed size: no per-frame allocation, and the cap costs only
// constant-buffer bytes - the shader loops to the live count.

#include <cstdint>
#include <cstring>
#include <mutex>

namespace dxvk {
  namespace kenshi_fog {

    // 32 volumes. Kenshi's block hulls are far larger than the 50000-unit view distance, so most of the
    // world's blocks pass the frustum cull at once (18 were measured publishable in one Swamp frame), and an
    // overflow drops whichever volumes the per-frame back-to-front sort puts last, so fog blinks with camera
    // yaw. fogfeatures.dat holds exactly 26 publishable volumes (25 blocks + 1 sphere), so with duplicates
    // rejected real geometry cannot overflow 32; m_dropped reports a mod that does.
    // Cost: 160 constant-buffer bytes per volume (CompositeArgs 8288 -> 10848 of a 64 KiB uniform buffer);
    // per-pixel cost follows the live count.
    static constexpr uint32_t kMaxFogVolumes = 32u;

    enum VolumeType : uint32_t {
      Type_None = 0u,
      Type_Block = 1u,      // fog_planes_fs  - convex hull of 7 planes
      Type_Sphere = 2u,     // fog_sphere_fs  - analytic sphere with break-up
      Type_Cylinder = 3u,   // fog_beam_fs    - capped cylinder
    };

    struct Volume {
      // Block:    seven world-space planes, dot(n, p) <= w.
      // Sphere:   [0] = (centre.xyz, radius).
      // Cylinder: [0] = (base.xyz, radius), [1] = (axis.xyz, height).
      float planes[7][4] = {};
      float colour[4] = {};      // rgb + a
      float density = 0.0f;
      float edgeBlur = 0.0f;
      float sunLight = 1.0f;     // sunColour.w, the `light` argument of fogValue
      uint32_t type = Type_None;
      // Blocks only. fog_planes_fs adds this to the CAMERA before testing
      // the planes (`camera += worldOffset`), so it must travel with them
      // rather than being folded into the plane constants.
      float worldOffset[3] = {};
    };

    class Store {
    public:
      static Store& get() {
        static Store s_instance;
        return s_instance;
      }

      // The clear is keyed on the frame id rather than an external begin-frame call:
      // D3D11Rtx::ResetCommandListState() runs several times per frame and would wipe the volumes between the
      // fog draws and the composite read.
      void add(const Volume& volume, uint32_t frameId) {
        std::lock_guard<std::mutex> lock { m_lock };
        if (frameId != m_frameId) {
          m_frameId = frameId;
          m_count = 0u;
        }
        // Reject a volume already published in this generation. The key (getCurrentFrameId(), advanced on the
        // CS thread at present) does not change one-for-one with game frames: the app thread can run up to
        // maxFrameLatency frames ahead, so two frames' fog draws can land in one generation, doubling every
        // volume's alpha for a frame (pulsing fog) and burning slots. Moving the snapshot onto the app thread
        // at the injection site would fix the key itself.
        for (uint32_t i = 0; i < m_count; ++i) {
          if (sameVolume(m_volumes[i], volume)) {
            ++m_duplicates;
            return;
          }
        }
        if (m_count >= kMaxFogVolumes) {
          ++m_dropped;
          return;
        }
        m_volumes[m_count++] = volume;
      }

      uint32_t snapshot(Volume* out, uint32_t maxOut) const {
        std::lock_guard<std::mutex> lock { m_lock };
        const uint32_t n = m_count < maxOut ? m_count : maxOut;
        if (out != nullptr && n > 0u) {
          std::memcpy(out, m_volumes, sizeof(Volume) * n);
        }
        return n;
      }

      uint32_t droppedCount() const {
        std::lock_guard<std::mutex> lock { m_lock };
        return m_dropped;
      }

      uint32_t duplicateCount() const {
        std::lock_guard<std::mutex> lock { m_lock };
        return m_duplicates;
      }

    private:
      // What counts as the same volume. sunLight is excluded: it is harvested per draw from constant buffers
      // refreshed at slightly different moments, so two frames' copies of one volume can differ by a hair.
      // Everything else is authored and bit-identical between frames. All seven plane rows and worldOffset
      // are compared regardless of type; unused rows are zero on both sides (default initialisers, and the
      // publisher value-initialises each volume).
      static bool sameVolume(const Volume& a, const Volume& b) {
        if (a.type != b.type || a.density != b.density || a.edgeBlur != b.edgeBlur)
          return false;
        for (uint32_t i = 0; i < 4u; ++i)
          if (a.colour[i] != b.colour[i])
            return false;
        for (uint32_t r = 0; r < 7u; ++r)
          for (uint32_t c = 0; c < 4u; ++c)
            if (a.planes[r][c] != b.planes[r][c])
              return false;
        for (uint32_t i = 0; i < 3u; ++i)
          if (a.worldOffset[i] != b.worldOffset[i])
            return false;
        return true;
      }

      Store() = default;
      Store(const Store&) = delete;
      Store& operator=(const Store&) = delete;

      mutable std::mutex m_lock;
      Volume m_volumes[kMaxFogVolumes] = {};
      uint32_t m_count = 0u;
      uint32_t m_frameId = 0xffffffffu;
      uint32_t m_dropped = 0u;
      uint32_t m_duplicates = 0u;
    };

  }  // namespace kenshi_fog
}  // namespace dxvk
