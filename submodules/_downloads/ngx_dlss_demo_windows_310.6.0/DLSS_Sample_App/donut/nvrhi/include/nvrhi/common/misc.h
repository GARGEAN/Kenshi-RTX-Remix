/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/
#pragma once

namespace nvrhi 
{
    template<typename T> T align(T size, uint32_t alignment)
    {
        return (size + alignment - 1) & ~(T(alignment) - 1);
    }

    template<typename T, typename U> bool arraysAreDifferent(const T& a, const U& b)
    {
        if (a.size() != b.size())
            return true;

        for (uint32_t i = 0; i < uint32_t(a.size()); i++)
        {
            if (a[i] != b[i])
                return true;
        }

        return false;
    }
} // namespace nvrhi
