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
#include <memory>

namespace donut::vfs
{
    class IBlob;
}

namespace donut::engine
{
    struct TextureData;

    // Initialized the TextureInfo from the 'data' array, which must be populated with DDS data
    bool LoadDDSTextureFromMemory(TextureData& textureInfo);

    // Creates a texture based on DDS data in memory
    nvrhi::TextureHandle CreateDDSTextureFromMemory(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, std::shared_ptr<vfs::IBlob> data, const char* debugName = nullptr, bool forceSRGB = false);

    std::shared_ptr<vfs::IBlob> SaveStagingTextureAsDDS(nvrhi::IDevice* device, nvrhi::IStagingTexture* stagingTexture);
}