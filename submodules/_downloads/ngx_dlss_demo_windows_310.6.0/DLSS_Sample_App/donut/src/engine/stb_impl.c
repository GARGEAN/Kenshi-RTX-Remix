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

#define _CRT_SECURE_NO_WARNINGS

// this would be a better solution instead of _CRT_SECURE_NO_WARNINGS, but then stb has some other warnings about sprintf_s
// #define STBI_MSC_SECURE_CRT

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_M_ARM64EC) || defined(_ARM64EC_) || defined(ARM64EC)
#define STBI_NO_SIMD
#endif

#include "stb_image.h"
#include "stb_image_write.h"
