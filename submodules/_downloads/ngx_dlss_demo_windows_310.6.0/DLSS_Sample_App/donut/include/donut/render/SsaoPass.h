/*
 * SPDX-FileCopyrightText: Copyright (c) 2018-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
    class ICompositeView;
}


namespace donut::render
{
    struct SsaoParameters
    {
        float amount = 2.f;
        float backgroundViewDepth = 100.f;
        float radiusWorld = 0.5f;
        float surfaceBias = 0.1f;
        float powerExponent = 2.f;
        bool enableBlur = true;
        float blurSharpness = 16.f;
    };

    class SsaoPass
    {
    private:
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::BufferHandle m_SsaoCB;

        nvrhi::BindingLayoutHandle m_RenderBindingLayout;
        nvrhi::BindingSetHandle m_RenderBindingSet;
        nvrhi::GraphicsPipelineHandle m_RenderPso;

        std::shared_ptr<engine::CommonRenderPasses> m_CommonPasses;
        std::shared_ptr<engine::FramebufferFactory> m_FramebufferFactory;

    public:
        SsaoPass(
            nvrhi::IDevice* device,
            std::shared_ptr<engine::ShaderFactory> shaderFactory,
            std::shared_ptr<engine::CommonRenderPasses> commonPasses,
            std::shared_ptr<engine::FramebufferFactory> framebufferFactory,
            nvrhi::ITexture* gbufferDepth,
            nvrhi::ITexture* gbufferNormals,
            const engine::ICompositeView& compositeView,
            bool useMultiplicativeBlending);

        void Render(
            nvrhi::ICommandList* commandList,
            const SsaoParameters& params,
            const engine::ICompositeView& compositeView,
            dm::float2 randomOffset);
    };
}