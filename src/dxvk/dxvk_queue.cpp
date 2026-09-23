/*
* Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include "dxvk_device.h"
#include "dxvk_queue.h"
#include "dxvk_scoped_annotation.h"
#include "../util/util_kenshi_fault.h"

#include "NvLowLatencyVk.h"
#include "GFSDK_Aftermath_GpuCrashDump.h"

namespace dxvk {

  DxvkSubmissionQueue::DxvkSubmissionQueue(DxvkDevice* device)
  : m_device(device),
    m_submitThread([this] () { submitCmdLists(); }),
    m_finishThread([this] () { finishCmdLists(); }) {
    // DX11_V319_FAULT_ADDRESS_NAMING: start tracking GPU virtual address ranges
    // only when a fault could actually be reported. Without VK_EXT_device_fault
    // the registry could never be read, so it would be pure overhead.
    if (m_device->features().extDeviceFault.deviceFault) {
      DxvkGpuAddressRegistry::get().setEnabled(true);
      KENSHI_DIAGNOSTIC_INFO("[device-fault] VK_EXT_device_fault enabled; tracking GPU address ranges "
                           "so a faulting address can be attributed to an allocation.");
    }
  }


  // NV-DXVK start: DX11_V298_DEVICE_FAULT - vkd3d-proton-style device-loss
  // diagnostics. When the GPU faults, VK_ERROR_DEVICE_LOST alone says nothing
  // about WHY; VK_EXT_device_fault returns the fault kind and the faulting
  // GPU virtual addresses, which distinguishes an out-of-bounds read (bad
  // geometry/index data) from a shader instruction-pointer fault (bad
  // pipeline) from a page fault in a freed allocation (lifetime bug).
  void DxvkSubmissionQueue::logDeviceFaultInfo() {
    static std::atomic<bool> s_faultLogged = { false };
    if (s_faultLogged.exchange(true))
      return;

    // Query progress once on loss; never query or retain application fences.
    if (m_device->extensions().nvDeviceDiagnosticCheckpoints) {
      const VkQueue queues[]={m_device->queues().graphics.queueHandle,m_device->queues().transfer.queueHandle,
        m_device->queues().asyncCompute.queueHandle,m_device->queues().present.queueHandle};
      for (size_t q=0;q<4;++q) {
        if (!queues[q]) continue;
        bool duplicate=false;for(size_t j=0;j<q;++j) duplicate|=queues[j]==queues[q];
        if(duplicate) continue;
        std::array<VkCheckpointDataNV,16> checkpoints {};
        for(auto& checkpoint:checkpoints) checkpoint.sType=VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
        uint32_t available=0;
        m_device->vkd()->vkGetQueueCheckpointDataNV(queues[q],&available,nullptr);
        uint32_t count=std::min(available,uint32_t(checkpoints.size()));
        m_device->vkd()->vkGetQueueCheckpointDataNV(queues[q],&count,checkpoints.data());
        for(uint32_t i=0;i<std::min(count,uint32_t(checkpoints.size()));++i) {
          const auto token=reinterpret_cast<uintptr_t>(checkpoints[i].pCheckpointMarker);
          kenshi_fault::add(kenshi_fault::QueueCheckpoint,kenshi_fault::currentFrame.load(),q,checkpoints[i].stage,token,available);
          Logger::err(str::format("[FaultRecord V717] queue=",q," stage=",checkpoints[i].stage," marker=",token," available=",available," recorded=",count));
        }
      }
    }
    kenshi_fault::add(kenshi_fault::DeviceLost,kenshi_fault::currentFrame.load(),kenshi_fault::submitted.load(),kenshi_fault::completed.load());
    kenshi_fault::fault("VK_ERROR_DEVICE_LOST");

    if (!m_device->features().extDeviceFault.deviceFault) {
      Logger::err("[device-fault] VK_EXT_device_fault not supported/enabled; no GPU fault details available.");
      return;
    }

    auto vkd = m_device->vkd();

    VkDeviceFaultCountsEXT counts = {};
    counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    if (vkd->vkGetDeviceFaultInfoEXT(vkd->device(), &counts, nullptr) < VK_SUCCESS) {
      Logger::err("[device-fault] vkGetDeviceFaultInfoEXT (counts) failed.");
      return;
    }

    std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT>  vendorInfos(counts.vendorInfoCount);
    counts.vendorBinarySize = 0; // the opaque vendor blob is not useful in a text log

    VkDeviceFaultInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    info.pAddressInfos     = addressInfos.empty() ? nullptr : addressInfos.data();
    info.pVendorInfos      = vendorInfos.empty()  ? nullptr : vendorInfos.data();
    info.pVendorBinaryData = nullptr;

    if (vkd->vkGetDeviceFaultInfoEXT(vkd->device(), &counts, &info) < VK_SUCCESS) {
      Logger::err("[device-fault] vkGetDeviceFaultInfoEXT (info) failed.");
      return;
    }

    Logger::err(str::format("[device-fault] GPU fault report: '", info.description, "'"));

    static const char* const kAddressTypeNames[] = {
      "none", "read-invalid", "write-invalid", "execute-invalid",
      "instruction-pointer-unknown", "instruction-pointer-invalid",
      "instruction-pointer-fault",
    };
    for (uint32_t i = 0; i < counts.addressInfoCount && i < addressInfos.size(); i++) {
      const auto& address = addressInfos[i];
      const uint32_t type = uint32_t(address.addressType);
      Logger::err(str::format("[device-fault]   address[", i, "]: type=",
        type < 7u ? kAddressTypeNames[type] : "unknown",
        " gpuVA=0x", std::hex, address.reportedAddress, std::dec,
        " precision=", address.addressPrecision));

      // DX11_V319_FAULT_ADDRESS_NAMING: a bare GPU virtual address names
      // nothing and cannot be acted on. Resolve it against the allocations
      // currently reachable by device address; "no live allocation" is itself
      // the answer when the fault is a use-after-free.
      Logger::err(str::format("[device-fault]     -> ",
        DxvkGpuAddressRegistry::get().describe(address.reportedAddress)));
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount && i < vendorInfos.size(); i++) {
      const auto& vendor = vendorInfos[i];
      Logger::err(str::format("[device-fault]   vendor[", i, "]: '", vendor.description,
        "' code=", vendor.vendorFaultCode, " data=", vendor.vendorFaultData));
    }
  }
  // NV-DXVK end
  
  
  DxvkSubmissionQueue::~DxvkSubmissionQueue() {
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }
    
    m_appendCond.notify_all();
    m_submitCond.notify_all();

    m_submitThread.join();
    m_finishThread.join();
    kenshi_fault::close();
  }
  
  
  void DxvkSubmissionQueue::submit(DxvkSubmitInfo submitInfo) {
    ScopedCpuProfileZone();
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_finishCond.wait(lock, [this] {
      return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;
    });

    DxvkSubmitEntry entry = { };
    entry.submit = std::move(submitInfo);

    m_pending += 1;
    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::present(DxvkPresentInfo presentInfo, DxvkSubmitStatus* status) {
    ScopedCpuProfileZone();
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    DxvkSubmitEntry entry = { };
    entry.status  = status;
    entry.present = std::move(presentInfo);
    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


// NV-DXVK begin: DLFG integration
  void DxvkSubmissionQueue::setupFrameInterpolation(DxvkFrameInterpolationInfo frameInterpolationInfo) {
    ZoneScoped;
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    DxvkSubmitEntry entry = { };
    entry.frameInterpolation = std::move(frameInterpolationInfo);
    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }
// NV-DXVK end

  void DxvkSubmissionQueue::synchronizeSubmission(
          DxvkSubmitStatus*   status) {
    ScopedCpuProfileZone();
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    // The submission worker is what publishes this result. If it has exited -
    // which is what happens once the device is lost - nothing will ever set it,
    // and an unconditional wait here parks the game's present thread forever
    // (the "not responding" hang). Let a dead queue break the wait too.
    m_submitCond.wait(lock, [this, status] {
      return status->result.load() != VK_NOT_READY
          || m_stopped.load()
          || m_lastError.load() == VK_ERROR_DEVICE_LOST;
    });

    // Surface the failure to the caller rather than letting a never-completed
    // submission look like it succeeded.
    if (status->result.load() == VK_NOT_READY) {
      const VkResult lastError = m_lastError.load();
      status->result.store(lastError != VK_SUCCESS ? lastError : VK_ERROR_DEVICE_LOST);

      ONCE(Logger::err(
        "DxvkSubmissionQueue: submission never completed (queue stopped or device lost); "
        "failing the wait instead of blocking present forever."));
    }
  }


  void DxvkSubmissionQueue::synchronize() {
    ScopedCpuProfileZone();
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    // Same hazard as synchronizeSubmission: only the worker drains this queue,
    // so a stopped or device-lost queue would never satisfy an unconditional wait.
    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty()
          || m_stopped.load()
          || m_lastError.load() == VK_ERROR_DEVICE_LOST;
    });

    // NV-DXVK start: DLFG integration
    if (m_lastPresenter != nullptr) {
      m_lastPresenter->synchronize();
      m_lastPresenter = nullptr;
    }
    // NV-DXVK end
  }


  void DxvkSubmissionQueue::lockDeviceQueue() {
    ScopedCpuProfileZone();
    m_mutexQueue.lock();
  }


  void DxvkSubmissionQueue::unlockDeviceQueue() {
    ScopedCpuProfileZone();
    m_mutexQueue.unlock();
  }

  void DxvkSubmissionQueue::submitCmdLists() {
    env::setThreadName("dxvk-submit");

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    while (!m_stopped.load()) {
      m_appendCond.wait(lock, [this] {
        return m_stopped.load() || !m_submitQueue.empty();
      });
      
      if (m_stopped.load())
        return;

      ScopedCpuProfileZone();

      DxvkSubmitEntry entry = std::move(m_submitQueue.front());
      lock.unlock();
      
      // Submit command buffer to device
      VkResult status = VK_NOT_READY;

      if (m_lastError != VK_ERROR_DEVICE_LOST) {
        // NV-DXVK start: Rename lock to lockQueue to avoid shadowing other mutex
        std::lock_guard<dxvk::mutex> lockQueue(m_mutexQueue);
        // NV-DXVK end

          // NV-DXVK start: Reflex render submit
        const auto& reflex = m_device->getCommon()->metaReflex();
        // NV-DXVK end

        if (entry.submit.cmdList != nullptr) {
          // When using Reflex with Remix, we need to wrap the queue submit for the injectRTX rendering
          // work with the reflex render_submit markers.  This is because in Remix we essentially
          // have one large cmd list of work (inject rtx) and we want the Reflex timing to prioritize
          // this work for best latency reduction while minimizing performance impact.  So we tag the submit
          // upstream (RtxContext) which contains the injectRTX call as the one we want to wrap with Reflex markers.
          // NV-DXVK start: Reflex render submit
          if (entry.submit.insertReflexRenderMarkers) {
            reflex.beginRendering(entry.submit.cachedReflexFrameId);
          }

          status = entry.submit.cmdList->submit(
            entry.submit.waitSync,
            entry.submit.wakeSync);
          if (kenshi_telemetry::enabled()) {
            const auto serial=++kenshi_fault::submitted;
            kenshi_fault::add(kenshi_fault::Submit,entry.submit.cachedReflexFrameId,
              reinterpret_cast<uintptr_t>(entry.submit.cmdList.ptr()),serial,uint64_t(int64_t(status)));
          }

          if (entry.submit.insertReflexRenderMarkers) {
            reflex.endRendering(entry.submit.cachedReflexFrameId);
          }
          // NV-DXVK end
        }
        // NV-DXVK start: DLFG integration
        else if (entry.frameInterpolation.valid()) {
          // stash frame interpolation data for next present call
          m_currentFrameInterpolationData = entry.frameInterpolation;
        }
        else if (entry.present.presenter != nullptr) {
          m_lastPresenter = entry.present.presenter;

          // NV-DXVK start: Reflex present start
          const auto insertReflexPresentMarkers = entry.present.insertReflexPresentMarkers;
          const auto cachedReflexFrameId = entry.present.cachedReflexFrameId;

          // Note: Only insert Reflex Present markers around the Presenter's present call if requested.
          if (insertReflexPresentMarkers) {
            reflex.beginPresentation(cachedReflexFrameId);
          }
          // NV-DXVK end

          // NV-DXVK start: DLFG acquired image information retrieval
          const auto cachedAcquiredImageIndex = entry.present.cachedAcquiredImageIndex;
          // NV-DXVK end

          // m_device->vkd()->vkQueueWaitIdle(m_device->queues().graphics.queueHandle);
          status = entry.present.presenter->presentImage(&entry.status->result, entry.present, m_currentFrameInterpolationData, cachedAcquiredImageIndex);
          // if both submit and DLFG+present run on the same queue, then we need to wait for present to avoid racing on the queue
#if __DLFG_USE_GRAPHICS_QUEUE
          entry.present.presenter->synchronize();
#endif

          // NV-DXVK start: Reflex present end
          // Note: Only insert Reflex Present markers around the Presenter's present call if requested.
          if (insertReflexPresentMarkers) {
            reflex.endPresentation(cachedReflexFrameId);
          }
          // NV-DXVK end

          m_currentFrameInterpolationData.reset();

          const auto presentThrottleDelay = m_device->config().presentThrottleDelay;

          if (presentThrottleDelay > 0) {
            ScopedCpuProfileZoneN("Present Throttle Delay Sleep");

            Sleep(presentThrottleDelay);
          }
        }
      } else {
        // Don't submit anything after device loss
        // so that drivers get a chance to recover
        status = VK_ERROR_DEVICE_LOST;
      }

      if (entry.status)
        // NV-DXVK start: DLFG integration
        // if we queued for interpolation, then don't touch the output status here; DLFG presenter thread will update it (and may have already done so)
        if (status != VK_EVENT_SET) {
          entry.status->result = status;
        }
        // NV-DXVK end

      // On success, pass it on to the queue thread
      lock = std::unique_lock<dxvk::mutex>(m_mutex);

      bool needsWaitForIdle = false;

      if (status == VK_SUCCESS) {
        if (entry.submit.cmdList != nullptr)
          m_finishQueue.push(std::move(entry));
      } else if (status == VK_ERROR_DEVICE_LOST || entry.submit.cmdList != nullptr) {
        Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", status));
        m_lastError = status;

        // VK_ERROR_DEVICE_LOST on its own says nothing about what faulted.
        // VK_EXT_device_fault is enabled, so ask the driver for the fault
        // addresses and vendor detail while the device is still queryable -
        // this has to happen before anything tears the device down.
        if (status == VK_ERROR_DEVICE_LOST)
          logDeviceFaultInfo();

        if (m_device->config().enableAftermath) {
          // Stall the pending exception until aftermath has finished writing (or hits some error)
          uint32_t counter = 0;
          GFSDK_Aftermath_CrashDump_Status aftermathStatus = GFSDK_Aftermath_CrashDump_Status_NotStarted; 
          
          static const uint32_t kTimeoutPreventionLimit = 5000;
          
          while (counter < kTimeoutPreventionLimit) {
            GFSDK_Aftermath_GetCrashDumpStatus(&aftermathStatus);

            if (aftermathStatus == GFSDK_Aftermath_CrashDump_Status_Finished || aftermathStatus == GFSDK_Aftermath_CrashDump_Status_Unknown)
              break; // Our dump was written

            static const uint32_t kTimeoutPerTry = 100;
            Sleep(kTimeoutPerTry);
            counter += kTimeoutPerTry;
          }
        }
        // Deliberately NOT waiting for idle here - see below.
        needsWaitForIdle = true;
      }

      // Retire the entry before any wait-for-idle. waitForIdle() goes through
      // lockSubmission() -> synchronize(), which blocks until m_submitQueue is
      // empty; this worker is the only thing that drains that queue, so waiting
      // while the just-failed entry is still queued deadlocks the worker against
      // itself and takes the render thread down with it.
      m_submitQueue.pop();
      m_submitCond.notify_all();

      if (needsWaitForIdle) {
        // Drop the queue lock across the wait: synchronize() acquires it, and
        // the loop below expects to re-enter holding it.
        lock.unlock();
        m_device->waitForIdle();
        lock.lock();
      }
    }
  }
  
  
  void DxvkSubmissionQueue::finishCmdLists() {
    env::setThreadName("dxvk-queue");

    while (!m_stopped.load()) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);

      if (m_finishQueue.empty()) {
        auto t0 = dxvk::high_resolution_clock::now();

        m_submitCond.wait(lock, [this] {
          return m_stopped.load() || !m_finishQueue.empty();
        });

        auto t1 = dxvk::high_resolution_clock::now();
        m_gpuIdle += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      }

      if (m_stopped.load())
        return;

      ScopedCpuProfileZone();
      
      DxvkSubmitEntry entry = std::move(m_finishQueue.front());
      lock.unlock();
      
      VkResult status = m_lastError.load();
      
      if (status != VK_ERROR_DEVICE_LOST)
        status = entry.submit.cmdList->synchronize();
      
      if (status != VK_SUCCESS) {
        Logger::err(str::format("DxvkSubmissionQueue: Failed to sync fence: ", status));
        m_lastError = status;
        if (status==VK_ERROR_DEVICE_LOST) logDeviceFaultInfo();
        m_device->waitForIdle();
      }
      if (kenshi_telemetry::enabled() && status==VK_SUCCESS) {
        const auto serial=++kenshi_fault::completed;
        kenshi_fault::add(kenshi_fault::Complete,entry.submit.cachedReflexFrameId,
          reinterpret_cast<uintptr_t>(entry.submit.cmdList.ptr()),serial);
      }

      // Release resources and signal events, then immediately wake
      // up any thread that's currently waiting on a resource in
      // order to reduce delays as much as possible.
      entry.submit.cmdList->notifyObjects();

      lock.lock();
      m_pending -= 1;

      m_finishQueue.pop();
      m_finishCond.notify_all();
      lock.unlock();

      // Free the command list and associated objects now
      entry.submit.cmdList->reset();
      m_device->recycleCommandList(entry.submit.cmdList);
    }
  }
}
