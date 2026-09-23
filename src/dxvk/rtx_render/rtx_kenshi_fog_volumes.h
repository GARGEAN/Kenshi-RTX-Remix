#pragma once

// DX11_V505_KENSHI_FOG_VOLUMES step 2: per-frame transport for Kenshi's local
// fog volumes.
//
// The established bridge->renderer transport in this fork is an RTX_OPTION
// written with setDeferred (see kenshiAmbientTint, kenshiFogColour). That cannot
// carry an ARRAY, and these are per-draw: one volume per draw, several per frame.
// So this is a small fixed-size store instead, written by d3d11_rtx.cpp as the
// draws arrive and snapshotted by rtx_composite.cpp when it fills CompositeArgs.
// Both live in d3d11.dll, so this is an ordinary in-process singleton.
//
// Fixed size on purpose: no per-frame allocation, and the cap itself costs only
// constant-buffer bytes - the shader loops to the live count, never to the cap.

#include <cstdint>
#include <cstring>
#include <mutex>

namespace dxvk {
  namespace kenshi_fog {

    // DX11_V754: 32, raised from 16, with identity dedup in add() below.
    //
    // 16 came from "28 volumes in the whole world (newland/land/fogfeatures.dat),
    // so 16 visible at once is already generous". That reasoning was wrong, and a
    // Ctrl+Alt+O trace taken in the Swamp while the user watched one volume blink
    // out and back measured why. Three frames over 11 seconds, publishable count
    // being blocks + sphere (cylinders are never published, and the two
    // fullscreen fog passes are value-rejected by the publisher):
    //
    //   f=1430058  13 blocks + 1 sphere = 14  ->  fits         volume VISIBLE
    //   f=1430192  17 blocks + 1 sphere = 18  ->  2 dropped    volume GONE
    //   f=1430377  16 blocks + 1 sphere = 17  ->  1 dropped    volume VISIBLE
    //
    // Kenshi's block hulls are much larger than the 50000-unit view distance
    // (fog_planes_vs clamps pos.z to pos.w precisely because they cross the far
    // plane), so most of the world's blocks pass its frustum cull at once. 16 is
    // simply too small for ordinary play.
    //
    // The overflow policy made it worse: add() keeps the FIRST kMaxFogVolumes and
    // drops the tail, over a draw order Ogre re-sorts back-to-front every frame.
    // So which volume fell off the end changed with camera yaw, and the fog it
    // carried vanished and returned with no relation to where it sat on screen.
    // The sphere lands in slot 3, then 5, then 4 across the three frames above -
    // that is the sort moving under the cut.
    //
    // 32 is a ceiling, not a guess: fogfeatures.dat holds exactly 26 publishable
    // volumes (25 blocks + 1 sphere), so with duplicates rejected the store can
    // never overflow from real geometry. A mod that authors more volumes is the
    // only way past it, and m_dropped is there to say so.
    //
    // Cost is constant-buffer bytes only: 160 per volume, so CompositeArgs goes
    // 8288 -> 10848, inside a 64 KiB uniform buffer. Per-pixel cost does rise,
    // because the live count may now exceed 16 where it used to be clamped.
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

      // DX11_V507: the clear is keyed on the FRAME ID, not driven by an external
      // begin-frame call.
      //
      // V505/V506 cleared this from D3D11Rtx::ResetCommandListState(), which is
      // NOT a per-frame hook - it runs whenever the D3D11 command-list state is
      // reset, several times per frame. The volumes were therefore wiped after
      // the fog draws and before the composite read them, and the same mistake
      // zeroed the census counters: `fogVol=` read 0 in 2500 frames of 2533 while
      // the volumes were being detected correctly every time.
      //
      // Keying on the frame id makes the lifetime correct no matter where the
      // writer is called from.
      void add(const Volume& volume, uint32_t frameId) {
        std::lock_guard<std::mutex> lock { m_lock };
        if (frameId != m_frameId) {
          m_frameId = frameId;
          m_count = 0u;
        }
        // DX11_V754: reject a volume already published in this generation.
        //
        // The generation key above is getCurrentFrameId(), which is
        // QueuePresentCount and is advanced ON THE CS THREAD, inside the EmitCs
        // lambda in D3D11SwapChain::SubmitPresent. This writer runs on the app
        // thread, which DXVK lets run up to maxFrameLatency frames ahead over an
        // unbounded CS chunk queue, so the key does NOT change one-for-one with
        // game frames: two frames' fog draws can land in one generation.
        //
        // That is measured, not inferred. The submit summary's fogVol= counter is
        // keyed the same way and shows exact doublings of the steady value,
        // flanked by that value on both sides:
        //
        //   d3d11.2508.log    22:10:32.026 fogVol=12, 35.077 12, 36.382 24, 36.428 12
        //   d3d11.27480.log   5 -> 10 -> 5, 8 -> 17 -> 8, 3 -> 6 -> 3 (x5), 2 -> 4 -> 2 (x6)
        //
        // A doubled generation applies every volume's alpha twice - denser fog for
        // one frame, the reported pulsing - and burns twice the slots. Rejecting
        // an identical volume makes both harmless without the generation key
        // having to be right, which is the cheaper half of the fix. Moving the
        // snapshot onto the app thread at the injection site is still outstanding.
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
      // DX11_V754: what counts as the same volume for dedup.
      //
      // sunLight is deliberately EXCLUDED. It is a per-draw harvest of
      // sunColour.w out of per-program constant buffers that are refreshed at
      // slightly different moments, so two frames' copies of one volume can
      // differ there by a hair. Chapter 41 measured exactly that class of
      // disagreement (0.0375 against 0.0400 on a smooth ramp), and including it
      // here would defeat the dedup on precisely the frames it exists to catch.
      // Everything else is authored, read from the same constant-buffer bytes,
      // and bit-identical between frames.
      //
      // All seven plane rows and worldOffset are compared whatever the type:
      // Volume's default member initialisers zero them and the publisher in
      // d3d11_rtx.cpp value-initialises each `published` before filling it, so
      // the rows a sphere does not use hold zero on both sides.
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
