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

#include <donut/render/DrawStrategy.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/View.h>

using namespace donut::math;
using namespace donut::engine;
using namespace donut::render;


InstancedOpaqueDrawStrategy::InstancedOpaqueDrawStrategy(IMeshSet& meshSet)
{
    const std::vector<MeshInstance*>& sourceInstances = meshSet.GetMeshInstances();

    for (MeshInstance* instance : sourceInstances)
    {
        if (instance->mesh->material->domain != MD_TRANSPARENT)
        {
            m_MeshInstances.push_back(instance);
        }
    }

    auto compareMeshInstances = [](const MeshInstance* a, const MeshInstance* b)
    {
        if (a->mesh->buffers != b->mesh->buffers)
            return a->mesh->buffers < b->mesh->buffers;
        else if (a->mesh->material != b->mesh->material)
            return a->mesh->material < b->mesh->material;
        else if (a->mesh != b->mesh)
            return a->mesh < b->mesh;
        else
            return a->instanceOffset < b->instanceOffset;
    };

    std::sort(m_MeshInstances.begin(), m_MeshInstances.end(), compareMeshInstances);
}

void donut::render::InstancedOpaqueDrawStrategy::PrepareForView(const engine::IView* view, std::vector<const MeshInstance*>& instancesToDraw)
{
    instancesToDraw.resize(0);
    instancesToDraw.reserve(m_MeshInstances.size());

    for (const MeshInstance* instance : m_MeshInstances)
    {
        if (view->IsMeshVisible(instance->transformedBounds))
        {
            instancesToDraw.push_back(instance);
        }
    }
}


TransparentDrawStrategy::TransparentDrawStrategy(IMeshSet& meshSet)
{
    const std::vector<MeshInstance*>& sourceInstances = meshSet.GetMeshInstances();

    for (MeshInstance* instance : sourceInstances)
    {
        if (instance->mesh->material->domain == MD_TRANSPARENT)
        {
            m_MeshInstances.push_back(instance);
        }
    }
}

void TransparentDrawStrategy::PrepareForView(const IView* view, std::vector<const MeshInstance*>& instancesToDraw)
{
    instancesToDraw.resize(0);
    for (const MeshInstance* instance : m_MeshInstances)
    {
        if (view->IsMeshVisible(instance->transformedBounds))
        {
            instancesToDraw.push_back(instance);
        }
    }

    float3 viewOrigin = view->GetViewOrigin();

    auto compareMeshInstances = [viewOrigin](const MeshInstance* a, const MeshInstance* b)
    {
        float a_distance = lengthSquared(a->transformedCenter - viewOrigin);
        float b_distance = lengthSquared(b->transformedCenter - viewOrigin);

        return a_distance > b_distance;
    };

    std::sort(instancesToDraw.begin(), instancesToDraw.end(), compareMeshInstances);
}
