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
#include <chrono>

namespace donut::app
{
    class HiResTimer
    {
    public:

        std::chrono::time_point<std::chrono::high_resolution_clock> startTime;
        std::chrono::time_point<std::chrono::high_resolution_clock> stopTime;

        HiResTimer()
        {
        }

        void Start()
        {
            startTime = std::chrono::high_resolution_clock::now();
        }

        void Stop()
        {
            stopTime = std::chrono::high_resolution_clock::now();
        }

        double Seconds()
        {
            auto duration = stopTime - startTime;
            return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() * 1e-9;
        }

        double Milliseconds()
        {
            return Seconds() * 1e3;
        }
    };
}