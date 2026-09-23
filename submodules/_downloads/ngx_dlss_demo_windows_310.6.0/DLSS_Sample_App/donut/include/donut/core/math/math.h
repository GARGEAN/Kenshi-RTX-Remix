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

namespace donut::math {}
namespace dm = donut::math;

#include "basics.h"
#include "vector.h"
#include "matrix.h"
#include "affine.h"
#if defined(_M_AMD64) && !defined(_M_ARM64EC)
#include "simd.h"
#endif
#include "box.h"
#include "color.h"
#include "quat.h"
#include "sphere.h"
#include "frustum.h"
