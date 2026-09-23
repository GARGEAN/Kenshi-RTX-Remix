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

#ifndef SKY_CB_H
#define SKY_CB_H

struct SkyConstants
{
    float4x4    matClipToTranslatedWorld;

    float4      directionToSun;

    float2      padding;
    float       lightIntensity;
    float       angularSizeOfLight;
};

#endif // SKY_CB_H