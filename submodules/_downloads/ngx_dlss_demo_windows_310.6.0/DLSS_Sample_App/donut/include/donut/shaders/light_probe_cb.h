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

#ifndef LIGHT_PROBE_CB_H
#define LIGHT_PROBE_CB_H

struct LightProbeConstants
{
    uint sampleCount;
    float lodBias;
    float roughness;
    float inputCubeSize;
};

#endif // LIGHT_PROBE_CB_H