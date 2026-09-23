#pragma once

#include <atomic>
#include <condition_variable>
#include <fstream>

#include "dxvk_include.h"

#include "../util/sha1/sha1_util.h"
#include "../util/util_env.h"
#include "../util/util_time.h"
#include "../util/thread.h"

namespace dxvk {

  /**
   * \brief Pipeline cache
   *
   * Allows the Vulkan implementation to
   * re-use previously compiled pipelines.
   */
  class DxvkPipelineCache : public RcObject {

  public:

    DxvkPipelineCache(const Rc<vk::DeviceFn>& vkd);
    ~DxvkPipelineCache();

    /**
     * \brief Pipeline cache handle
     * \returns Pipeline cache handle
     */
    VkPipelineCache handle() const {
      return m_handle;
    }

    /**
     * \brief Serializes the pipeline cache to disk
     *
     * DX11_V298_PERSISTENT_PIPELINE_CACHE: writes the driver's serialized
     * pipeline blob to rtx-remix/cache beside the game executable so the
     * next launch seeds vkCreatePipelineCache with it and the boot prewarm
     * reuses compiled pipelines instead of rebuilding them from scratch.
     * Safe to call while compiles are in flight (a default VkPipelineCache
     * is internally synchronized); no-op when nothing new was compiled.
     */
    void save();

  private:

    Rc<vk::DeviceFn>        m_vkd;
    VkPipelineCache         m_handle;

    dxvk::mutex             m_saveMutex;
    size_t                  m_lastSavedSize = 0;

  };

}
