#pragma once
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V218_BRUTE_FORCE_BUFFER_COOKIE_SINGLE_METHODS
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V216_IDENTIFY_AND_FIX_UNDECLARED_CONTENT_COOKIE
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V215_GDI_NO_D3DUKMDT_AND_BUFFER_COOKIE_FIX
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V214_D3D11_FEATURE_QUERY_AND_GDI_FORMAT_FIX
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V213_FIX_WX_C4099_C4146
#endif
#include "d3d11_include.h"

#include "../dxvk/rtx_render/rtx_types.h"
#include "../dxvk/rtx_render/rtx_hashing.h"
#include "../dxvk/rtx_render/rtx_materials.h"
#include "../dxvk/rtx_render/rtx_utils.h"
#include "../dxvk/dxvk_buffer.h"
#include "../util/util_matrix.h"
#include "../util/util_threadpool.h"

#include <unordered_set>

namespace dxvk {

  class D3D11DeviceContext;

  class D3D11Rtx {
  public:
    explicit D3D11Rtx(D3D11DeviceContext* pContext);

    // DX11_V225: DX11 capture-layer options exposed in the RTX Remix "Game Setup"
    // menu. RTX_OPTION generates both the value accessor (e.g. useVertexCapture())
    // and the option-object accessor (useVertexCaptureObject()) the UI binds to.
    RTX_OPTION("rtx", bool, useVertexCapture, true, "Capture programmable vertex-shader output so such draws can be ray traced / terrain baked.");
    RTX_OPTION("rtx", bool, useVertexCapturedNormals, true, "Use normals produced by vertex capture instead of input-assembler normals.");
    RTX_OPTION("rtx", bool, useWorldMatricesForShaders, true, "Use captured world/view matrices when reconstructing transforms for programmable-shader draws.");
    RTX_OPTION("rtx", bool, allowCubemaps, false, "Allow cubemap render targets/draws to be considered for ray tracing.");
    RTX_OPTION("rtx", bool, orthographicIsUI, true, "Treat draws with an orthographic projection as UI and skip them from the ray traced scene.");
    RTX_OPTION("rtx", bool, projectionYFlipOverride, false, "Override automatic projection Y-axis detection. Enable this when a game engine's clip-space Y convention is detected incorrectly.");
    RTX_OPTION("rtx", bool, projectionYFlip, true, "Flip the ray-traced projection Y axis when rtx.projectionYFlipOverride is enabled. This is commonly required by Unity games that use a negative Y projection scale.");
    RTX_OPTION("rtx", float, integerTexcoordScale, 1.0f / 2048.0f, "Scale applied when decoding fixed-point integer (SINT/UINT) texcoord vertex formats to floating point UVs. Engines that store UVs as 16-bit integers use an engine-specific divisor; 1/2048 fits common fixed-point conventions (Saints Row IV: TEXCOORD0 = R16G16_SINT). Adjust per game in rtx.conf if textures tile incorrectly.");
    RTX_OPTION("rtx", float, fallbackCameraFovDegrees, 60.0f, "Vertical field of view (degrees) of the synthesized fallback camera used when no projection matrix is found in any constant buffer. Tune per game in rtx.conf when the traced image looks zoomed relative to the raster view. Clamped to [20, 140].");

    RTX_OPTION("rtx.d3d11", int, csChunkFlushesPerFrame, 8,
               "How many times per frame the capture layer hands recorded work to the command-stream thread, so the GPU can start on a batch while the CPU is still recording the rest of the frame.\n"
               "The flush interval is derived each frame from the previous frame's draw volume to hit this count, clamped to a sane range. The old behaviour was a fixed 512-draw interval, which meant any title submitting fewer than 512 draws per frame never flushed mid-frame at all - the GPU idled through recording and then received the whole frame at once, which reads as stutter in scenes that should be cheap.\n"
               "Raise it for smoother pacing on light frames at the cost of more submissions; lower it if a title is submission-bound. Set to 0 to restore the old fixed-interval behaviour.");

    RTX_OPTION("rtx.d3d11", bool, conservativeOcclusionQueries, false,
               "Answer the game's occlusion queries as 'visible' immediately instead of reporting what the rasterized scene actually measured.\n"
               "When Remix path traces, the draws a game brackets in an occlusion query are captured rather than rasterized, so the query's scope measures nothing and returns zero samples. The game reads that as 'fully occluded' and culls the object, which removes it from the ray-traced scene too - typically as flicker, popping, or geometry that vanishes when the camera moves.\n"
               "Answering conservatively keeps that geometry alive. It also removes the readback stall: games spin on GetData waiting for a GPU result that is meaningless here, and each poll is a synchronous round trip (especially costly for 32-bit titles going through the bridge).\n"
               "Enable for games that lose geometry or flicker under path tracing. Leave off if the title still relies on real occlusion results for gameplay-visible logic, since every query will report visible.");
    RTX_OPTION("rtx.d3d11", bool, respectPredicatedDraws, false,
               "Implement D3D11 predication instead of ignoring it.\n"
               "Games call SetPredication with an occlusion predicate to drop work for geometry an earlier query proved invisible. This runtime previously recorded the predicate and did nothing with it, so every predicated draw and dispatch executed anyway and the capture layer submitted it - adding ray-traced instances, BLAS builds and raster work for objects the game had already determined the player cannot see.\n"
               "Enabling this honours the predicate across all draw and dispatch entry points, skipping the raster command and the RTX capture together so the two can never disagree.\n"
               "The predicate is read without blocking, and only a resolved result can skip work: an unresolved predicate always executes. That is the direction D3D11 permits implementations to be conservative in, so geometry is never lost to a late readback.\n"
               "Note that copies, clears and resolves remain unpredicated - executing them is always spec-legal and they are not a meaningful cost here.\n"
               "Do not combine with rtx.d3d11.conservativeOcclusionQueries: that option forces every predicate to report visible, which makes this one a no-op.");
    RTX_OPTION("rtx.d3d11", bool, logOcclusionQueries, false,
               "Log a one-time summary the first time an occlusion query is answered conservatively, and count how often the game polls a still-pending occlusion readback. Use to confirm whether a title's missing geometry is occlusion-query driven.");

    RTX_OPTION("rtx.d3d11", bool, materialInstanceIdentity, false,
               "Identify a material by the pixel shader that renders it plus the full set of textures bound to the slots that shader declares, instead of by its primary albedo texture alone.\n"
               "Engines that build many material instances from one shader (varying only which textures/parameters are bound) otherwise collapse into a single hash, so tagging or replacing one of them hits all of them.\n"
               "With this enabled such instances get distinct hashes, and the replacement lookup falls back through shader+texture-set and then plain texture hashes, so existing texture-keyed replacements keep working.\n"
               "Requires shaders to retain their DXBC reflection (RDEF) chunk; draws whose shaders were compiled with reflection stripped fall back to texture-hash identity automatically.\n"
               "Off by default because it changes material hashes: captures and replacements authored before enabling it are keyed by the old hashes.");
    RTX_OPTION("rtx.d3d11", fast_unordered_set, materialInstanceIdentityExcludedShaders, {},
               "Bytecode hashes of pixel shaders that must not participate in rtx.d3d11.materialInstanceIdentity.\n"
               "Use for uber-shaders whose bound texture set varies frame to frame (dynamic atlases, streaming feedback), where including it in the identity would mint a new material hash every frame.\n"
               "Excluded shaders fall back to primary-texture identity.");

    // DX11_V384_FAMILY_ISOLATION: attribution by removal, for when no probe can
    // answer "which draw owns those pixels".
    //
    // Reading a per-pixel identity out of the path tracer keeps failing for
    // transparent geometry: alpha-blended and unordered surfaces never become
    // the committed surface, so object picking returns EMPTY and the GPU-print
    // probe reports no hit. Removing one shader family and looking at what
    // disappears has none of that trouble, and it is decisive in one run.
    //
    // Comma-separated fragments matched against the bridge's vertex shader name
    // (`VS_<sha1>`, the same string the logs print). A draw whose VS name
    // contains ANY listed fragment stays on raster and never enters the RTX
    // scene. An 8-hex prefix like "eda75c5b" is the natural granularity.
    //
    // Diagnostic only - empty by default, and it changes nothing when empty.
    RTX_OPTION("rtx.d3d11", std::string, kenshiIsolateVertexShaders, "",
               "Comma-separated vertex-shader name fragments to EXCLUDE from the RTX scene (kept on the raster layer).\n"
               "Matched as a substring against the bridge's shader name, e.g. 'eda75c5b' or 'eda75c5b,1efe737d'.\n"
               "Diagnostic switch for attributing on-screen geometry to a draw family by removing it; empty disables it entirely.");

    void Initialize();
    bool OnDrawAuto();
    bool OnDraw(UINT vertexCount, UINT startVertex);
    bool OnDrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex);
    bool OnDrawInstanced(UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance);
    bool OnDrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance);
    bool OnDrawInstancedIndirect(ID3D11Buffer* argumentBuffer, UINT argumentOffset);
    bool OnDrawIndexedInstancedIndirect(ID3D11Buffer* argumentBuffer, UINT argumentOffset);
    void OnClearState();
    void ResetCommandListState();

    // Must be called with the context lock held.
    // EndFrame runs the RT pipeline writing output into backbuffer (called BEFORE recording the blit).
    static uint64_t GetReflexFrameId();
    static void IncrementReflexFrameId();

    void EndFrame(const Rc<DxvkImage>& backbuffer, VkExtent2D remixViewportExtent = { 0u, 0u });
    // Queue the same final-image capture used by the Remix developer window.
    // The swapchain WndProc uses this for Print Screen after consuming the key
    // before it reaches the vanilla game.
    void RequestScreenshot();
    // Queue one full internal RTX diagnostic capture. Unlike the normal
    // screenshot, this is consumed only at Kenshi's verified scene boundary.
    void RequestKenshiDebugScreenshot();
    // DX11_V329_ON_DEMAND_DIAGNOSTICS: arm one frame of verbose diagnostics
    // (Ctrl+Alt+D). No data members are added for this - the state lives in
    // translation-unit statics in d3d11_rtx.cpp, because adding fields to this
    // class changes its layout and silently kills startup.
    void ArmOnDemandDiagnostics();
    // The single user-facing safety baseline. It keeps D3D11 capture and
    // rigid-world-instance submission plus frontend-state-restore active, but preserves the game's
    // native raster image until Kenshi's presentation boundary is identified.
    bool ToggleUserSafeCaptureBaseline();
    bool isUserSafeCaptureBaselineEnabled() const { return m_userSafeCaptureBaseline; }
    // OnPresent registers the swapchain present image (called AFTER recording the blit).
    void OnPresent(const Rc<DxvkImage>& swapchainImage, VkExtent2D remixViewportExtent = { 0u, 0u });

    uint32_t getDrawCallID() const { return m_drawCallID; }
    uint32_t getAcceptedSceneDrawCount() const;

  private:
    static constexpr uint32_t kMaxConcurrentDraws = 6 * 1024;
    using GeometryProcessor = WorkerThreadPool<kMaxConcurrentDraws>;

    D3D11DeviceContext*                  m_context;
    std::unique_ptr<GeometryProcessor>   m_pGeometryWorkers;
    uint32_t                             m_drawCallID = 0;

    // DX11_V301_PERF_LOG: CPU cost of the draw-submission path, accumulated per
    // frame. The raytracing passes already time themselves and come in around a
    // couple of milliseconds, so when frames are slow the time is being spent
    // here - this attributes it to a specific draw rather than a whole frame.
    uint64_t                             m_frameDrawCpuNs = 0;
    uint64_t                             m_frameSlowestDrawNs = 0;
    // DX11_V312_PHASE_TIMERS: per-frame wall time inside the three candidate
    // blockers, to locate the fixed ~96ms stall that one draw absorbs each
    // frame. mutable because FillMaterialData is const.
    mutable uint64_t                     m_framePhaseExtractNs = 0;
    mutable uint64_t                     m_framePhaseMaterialNs = 0;
    mutable uint64_t                     m_framePhaseHelperNs = 0;
    uint32_t                             m_frameSlowestDrawId = 0;
    uint32_t                             m_frameSlowestDrawIndices = 0;
    XXH64_hash_t                         m_frameSlowestDrawHash = kEmptyHash;
    uint32_t                             m_frameTimedDraws = 0;
    // DX11_V319_PERF_LOG_THROTTLE: opening-burst counter, last reported frame
    // and the worst cost reported so far, so a permanently slow game samples
    // the probe periodically instead of writing one warning per frame.
    // DX11_V319_DEFERRED_LIGHT_VOLUMES: lights created from light-volume draws
    // this frame, bounded by rtx.dx11.deferredLightVolumeMaxPerFrame.
    uint32_t                             m_deferredLightVolumesThisFrame = 0;
    // DX11_V319_LIGHT_VOLUMES_SELF_ARM: evidence that this game lights deferred,
    // gathered from the draws themselves so no engine has to be recognised.
    uint32_t                             m_deferredLightVolumeCandidatesThisFrame = 0;
    uint32_t                             m_deferredLightVolumeArmingFrames = 0;
    bool                                 m_deferredLightVolumeAutoArmed = false;
    // DX11_V319_WORLD_SCAN_GIVE_UP: consecutive draws in which the world-matrix
    // search found nothing, per vertex shader. Keyed by shader because that is
    // what the answer is a property of - a different shader re-arms the search,
    // so this can never mismatch the way a cached location would.
    std::unordered_map<const void*, uint32_t> m_worldScanMissesByShader;
    uint32_t                             m_drawPerfLogCount = 0;
    uint32_t                             m_drawPerfLogLastFrame = ~0u;
    double                               m_drawPerfLogWorstMs = 0.0;

    // Cached projection cbuffer location — found on first draw with a perspective
    // matrix and reused for the rest of the frame. Reset to invalid in EndFrame.
    uint32_t                             m_projSlot   = UINT32_MAX;
    size_t                               m_projOffset = SIZE_MAX;
    int                                  m_projStage  = -1;
    // true when the engine stores matrices in column-major order (Unity, Godot).
    // Detected during the projection scan — all subsequent reads are transposed.
    bool                                 m_columnMajor = false;

    // Cached view matrix cbuffer location — mirrors projection caching.
    // Once a valid view matrix is found at (stage, slot, offset), subsequent
    // draws re-read from the same location instead of rescanning.
    uint32_t                             m_viewSlot   = UINT32_MAX;
    size_t                               m_viewOffset = SIZE_MAX;
    int                                  m_viewStage  = -1;
    bool                                 m_viewColumnMajor = false;
    // DX11_V260_PRECISE_CAMERA: the cached view location was confirmed by
    // matching candidateView x rawProjection against a stored ViewProj block
    // in the same cbuffer - decisive, because rigid-body checks alone cannot
    // tell the camera view from shadow-light views, mirror cameras, bone
    // matrices, or a stored camera-to-world.
    bool                                 m_viewConfirmed = false;
    // The confirmed view location belongs to a coherent camera-relative frame
    // block and may legitimately contain identity. The block is revalidated
    // before each identity view is accepted.
    bool                                 m_viewCameraRelative = false;
    // The confirmed location stores camera-to-world; invert on each re-read.
    bool                                 m_viewInverted = false;

    // DX11_V309_CAMERA_RESOLVER (shadow mode, step 1 of the camera replacement).
    // TryCapturePositionsViaStreamOut and SubmitDraw are separate functions, so
    // the capture path's two pieces of evidence have to be carried across to the
    // accept site where the comparison is made. These are READ-ONLY inputs to
    // the shadow log and must never influence rendering - the whole point of
    // step 1 is that behaviour is unchanged while the two opinions are compared.
    bool                                 m_shadowCapturedPostTransform = false;
    bool                                 m_shadowCaptureWorldAnchored = false;
    // Throttles late-session confirmation attempts to once per frame.
    uint32_t                             m_lastViewConfirmFrame = UINT32_MAX;

    // Cached world matrix cbuffer location — reduces per-draw scanning.
    // World matrices change every draw but often live at the same (stage, slot, offset).
    // Smoothed camera position — exponential moving average dampens
    // micro-jitter from floating-point rounding in cbuffer matrix extraction.
    Vector3                              m_smoothedCamPos = Vector3(0.0f);
    bool                                 m_hasPrevCamPos  = false;

    // Sole source of truth for resize transition detection. Only changes to
    // this extent trigger `m_resizeTransitionFramesRemaining` and
    // `resetScreenResolution`.
    VkExtent2D                           m_lastOutputExtent = { 0u, 0u };

    // Used only for viewport fallback. This is the Remix-owned output extent
    // (backbuffer or present image), not the game HWND client rect.
    VkExtent2D                           m_lastRemixViewportExtent = { 0u, 0u };

    // Axis convention auto-detection — voting system accumulates evidence
    // from projection and view matrices, then settles once confident.
    // Re-checks during warmup to correct boot/loading screen misdetections.
    bool                                 m_axisDetected = false;
    bool                                 m_axisLogged   = false;
    uint32_t                             m_axisDetectFrame = 0;

    // Voting counters for Z-up vs Y-up and LH vs RH.
    // Accumulate votes over multiple frames, settle when |votes| >= threshold.
    int                                  m_zUpVotes     = 0;  // positive = Z-up, negative = Y-up
    int                                  m_lhVotes      = 0;  // positive = LH, negative = RH
    int                                  m_yFlipVotes   = 0;  // positive = flipped, negative = normal
    bool                                 m_zUpSettled    = false;
    bool                                 m_lhSettled     = false;
    bool                                 m_yFlipSettled  = false;
    bool                                 m_projectionYFlipOverrideWasEnabled = false;
    bool                                 m_projectionYFlipOverrideInitialized = false;
    static constexpr int kVoteThreshold  = 5; // votes needed to settle
    mutable Rc<DxvkSampler>              m_defaultSampler;

    // CPU-GPU pacing: flush the CS chunk every N draws to prevent the CPU
    // from queuing unbounded work while the GPU is still on a prior batch.
    // Without this, frame latency spikes and memory pressure builds from
    // thousands of buffered DrawCallState objects.
    //
    // A fixed interval cannot serve both ends of the range. At 512 a title that
    // submits fewer than 512 draws per frame never flushes mid-frame at all: the
    // entire frame accumulates in one chunk and is handed over at end-of-frame,
    // so the GPU sits idle while the CPU records and is then handed the whole
    // backlog at once. That is the case that "should be fast but isn't" - light
    // and mid-weight frames get the worst overlap. The interval is therefore
    // derived per frame from the previous frame's draw volume (see
    // ResetCommandListState) and only clamped by these bounds.
    static constexpr uint32_t kMinDrawsPerFlush = 32;
    static constexpr uint32_t kMaxDrawsPerFlush = 512;
    // Used until a frame's draw volume has been observed. Deliberately far below
    // kMaxDrawsPerFlush so the first frames of a level load overlap too.
    static constexpr uint32_t kInitialDrawsPerFlush = 128;
    uint32_t                             m_drawsPerFlush = kInitialDrawsPerFlush;
    // Bound per-draw frontend analysis so large meshes do not dominate DX11
    // CPU time when a real scene first appears.
    static constexpr uint32_t kMaxHashedVertices = 1024;
    static constexpr uint32_t kMaxHashedIndices = 4096;
    static constexpr uint32_t kMaxSkinningVerticesToScan = 1024;
    static constexpr uint32_t kResizeCameraGraceFrames = 45;
    static constexpr uint32_t kSceneCameraGraceFrames = 90;

    struct SubmitRejectStats {
      uint32_t total = 0;
      uint32_t accepted = 0;
      uint32_t sceneAccepted = 0;
      uint32_t realSceneAccepted = 0;
      uint32_t sceneCandidates = 0;
      uint32_t significanceCulled = 0;
      uint32_t queueOverflow = 0;
      uint32_t nonTriangleTopology = 0;
      uint32_t noPixelShader = 0;
      uint32_t noRenderTarget = 0;
      uint32_t trivialDraw = 0;
      uint32_t fullscreenPostFx = 0;
      uint32_t noInputLayout = 0;
      uint32_t noSemantics = 0;
      uint32_t noPositionSemantic = 0;
      uint32_t noTexcoordLayout = 0;
      uint32_t texcoordGenerated = 0;
      // DX11_V280: no-TEXCOORD-layout draws whose UVs were recovered from the
      // vertex shader's output via the stream-out capture replay.
      uint32_t texcoordCaptured = 0;
      // DX11_V290: draws whose true post-skinning/post-transform world/view
      // positions were captured from the vertex shader for BLAS input.
      uint32_t positionCaptured = 0;
      // Camera-relative draws must never fall back to a guessed cbuffer world
      // matrix when exact post-VS capture is unavailable. Mixing those two
      // coordinate systems creates the giant enclosing planes/black rectangle
      // failure. These draws are deliberately omitted instead.
      uint32_t unsafeCameraRelativeSkipped = 0;
      // Exact post-VS captures that could not be scheduled inside the bounded
      // per-frame GPU work/allocation budget. Kept separate from structural
      // capture failures so field logs show whether a scene needs more budget.
      uint32_t positionCaptureBudgetRejected = 0;
      // NOTE: capture attempt/failure counters deliberately live as
      // translation-unit state in d3d11_rtx.cpp, NOT here. Adding fields to
      // this struct changes the D3D11Rtx object layout, and this project has
      // already shipped one build that exited silently just after Kenshi's
      // pre-game menu for exactly that reason. Diagnostics must not alter
      // this class's layout.
      uint32_t position2D = 0;
      uint32_t noPositionBuffer = 0;
      uint32_t noIndexBuffer = 0;
      uint32_t compositeSkip = 0;
      uint32_t screenSpaceUiSkip = 0;
      // DX11_V314: meshes dropped because they collapsed onto the viewpoint
      // (enclose the eye AND both object and camera translations are zero).
      uint32_t collapsedEyeGeometry = 0;
      uint32_t screenSpaceGarbageSkip = 0;
      uint32_t geometryHashScheduleFailed = 0;
      uint32_t forceInjectionIdle = 0;
      // DX11_V249: draws dropped because their position stream cannot be made
      // safe for the interleaver/BLAS (unconvertible format, all-garbage data,
      // or an index range whose vertex extent cannot be bounded).
      uint32_t positionFormatRejected = 0;
      uint32_t poisonedPositions = 0;
      uint32_t vertexRangeRejected = 0;
      // Draws whose index stream is truncated, overflows, or addresses a
      // vertex outside the submitted position slice. Passing any of these to
      // a Vulkan BLAS build is undefined and can reset the GPU driver.
      uint32_t indexRangeRejected = 0;
      // DX11_V277: depth-only draws (prepass/shadow re-renders) excluded so
      // geometry never enters the RT scene as stacked coincident copies.
      uint32_t depthOnlySkipped = 0;
      // DX11_V281: wireframe-fill draws (debug/editor overlays) excluded -
      // their triangles are lines on screen, not solid RT surfaces.
      uint32_t wireframeSkipped = 0;
      // Host emulator draws whose guest vertices have already been transformed
      // to screen XY/depth. They are deliberately kept on the native raster
      // surface because there is no world-space BLAS or camera to reconstruct
      // at the D3D11 boundary.
      uint32_t postTransformEmulator = 0;
    };

    SubmitRejectStats                    m_submitRejectStats;
    // Screen-space UI is raster composition, not ray-traced world geometry.
    // Keep the texture hashes discoverable for manual categorization, but
    // preserve draw order by either injecting immediately before the first
    // late UI draw or passing an early-UI frame through unchanged.
    bool                                 m_rasterUiSeenThisFrame = false;
    bool                                 m_midFrameRtxInjected = false;
    bool                                 m_forceRasterPassThroughThisFrame = false;
    bool                                 m_userSafeCaptureBaseline = false;
    // A raster -> PT transition arms one bounded frame of committed-draw
    // telemetry. The toggle is sampled during Present, so Pending becomes
    // Active at that Present's EndFrame and records the following game frame.
    bool                                 m_ptTransitionTelemetryPending = false;
    bool                                 m_ptTransitionTelemetryActive = false;
    uint32_t                             m_ptTransitionTelemetryLogged = 0;
    bool                                 m_kenshiDebugScreenshotRequested = false;
    // Kenshi finishes its world composite with a full-screen swapchain draw,
    // immediately calls ClearState, then starts native UI composition.  The
    // target is armed by that exact draw signature and consumed at ClearState,
    // which is the safe ordering point for the RTX composite.
    Rc<DxvkImage>                         m_kenshiPendingCompositeTarget;
    // Native raster is required while the game constructs its frame and for
    // proven late UI, but it must not execute for scene/helper draws after the
    // RTX composite has already replaced the color target. This per-API-draw
    // decision is consumed by D3D11DeviceContext before it queues the native
    // draw command.
    bool                                 m_allowNativeRasterForCurrentDraw = true;
    // Process-lifetime capability latch. PCSX2's GS renderer exposes packed
    // screen XY/Z (or an SV_VertexID StructuredBuffer) rather than the guest
    // game's pre-transform world vertices. Once observed, every later frame
    // must preserve the complete guest raster surface; otherwise force-
    // injection or a stale scene can replace it with an empty black RTX frame.
    bool                                 m_postTransformEmulatorHost = false;
    // Set only by the versioned private-data ABI. Once authenticated, legacy
    // PCSX2 layout heuristics can no longer force the whole process to raster.
    bool                                 m_authenticatedEmulatorHost = false;
    bool                                 m_hasSeenRealSceneProjection = false;
    // Learned only from a real projection whose aspect agrees with the
    // established output. Once learned, square reflection/probe cameras can no
    // longer masquerade as the primary camera on a widescreen swap chain.
    bool                                 m_hasSeenOutputAspectCompatibleProjection = false;
    uint32_t                             m_drawsSinceFlush = 0;
    uint32_t                             m_resizeTransitionFramesRemaining = 0;

    // Persistence escape hatch for the occluding-helper-window heuristic.
    // If the same "occluding" extent keeps arriving, the window genuinely
    // became small (user resized below the heuristic floor) and must
    // eventually be accepted instead of being rejected forever. The counter
    // accumulates rejection events (up to ~4 per frame: output + viewport
    // tracker in both EndFrame and OnPresent), so 240 events is roughly
    // 1-2 seconds at 60 fps.
    VkExtent2D                           m_pendingRejectedExtent = { 0u, 0u };
    uint32_t                             m_pendingRejectedExtentCount = 0;
    static constexpr uint32_t kRejectedExtentAcceptEvents = 240;

    // Resize debounce: deferred pipelines bind several RT sizes per frame at
    // scene transitions, which previously re-triggered resize grace every
    // frame (a "resize storm" - 8 consecutive camera carry-forwards observed
    // in Saints Row IV at the scene->loading boundary). A new output extent
    // must persist for this many consecutive observations before it commits.
    VkExtent2D m_pendingResizeExtent = { 0u, 0u };
    uint32_t   m_pendingResizeCount = 0;
    static constexpr uint32_t kResizeDebounceFrames = 3;

    // Frame id of the most recent real cbuffer projection. Lets
    // m_hasSeenRealSceneProjection decay after extended camera absence so
    // menu / loading-screen draws (which rely on viewport fallback) are not
    // permanently blocked once a session has run (Saints Row IV bug 1).
    uint32_t m_lastRealCameraFrameId = 0;

    // Minimum coverage for origin-anchored viewport-fallback acceptance once
    // a stable scene extent exists (loading screens render small origin
    // rects: SR4 uses 600x337 = 31% during loads).
    static constexpr float kMinNearOriginCoverage = 0.25f;

    // forceInjection overflow guard (Saints Row IV open world): with
    // injection forced and no usable scene found in the previous frame, cap
    // geometry submission to a probe window so camera discovery still works
    // but the RT acceleration structure cannot balloon to thousands of junk
    // instances (25k draws -> 6144-instance AS rebuilt every frame, ~1.4 s).
    uint32_t m_prevFrameSceneAccepted = 0;
    uint32_t m_prevFrameRealSceneAccepted = 0;
    static constexpr uint32_t kForceInjectionProbeDraws = 512;
    // DX11_V295_ROTATING_PROBE: sweep state for the force-injection discovery
    // window (heavy frames put their camera/scene draws past a fixed first-N
    // window - Hello Neighbor 2 issues ~2000 draws per frame).
    uint32_t m_prevFrameTotalDraws = 0;
    uint32_t m_forceInjectionProbePhase = 0;

    // UE-style significance manager (rtx.significanceCulling, default OFF).
    // Unreal's Significance Manager spends a fixed budget by importance, not by
    // arrival order. Applied here so that when a frame's scene draws exceed
    // rtx.maxInstanceSubmissions, the draws KEPT are the ones nearest the
    // camera (importance = camera-space depth), instead of whatever the engine
    // happened to submit first - which in a 25k-draw open world is often
    // distant terrain/skybox while the geometry you are looking at is dropped.
    // A multiplicative control loop adjusts a squared-distance threshold toward
    // the budget each frame, so it is temporal (no hard pop) and arms only when
    // real demand exceeds budget. m_significanceMaxDistanceSq == 0 means
    // "no limit / disarmed".
    float    m_significanceMaxDistanceSq = 0.0f;
    uint32_t m_prevFrameSceneCandidates = 0;

    // Single coherent policy for maintaining m_lastOutputExtent and
    // m_lastRemixViewportExtent. Called from both EndFrame and OnPresent so
    // the two paths cannot drift apart again.
    void UpdateTrackedExtents(const Rc<DxvkImage>& outputImage, VkExtent2D remixViewportExtent);

    Rc<DxvkSampler> getDefaultSampler() const;

    // DX11_V280_TEXCOORD_CAPTURE: recover texture coordinates for textured
    // draws whose input layout has no TEXCOORD stream by replaying the draw's
    // vertex range through a stream-out passthrough pipeline (game VS +
    // generated xfb GS, rasterization discarded) and feeding the captured
    // per-vertex UVs to the RT geometry. Engine-agnostic: keyed purely off
    // the VS output signature and device transform-feedback support.
    //
    // DX11_V394_NORMAL_CAPTURE: the same replay serves any per-vertex stream
    // the vertex shader computes. Passing an explicit semantic and a component
    // count of 3 captures a world NORMAL instead of a UV - needed where the IA
    // normal is not usable as one (Kenshi's terrain stores it component
    // rotated) and where the RT side wants the shader's own normal anyway.
    bool TryCaptureTexcoordsViaStreamOut(DrawCallState& dcs, RasterGeometry& geo,
                                         bool indexed, UINT count, UINT start, INT base,
                                         const char* overrideSemanticName = nullptr,
                                         uint32_t overrideSemanticIndex = 0,
                                         uint32_t componentCount = 2u);
    uint32_t     m_texcoordCapturesThisFrame = 0;
    VkDeviceSize m_texcoordCaptureBytesThisFrame = 0;

    // DX11_V290_POST_VS_POSITION_CAPTURE: re-evaluate shader-generated vertex
    // positions into a device-local stream before the RT scene consumes them.
    // This is required when IA POSITION is object space but skinning and all
    // model/view transforms live only in the game VS.
    bool TryCapturePositionsViaStreamOut(DrawCallState& dcs, RasterGeometry& geo,
                                         bool indexed, UINT count, UINT start, INT base,
                                         bool hasExternalInstanceTransform,
                                         UINT replayFirstInstance,
                                         UINT replayInstanceCount,
                                         bool requireIndexedFlatten);
    // DX11_V322_CAPTURE_FAILURE_VISIBILITY: the real work lives in Impl; the
    // public entry point above only counts attempts/successes and records a
    // state snapshot for the first few failures. Capture can stop producing
    // geometry entirely while every admission counter still reports the draw
    // as accepted (observed: scene=221, zero rejections, posCapture=0, and
    // the raster layer showing through the path-traced view). Attempt and
    // failure counts make that condition self-evident in the submit summary.
    bool TryCapturePositionsViaStreamOutImpl(DrawCallState& dcs, RasterGeometry& geo,
                                         bool indexed, UINT count, UINT start, INT base,
                                         bool hasExternalInstanceTransform,
                                         UINT replayFirstInstance,
                                         UINT replayInstanceCount,
                                         bool requireIndexedFlatten);
    uint32_t     m_positionCapturesThisFrame = 0;
    uint32_t     m_positionNewCaptureBuffersThisFrame = 0;
    uint32_t     m_positionReplayCapturesThisFrame = 0;
    VkDeviceSize m_positionCaptureBytesThisFrame = 0;
    uint64_t     m_positionCaptureVerticesSinceSubmission = 0;
    // One detailed line per distinct position-capture contract. Unlike the
    // old process-global 24-line counter, this remains useful after a long
    // menu and identifies the exact gameplay shader/draw admitted before a
    // driver reset without logging every hot-path occurrence.
    std::unordered_set<uint64_t> m_positionCaptureContractsLogged;

    // DX11_V286_GAMEPLAY_MATRIX_DUMP: env-free camera diagnostic. Steam's DRM
    // relaunch strips DXVK_REMIX_MTXDUMP from the child process, so the env
    // dump never fires for Steam titles. When a real gameplay scene frame is
    // detected (many accepted scene draws) but the view is unresolved/wrong,
    // EndFrame arms a short dump burst; ExtractTransforms then logs the full
    // live cbuffer matrix landscape ([gpdump] lines) for those frames -
    // undeduped, so the true per-frame values at the camera cbuffer locations
    // are visible. Bounded by a line budget and a small burst count.
    uint32_t m_forceMatrixDumpUntilFrame = 0;
    uint32_t m_forceMatrixDumpLines = 0;
    uint32_t m_forceMatrixDumpBursts = 3;

    // DX11_V285_TEXCOORD_CAPTURE_REUSE: one persistent capture buffer per draw
    // identity (VS + vertex-buffer bindings + vertex range), reused across
    // frames instead of allocating a fresh device-local buffer per draw per
    // frame. The per-draw-per-frame allocations flooded VRAM and the scene
    // manager's unique-buffer table within seconds of world rendering (Skyrim:
    // RADAR_PRE_LEAK + "pushing more unique buffers" + OMM budget collapse).
    // Re-replays into the same buffer at most once per frame via a fresh
    // rename slice (invalidateBuffer - the standard dxvk discard pattern), so
    // the GPU never races a slice it is still reading and the buffer object
    // stays stable for the scene manager's caches.
    struct TexcoordCaptureEntry {
      Rc<DxvkBuffer> buffer;
      VkDeviceSize   capacity = 0;
      uint32_t       lastUsedFrame = 0;
      uint32_t       lastCapturedFrame = ~0u;
    };
    std::unordered_map<uint64_t, TexcoordCaptureEntry> m_texcoordCaptureCache;
    VkDeviceSize m_texcoordCaptureCacheBytes = 0;
    void SweepTexcoordCaptureCache(uint32_t currentFrame);

    struct PositionCaptureEntry {
      Rc<DxvkBuffer> buffer;
      VkDeviceSize   capacity = 0;
      uint32_t       lastUsedFrame = 0;
      uint32_t       lastCapturedFrame = ~0u;
      uint64_t       contractIdentity = 0;
      uint64_t       transformStateIdentity = 0;
      bool           hasTransformStateIdentity = false;
      Matrix4        canonicalCapturedToWorld;
      bool           hasCanonicalCapturedToWorld = false;
      // Homogeneous clip coordinates must always be unprojected with the
      // inverse projection from the capture that produced them. Keeping this
      // pair together lets projection jitter/FOV changes reuse unchanged
      // geometry without mixing old clip coordinates with a new projection.
      Matrix4        capturedClipToPosition;
      bool           hasCapturedClipToPosition = false;
      bool           capturedClipUsesWDepth = false;
      // DX11_V319_WORLD_ANCHOR_CAMERA: everything needed to turn the bytes
      // sitting in this buffer back into camera-relative world offsets one
      // frame after they were written. Stored with the buffer rather than read
      // live because a capture's bytes belong to the view that produced them,
      // not to whichever view happens to be current when they are sampled.
      Matrix4        capturedViewRotationToWorld;
      bool           hasCapturedViewRotationToWorld = false;
      uint32_t       capturedVertexCount = 0;
      uint32_t       capturedStride = 0;
    };
    std::unordered_map<uint64_t, PositionCaptureEntry> m_positionCaptureCache;
    std::unordered_map<uint64_t, uint32_t> m_positionCaptureOccurrencesThisFrame;
    VkDeviceSize m_positionCaptureCacheBytes = 0;
    void SweepPositionCaptureCache(uint32_t currentFrame);

    // DX11_V319_WORLD_ANCHOR_CAMERA: bounded, stall-free readback of a few
    // vertices per mesh out of the post-VS capture buffers, feeding the
    // camera-position solve for engines that render camera-relative.
    //
    // Those capture buffers are DEVICE_LOCAL and cannot be mapped at all, so
    // the samples are copied into a small host-visible buffer by copyBuffer on
    // the CS thread and read one frame later, when the copy has certainly
    // retired. Two buffers ping-pong so the batch being read is never the batch
    // being written. Reading the CURRENT frame's copies instead would mean
    // blocking the render thread on the GPU - the ~100ms round trip the capture
    // path already goes out of its way to avoid (DX11_V303).
    //
    // The resulting camera position therefore lags the game by two frames. That
    // is deliberately not compensated: the camera and the geometry are anchored
    // with the SAME position, so a constant lag is a constant offset of the
    // whole scene, which is invisible. Only acceleration leaves a residual, and
    // the next frame's solve absorbs it.
    static constexpr uint32_t kCameraAnchorSampleVertices  = 4u;
    static constexpr uint32_t kCameraAnchorSampleSlotBytes = 128u;
    static constexpr uint32_t kCameraAnchorMaxSampleMeshes = 48u;
    static constexpr float    kCameraAnchorMaxTranslationPerFrame = 2000.0f;
    struct CameraAnchorSampleRequest {
      uint64_t meshKey = 0;
      Matrix4  viewRotationToWorld;
      Matrix4  clipToPosition;
      bool     clipUsesWDepth = false;
      uint32_t vertexCount = 0;
      uint32_t stride = 0;
    };
    Rc<DxvkBuffer>                         m_cameraAnchorStaging[2];
    std::vector<CameraAnchorSampleRequest> m_cameraAnchorRequests[2];
    uint32_t                               m_cameraAnchorWriteIndex = 0;
    uint32_t                               m_cameraAnchorLastConsumedFrame = ~0u;
    // Set per draw by ExtractTransforms: the game supplied a real view matrix
    // whose translation is exactly zero, i.e. it renders camera-relative.
    bool                                   m_cameraAnchorViewTranslationFree = false;
    void QueueCameraAnchorSample(const PositionCaptureEntry& entry, uint64_t meshKey);
    void ConsumeCameraAnchorSamples();

    // DX11_V285_HELPER_BUFFER_POOL: host-visible helper buffers (dynamic
    // vertex/index snapshots, format-conversion outputs, skinning streams)
    // must be FRESH per draw - record-time slice resolution forbids renaming
    // one shared buffer within a frame (the V250 bug class) - but the buffer
    // OBJECTS are recyclable: once the pool holds the only reference
    // (refCount()==1: no DrawCallState, no geometry-cache entry, no pending
    // hash job) and the GPU has retired every command list that touched it
    // (isInUse()==false), the object can back a new helper without a fresh
    // allocation. Kills the per-draw-per-frame buffer-object churn that
    // flooded system commit (Windows RADAR_PRE_LEAK) and the scene manager's
    // per-frame unique-buffer table during world rendering in every game.
    struct HelperPoolItem {
      Rc<DxvkBuffer> buffer;
      VkDeviceSize   capacity = 0;
    };
    std::vector<HelperPoolItem> m_helperRetired;
    std::vector<HelperPoolItem> m_helperFree;
    VkDeviceSize m_helperPoolBytes = 0;
    // DX11_V298_BOUNDED_HELPER_OVERFLOW: per-frame budget for allocations past
    // the pool cap; once exhausted, acquisitions fail instead of growing VRAM.
    VkDeviceSize m_helperUnpooledBytesThisFrame = 0;
    uint32_t m_helperUnpooledFrame = ~0u;
    Rc<DxvkBuffer> AcquireHostVisibleHelperBuffer(VkDeviceSize size, const char* name, bool cpuCached = false);
    void RecycleHelperBuffers();

    void SubmitDraw(bool indexed, UINT count, UINT start, INT base,
                    const Matrix4* instanceTransform = nullptr,
                    UINT replayFirstInstance = 0,
                    UINT replayInstanceCount = 1,
                    bool requireExactPositionCapture = false);
    void BeginNativeRasterDrawRouting();
    void SubmitInstancedDraw(bool indexed, UINT count, UINT start, INT base,
                             UINT instanceCount, UINT startInstance);
    DrawCallTransforms ExtractTransforms();
    Future<GeometryHashes> ComputeGeometryHashes(const RasterGeometry& geo,
                                                 uint32_t vertexCount,
                                                 uint32_t hashStartVertex,
                                                 uint32_t hashVertexCount) const;
    void ClearMaterialTextures(LegacyMaterialData& mat) const;
    void FillMaterialData(LegacyMaterialData& mat,
                          XXH64_hash_t primaryTextureHashOverride = 0) const;
  };

}
