/*
 * SPDX-FileCopyrightText: Copyright (c) 2019-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#include <donut/render/plugins/DlssPass.h>
#include <donut/engine/View.h>
#include <donut/core/log.h>
#include <assert.h>

#define NGX_ENABLE_DEPRECATED_SHUTDOWN
#include <nvsdk_ngx.h>
#if USE_DX11
#include <d3d11.h>
#endif
#if USE_DX12
#include <d3d12.h>
#include <nvrhi/d3d12/d3d12.h>
#endif

using namespace donut::math;
using namespace donut::engine;
using namespace donut::render;

DlssPass::DlssPass(nvrhi::IDevice* device, int2 maxRenderSize)
    : m_Device(device)
{
#if USE_DX11
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
    {
        ID3D11Device* d3ddevice = device->getNativeObject(nvrhi::ObjectTypes::D3D11_Device);

        NVSDK_NGX_Result Result;
        Result = NVSDK_NGX_D3D11_Init(0, L"./", d3ddevice);

        if (Result == NVSDK_NGX_Result_Success)
        {
            m_NgxInitialized = true;

            NVSDK_NGX_D3D11_AllocateParameters(&m_NgxParameters);

            m_NgxParameters->Set("Width", maxRenderSize.x);
            m_NgxParameters->Set("Height", maxRenderSize.y);

            size_t scratchBufferSize = 0;
            Result = NVSDK_NGX_D3D11_GetScratchBufferSize(NVSDK_NGX_Feature_SuperSampling, m_NgxParameters, &scratchBufferSize);

            if (scratchBufferSize > 0)
            {
				D3D11_BUFFER_DESC d3dBufferDesc = {};
				d3dBufferDesc.ByteWidth = static_cast<uint32_t>(scratchBufferSize);
				d3dBufferDesc.Usage = D3D11_USAGE_STAGING;
				d3dBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
				d3dBufferDesc.BindFlags = 0;

                nvrhi::RefCountPtr<ID3D11Buffer> buffer;
                d3ddevice->CreateBuffer(&d3dBufferDesc, nullptr, &buffer);

                m_NgxScratchBuffer = device->createHandleForNativeBuffer(nvrhi::ObjectTypes::D3D11_Buffer, nvrhi::Object(buffer), nvrhi::BufferDesc());

                m_NgxParameters->Set("Scratch", buffer);
                m_NgxParameters->Set("Scratch.SizeInBytes", scratchBufferSize);
            }

            m_NgxParameters->Set("Hint.HDR", 1);
            m_NgxParameters->Set("Hint.UseFolding", 1);

            Result = NVSDK_NGX_D3D11_CreateFeature(device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext), NVSDK_NGX_Feature_SuperSampling, m_NgxParameters, &m_NgxFeature);

            if (Result != NVSDK_NGX_Result_Success)
            {
                NVSDK_NGX_D3D11_Shutdown1(nullptr);
                m_NgxInitialized = false;

                log::error("Failed to create NGX anti-aliasing feature, error code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
            }
        }
        else
        {
            log::error("Failed to initialize NGX, error code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
        }

        return;
    }
#endif

#if USE_DX12
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        ID3D12Device* d3ddevice = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);

        NVSDK_NGX_Result Result;
        Result = NVSDK_NGX_D3D12_Init(0, L"./", d3ddevice);

        if (Result == NVSDK_NGX_Result_Success)
        {
            m_NgxInitialized = true;

            NVSDK_NGX_D3D12_AllocateParameters(&m_NgxParameters);

            m_NgxParameters->Set("Width", maxRenderSize.x);
            m_NgxParameters->Set("Height", maxRenderSize.y);

            size_t scratchBufferSize = 0;
            Result = NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature_SuperSampling, m_NgxParameters, &scratchBufferSize);

            if (scratchBufferSize > 0)
            {
				D3D12_RESOURCE_DESC d3dBufferDesc = {};
				d3dBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				d3dBufferDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
				d3dBufferDesc.Width = static_cast<uint32_t>(scratchBufferSize);
				d3dBufferDesc.Height = 1;
				d3dBufferDesc.DepthOrArraySize = 1;
				d3dBufferDesc.MipLevels = 1;
				d3dBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
				d3dBufferDesc.SampleDesc.Count = 1;
				d3dBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

				D3D12_HEAP_PROPERTIES heapProps = {};
				heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
				heapProps.CreationNodeMask = 1;
				heapProps.VisibleNodeMask = 1;

                nvrhi::RefCountPtr<ID3D12Resource> buffer;
                d3ddevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &d3dBufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));

                m_NgxScratchBuffer = device->createHandleForNativeBuffer(nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(buffer), nvrhi::BufferDesc());

                m_NgxParameters->Set("Scratch", buffer);
                m_NgxParameters->Set("Scratch.SizeInBytes", scratchBufferSize);
            }

            m_NgxParameters->Set("Hint.HDR", 1);
            m_NgxParameters->Set("Hint.UseFolding", 1);

            nvrhi::CommandListHandle commandList = device->createCommandList();
            commandList->open();

            Result = NVSDK_NGX_D3D12_CreateFeature(commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList), NVSDK_NGX_Feature_SuperSampling, m_NgxParameters, &m_NgxFeature);

            commandList->close();

            if (Result != NVSDK_NGX_Result_Success)
            {
                NVSDK_NGX_D3D12_Shutdown1(nullptr);
                m_NgxInitialized = false;

                log::error("Failed to create NGX anti-aliasing feature, error code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
            }
            else
            {
                device->executeCommandList(commandList);
            }
        }
        else
        {
            log::error("Failed to initialize NGX, error code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
        }
        return;
    }
#endif
}


DlssPass::~DlssPass()
{
    if (m_NgxInitialized)
    {
        m_Device->waitForIdle();

#if USE_DX11
        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
        {
            NVSDK_NGX_D3D11_DestroyParameters(m_NgxParameters);
            NVSDK_NGX_D3D11_Shutdown1(nullptr);
        }
#endif
#if USE_DX12
        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
        {
            NVSDK_NGX_D3D12_DestroyParameters(m_NgxParameters);
            NVSDK_NGX_D3D12_Shutdown1(nullptr);
        }
#endif
        m_NgxInitialized = false;
    }
}

void DlssPass::TemporalResolve(nvrhi::ICommandList* commandList, nvrhi::ITexture* unresolvedColor, nvrhi::ITexture* resolvedColor, nvrhi::ITexture* motionVectors, const ICompositeView& compositeView)
{
    if (!m_NgxInitialized)
        return;

    assert(compositeView.GetNumChildViews(ViewType::PLANAR) == 1); // DLSS is simple

    commandList->beginMarker("DLSS");

    for (uint viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

        nvrhi::ViewportState viewportState = view->GetViewportState();

        for (uint viewportIndex = 0; viewportIndex < viewportState.scissorRects.size(); viewportIndex++)
        {
            const nvrhi::Rect& scissorRect = viewportState.scissorRects[viewportIndex];

            m_NgxParameters->Set("Rect.X", scissorRect.minX);
            m_NgxParameters->Set("Rect.Y", scissorRect.minY);
            m_NgxParameters->Set("Rect.W", scissorRect.maxX - scissorRect.minX);
            m_NgxParameters->Set("Rect.H", scissorRect.maxY - scissorRect.minY);
            m_NgxParameters->Set("BlendFactor", 1.0f);

#if USE_DX11
            if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
            {
                m_NgxParameters->Set("Color", unresolvedColor->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource).pointer);
                m_NgxParameters->Set("Output", resolvedColor->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource).pointer);
                m_NgxParameters->Set("MotionVectors", motionVectors->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource).pointer);

                ID3D11DeviceContext* d3dcontext = m_Device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);

                NVSDK_NGX_Result Result;
                Result = NVSDK_NGX_D3D11_EvaluateFeature(d3dcontext, m_NgxFeature, m_NgxParameters);

                assert(Result == NVSDK_NGX_Result_Success);
            }
#endif
#if USE_DX12
            if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
            {
                commandList->endTrackingTextureState(unresolvedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::SHADER_RESOURCE);
                commandList->endTrackingTextureState(resolvedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UNORDERED_ACCESS);
                commandList->endTrackingTextureState(motionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::SHADER_RESOURCE);

                m_NgxParameters->Set("Color", unresolvedColor->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
                m_NgxParameters->Set("Output", resolvedColor->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
                m_NgxParameters->Set("MotionVectors", motionVectors->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);

                ID3D12GraphicsCommandList* d3dcommandList = commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);

                NVSDK_NGX_Result Result;
                Result = NVSDK_NGX_D3D12_EvaluateFeature(d3dcommandList, m_NgxFeature, m_NgxParameters);

                assert(Result == NVSDK_NGX_Result_Success);

                commandList->clearState();
            }
#endif
        }
    }

    commandList->endMarker();
}
