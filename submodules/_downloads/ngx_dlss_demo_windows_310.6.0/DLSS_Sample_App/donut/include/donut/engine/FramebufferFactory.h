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

#include <nvrhi/nvrhi.h>
#include <vector>
#include <unordered_map>

namespace donut::engine
{
    class IView;

    class FramebufferFactory
    {
    private:
        nvrhi::DeviceHandle m_Device;
        std::unordered_map<nvrhi::TextureSubresourceSet, nvrhi::FramebufferHandle> m_FramebufferCache;

    public:
        FramebufferFactory(nvrhi::IDevice* device) : m_Device(device) {}

        std::vector<nvrhi::TextureHandle> RenderTargets;
        nvrhi::TextureHandle DepthTarget;

        nvrhi::IFramebuffer* GetFramebuffer(const nvrhi::TextureSubresourceSet& subresources);
        nvrhi::IFramebuffer* GetFramebuffer(const IView& view);
    };
}