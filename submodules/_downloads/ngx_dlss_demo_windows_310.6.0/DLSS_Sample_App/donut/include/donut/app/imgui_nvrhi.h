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

#include <memory>
#include <vector>
#include <stdint.h>

#include <nvrhi/nvrhi.h>

#include <imgui.h>

namespace donut::engine
{
    class ShaderFactory;
}

namespace donut::app
{
    struct ImGui_NVRHI
    {
        nvrhi::DeviceHandle renderer = nullptr;
        nvrhi::CommandListHandle m_commandList;

        nvrhi::ShaderHandle vertexShader;
        nvrhi::ShaderHandle pixelShader;
        nvrhi::InputLayoutHandle shaderAttribLayout;

        // the constant buffer data depends only on the display size
        // cache the display size we updated the buffer with to avoid touching it every frame
        ImVec2 lastDisplaySize = ImVec2(-1.f, -1.f);
        nvrhi::BufferHandle constantBuffer;

        nvrhi::TextureHandle fontTexture;
        nvrhi::SamplerHandle fontSampler;

        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;

        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::GraphicsPipelineDesc basePSODesc;

        nvrhi::GraphicsPipelineHandle pso;
        nvrhi::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> bindingsCache;

        std::vector<ImDrawVert> vtxBuffer;
        std::vector<ImDrawIdx> idxBuffer;

        bool init(nvrhi::DeviceHandle renderer, std::shared_ptr<engine::ShaderFactory> shaderFactory);
        bool beginFrame(float elapsedTimeSeconds);
        bool render(nvrhi::IFramebuffer* framebuffer);
        void backbufferResizing();

    private:
        bool reallocateBuffer(nvrhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, bool isIndexBuffer);

        bool createFontTexture(nvrhi::ICommandList* commandList);

        nvrhi::IGraphicsPipeline* getPSO(nvrhi::IFramebuffer* fb);
        nvrhi::IBindingSet* getBindingSet(nvrhi::ITexture* texture);
        bool updateGeometry(nvrhi::ICommandList* commandList);
        void updateConstantBuffer(nvrhi::ICommandList* commandList);
    };
}