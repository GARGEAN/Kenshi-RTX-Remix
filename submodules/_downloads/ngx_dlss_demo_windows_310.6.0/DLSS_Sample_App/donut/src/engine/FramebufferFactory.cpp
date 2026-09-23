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
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/View.h>

using namespace donut::engine;

nvrhi::IFramebuffer* FramebufferFactory::GetFramebuffer(const nvrhi::TextureSubresourceSet& subresources)
{
    nvrhi::FramebufferHandle& item = m_FramebufferCache[subresources];

    if (!item)
    {
        nvrhi::FramebufferDesc desc;
        for (auto renderTarget : RenderTargets)
            desc.addColorAttachment(renderTarget, subresources);

        if (DepthTarget)
            desc.setDepthAttachment(DepthTarget, subresources);

        item = m_Device->createFramebuffer(desc);
    }
    
    return item;
}

nvrhi::IFramebuffer* FramebufferFactory::GetFramebuffer(const IView& view)
{
    return GetFramebuffer(view.GetSubresources());
}
