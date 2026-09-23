#include "../../src/dxvk/rtx_render/rtx_options.h"
#include "../../src/dxvk/rtx_render/rtx_option_manager.h"
#include "../../src/dxvk/rtx_render/rtx_rtxdi_rayquery.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace dxvk;

static void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

static std::string contents(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

static void apply() { dxvk::RtxOptionManager::applyPendingValues(nullptr, false); }

static void verifySavedValues() {
  require(!RtxOptions::ShadowTerminator::enableOffset(), "Shadow offset reverted");
  require(!RtxOptions::ShadowTerminator::soften(), "Shadow softening reverted");
  require(DxvkRtxdiRayQuery::initialSampleCount() == 7, "Initial samples reverted");
  require(DxvkRtxdiRayQuery::spatialSamples() == 5, "Spatial samples reverted");
  require(DxvkRtxdiRayQuery::disocclusionSamples() == 4, "Disocclusion samples reverted");
}

// Run with an isolated working directory containing the test user.conf and rtx.conf.
// Separate save/reload processes exercise real file loading, not cached layer state.
int main(int argc, char** argv) {
  try {
    require(argc == 2, "Expected save or reload");
    RtxOptionLayer::initializeSystemLayers();
    RtxOptionImpl::setInitialized(true);
    dxvk::RtxOptionManager::markOptionsWithCallbacksDirty();
    dxvk::RtxOptionManager::applyPendingValues(nullptr, true);
    auto* user = const_cast<RtxOptionLayer*>(RtxOptionLayer::getUserLayer());
    const std::string baseline = contents("rtx.conf");
    require(RtxOptions::graphicsPreset() == GraphicsPreset::Custom, "Fixture must use Custom");

    if (std::string(argv[1]) == "save") {
      {
        RtxOptionLayerTarget target(RtxOptionEditTarget::User);
        RtxOptions::ShadowTerminator::enableOffset.setDeferred(false);
        RtxOptions::ShadowTerminator::soften.setDeferred(false);
        DxvkRtxdiRayQuery::initialSampleCount.setDeferred(7);
        DxvkRtxdiRayQuery::spatialSamples.setDeferred(5);
        DxvkRtxdiRayQuery::disocclusionSamples.setDeferred(4);
      }
      apply();
      require(RtxOptions::ShadowTerminator::enableOffsetObject().hasValueInLayer(user), "Developer option routed outside user.conf");
      verifySavedValues();
      require(user->hasUnsavedChanges(), "Edited settings not dirty");
      require(user->save(), "Save failed");
      require(!user->hasUnsavedChanges(), "Successful save still dirty");
      require(contents("user.conf").find("rtx.shadowTerminator.enableOffset = False") != std::string::npos, "False missing from file");
      require(user->reload(), "Reload failed");
      apply();
      verifySavedValues();
    } else {
      verifySavedValues();
      // This is the startup lighting call made by the Custom graphics preset.
      RtxOptions::updateLightingSetting(true);
      apply();
      verifySavedValues();
      require(user->countMiscategorizedOptions() == 0, "Valid preferences marked misplaced");
      require(user->migrateMiscategorizedOptions() == 0, "Preferences migrated out of user.conf");

      // Explicit menu preset selection must still replace custom sample counts.
      RtxOptions::updatePathTracerPreset(PathTracerPreset::RayReconstruction);
      apply();
      require(DxvkRtxdiRayQuery::initialSampleCount() == 3, "Explicit preset did not apply");
      require(user->hasUnsavedChanges(), "Preset edit not dirty");

      const std::string saved = contents("user.conf");
      HANDLE lock = CreateFileW(L"user.conf", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      require(lock != INVALID_HANDLE_VALUE, "Could not lock test config");
      const bool savedWhileLocked = user->save();
      CloseHandle(lock);
      require(!savedWhileLocked, "Locked destination reported save success");
      require(user->hasUnsavedChanges(), "Failed save cleared dirty state");
      require(!RtxOptionLayer::getLastSaveError().empty(), "Failed save has no visible error");
      require(contents("user.conf") == saved, "Failed save damaged previous file");
      require(!std::filesystem::exists("user.conf.tmp"), "Failed save left temporary file");
      require(user->save(), "Retry after unlocking failed");
      require(RtxOptionLayer::getLastSaveError().empty(), "Successful retry retained error");
    }
    require(contents("rtx.conf") == baseline, "Save changed shipped defaults");
    std::cout << "PASS: " << argv[1] << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
}
