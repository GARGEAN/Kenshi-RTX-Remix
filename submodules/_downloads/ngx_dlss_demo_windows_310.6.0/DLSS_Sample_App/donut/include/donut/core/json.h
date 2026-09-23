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
#include <filesystem>
#include <string>

namespace Json
{
    class Value;
}

namespace donut::vfs
{
    class IFileSystem;
}

namespace donut::json
{
    bool LoadFromFile(vfs::IFileSystem& fs, const std::filesystem::path& jsonFileName, Json::Value& documentRoot);

    template<typename T> T Read(const Json::Value& node, const T& defaultValue);

    template<> std::string Read<std::string>(const Json::Value& node, const std::string& defaultValue);
    template<> int Read<int>(const Json::Value& node, const int& defaultValue);
    template<> bool Read<bool>(const Json::Value& node, const bool& defaultValue);
    template<> float Read<float>(const Json::Value& node, const float& defaultValue);
    template<> dm::float2 Read<dm::float2>(const Json::Value& node, const dm::float2& defaultValue);
    template<> dm::float3 Read<dm::float3>(const Json::Value& node, const dm::float3& defaultValue);
}
