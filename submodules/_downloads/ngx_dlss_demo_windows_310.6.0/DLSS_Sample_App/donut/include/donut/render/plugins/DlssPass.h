/*
 * SPDX-FileCopyrightText: Copyright (c) 2019-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */
#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>
#include <memory>

namespace donut::engine
{
    class ShaderFactory;
    class ShadowMap;
    class CommonRenderPasses;
    class FramebufferFactory;
    class IDrawStrategy;
    class ICompositeView;
}

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace donut::render
{
    class DlssPass
    {
    private:
        nvrhi::DeviceHandle m_Device;

        bool m_NgxInitialized = false;
        NVSDK_NGX_Parameter* m_NgxParameters = nullptr;
        NVSDK_NGX_Handle* m_NgxFeature = nullptr;
        nvrhi::BufferHandle m_NgxScratchBuffer;

    public:
        DlssPass(
            nvrhi::IDevice* device,
            dm::int2 maxRenderSize);

        ~DlssPass();

        bool IsInitialized() const { return m_NgxInitialized; }

        void TemporalResolve(
            nvrhi::ICommandList* commandList,
            nvrhi::ITexture* unresolvedColor,
            nvrhi::ITexture* resolvedColor,
            nvrhi::ITexture* motionVectors,
            const engine::ICompositeView& compositeView);

    };
}