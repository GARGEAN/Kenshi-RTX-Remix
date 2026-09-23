#pragma once

namespace dxvk {

  // Installs the Kenshi-specific OGRE InstanceBatchHW culling correction.
  bool RemixInstallKenshiInstanceBatchCullingPatch();
  void RemixUpdateKenshiTerrainCulling(bool enabled, float radius);

}
