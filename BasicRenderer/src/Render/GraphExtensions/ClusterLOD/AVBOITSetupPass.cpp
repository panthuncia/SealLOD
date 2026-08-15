#include "Render/GraphExtensions/ClusterLOD/AVBOITSetupPass.h"

#include "Managers/ViewManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Resources/GloballyIndexedResource.h"

AVBOITSetupPass::AVBOITSetupPass(
    std::shared_ptr<Buffer> configBuffer,
    std::shared_ptr<Buffer> fitStateBuffer,
    std::shared_ptr<Buffer> depthWarpLUTBuffer,
    std::shared_ptr<PixelBuffer> occupancyTexture,
    std::shared_ptr<PixelBuffer> coverageTexture,
    std::shared_ptr<PixelBuffer> occupancySliceMaskTexture,
    std::shared_ptr<PixelBuffer> scalarExtinctionTexture,
    std::shared_ptr<PixelBuffer> chromaticExtinctionTexture,
    std::shared_ptr<PixelBuffer> integratedTransmittanceTexture,
    std::shared_ptr<PixelBuffer> zeroTransmittanceSliceTexture,
    std::shared_ptr<PixelBuffer> accumulationTexture,
    std::shared_ptr<PixelBuffer> normalizationTexture,
    std::shared_ptr<PixelBuffer> shadingExtinctionTexture)
    : m_configBuffer(std::move(configBuffer))
    , m_fitStateBuffer(std::move(fitStateBuffer))
    , m_depthWarpLUTBuffer(std::move(depthWarpLUTBuffer))
    , m_occupancyTexture(std::move(occupancyTexture))
    , m_coverageTexture(std::move(coverageTexture))
    , m_occupancySliceMaskTexture(std::move(occupancySliceMaskTexture))
    , m_scalarExtinctionTexture(std::move(scalarExtinctionTexture))
    , m_chromaticExtinctionTexture(std::move(chromaticExtinctionTexture))
    , m_integratedTransmittanceTexture(std::move(integratedTransmittanceTexture))
    , m_zeroTransmittanceSliceTexture(std::move(zeroTransmittanceSliceTexture))
    , m_accumulationTexture(std::move(accumulationTexture))
    , m_normalizationTexture(std::move(normalizationTexture))
    , m_shadingExtinctionTexture(std::move(shadingExtinctionTexture))
{
}

void AVBOITSetupPass::DeclareResourceUsages(RenderPassBuilder* builder)
{
    if (m_configBuffer) {
        builder->WithShaderResource(m_configBuffer);
    }
    if (m_fitStateBuffer) {
        builder->WithShaderResource(m_fitStateBuffer);
    }
    if (m_depthWarpLUTBuffer) {
        builder->WithShaderResource(m_depthWarpLUTBuffer);
    }

    builder->WithUnorderedAccessClear(
        m_occupancyTexture,
        m_coverageTexture,
        m_occupancySliceMaskTexture,
        m_integratedTransmittanceTexture,
        m_zeroTransmittanceSliceTexture);

    builder->WithUnorderedAccess(
        m_scalarExtinctionTexture,
        m_chromaticExtinctionTexture);

    if (m_accumulationTexture) {
        builder->WithRenderTargetClear(m_accumulationTexture);
    }
    if (m_normalizationTexture) {
        builder->WithRenderTargetClear(m_normalizationTexture);
    }
    if (m_shadingExtinctionTexture) {
        builder->WithRenderTargetClear(m_shadingExtinctionTexture);
    }
}

void AVBOITSetupPass::Setup()
{
}

void AVBOITSetupPass::Update(const UpdateExecutionContext& executionContext)
{
    if (!m_configBuffer) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;

    m_configBuffer->EnsureVirtualDescriptorSlotsAllocated();
    m_occupancyTexture->EnsureVirtualDescriptorSlotsAllocated();
	m_coverageTexture->EnsureVirtualDescriptorSlotsAllocated();
    m_occupancySliceMaskTexture->EnsureVirtualDescriptorSlotsAllocated();
    if (m_fitStateBuffer) {
        m_fitStateBuffer->EnsureVirtualDescriptorSlotsAllocated();
    }
    if (m_depthWarpLUTBuffer) {
        m_depthWarpLUTBuffer->EnsureVirtualDescriptorSlotsAllocated();
    }
    m_scalarExtinctionTexture->EnsureVirtualDescriptorSlotsAllocated();
    m_chromaticExtinctionTexture->EnsureVirtualDescriptorSlotsAllocated();
	m_integratedTransmittanceTexture->EnsureVirtualDescriptorSlotsAllocated();
    m_zeroTransmittanceSliceTexture->EnsureVirtualDescriptorSlotsAllocated();

    CLodAVBOITConfig config{};
    config.occupancyUAVDescriptorIndex = m_occupancyTexture
        ? m_occupancyTexture->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    config.coverageUAVDescriptorIndex = m_coverageTexture
        ? m_coverageTexture->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    config.occupancySliceMaskUAVDescriptorIndex = m_occupancySliceMaskTexture
        ? m_occupancySliceMaskTexture->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    config.depthWarpLUTSRVDescriptorIndex = m_depthWarpLUTBuffer
        ? m_depthWarpLUTBuffer->GetSRVInfo(0).slot.index
        : 0xFFFFFFFFu;
    config.scalarExtinctionUAVDescriptorIndex = m_scalarExtinctionTexture
        ? m_scalarExtinctionTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index
        : 0xFFFFFFFFu;
    config.chromaticExtinctionUAVDescriptorIndex = m_chromaticExtinctionTexture
        ? m_chromaticExtinctionTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index
        : 0xFFFFFFFFu;
    config.integratedTransmittanceUAVDescriptorIndex = m_integratedTransmittanceTexture
        ? m_integratedTransmittanceTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index
        : 0xFFFFFFFFu;
    config.shadingTransmittanceSRVDescriptorIndex = m_integratedTransmittanceTexture
        ? m_integratedTransmittanceTexture->GetSRVInfo(SRVViewType::Texture2DArrayFull, 0).slot.index
        : 0xFFFFFFFFu;
    config.zeroTransmittanceSliceUAVDescriptorIndex = m_zeroTransmittanceSliceTexture
        ? m_zeroTransmittanceSliceTexture->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    config.sliceCount = CLodAVBOITDefaultSliceCount;
    config.virtualSliceCount = CLodAVBOITDefaultVirtualSliceCount;
    config.lowResolutionWidth = m_occupancyTexture ? m_occupancyTexture->GetWidth() : 0u;
    config.lowResolutionHeight = m_occupancyTexture ? m_occupancyTexture->GetHeight() : 0u;
    config.depthDistributionExponent = CLodAVBOITDefaultDepthDistributionExponent;
    config.lookupDepthBiasInSlices = CLodAVBOITDefaultLookupDepthBiasInSlices;
    config.zeroTransmittanceThreshold = CLodAVBOITDefaultZeroTransmittanceThreshold;

    if (context.viewManager) {
        context.viewManager->ForEachFiltered(ViewFilter::PrimaryCameras(), [&](uint64_t viewID) {
            const View* view = context.viewManager->Get(viewID);
            if (!view) {
                return;
            }

            config.viewNearDepth = view->cameraInfo.zNear;
            config.viewFarDepth = view->cameraInfo.zFar;
        });
    }

    BUFFER_UPLOAD(&config, sizeof(CLodAVBOITConfig), org::runtime::UploadTarget::FromShared(m_configBuffer), 0);

    if (m_fitStateBuffer && !m_fitStateInitialized) {
        const CLodAVBOITFitState fitState{};
        BUFFER_UPLOAD(&fitState, sizeof(CLodAVBOITFitState), org::runtime::UploadTarget::FromShared(m_fitStateBuffer), 0);
        m_fitStateInitialized = true;
    }
}

PassReturn AVBOITSetupPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

    const auto clearFloatResource = [&commandList](PixelBuffer* resource, float clearValueScalar = 0.0f) {
        if (!resource) {
            return;
        }

        rhi::UavClearFloat clearValue{};
        clearValue.v[0] = clearValueScalar;
        clearValue.v[1] = clearValueScalar;
        clearValue.v[2] = clearValueScalar;
        clearValue.v[3] = clearValueScalar;

        const unsigned int sliceCount = resource->GetNumUAVSlices();
        for (unsigned int sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
            rhi::UavClearInfo clearInfo{};
            clearInfo.cpuVisible = resource->GetUAVNonShaderVisibleInfo(0, sliceIndex).slot;
            clearInfo.shaderVisible = resource->GetUAVShaderVisibleInfo(0, sliceIndex).slot;
            clearInfo.resource = resource->GetAPIResource();
            commandList.ClearUavFloat(clearInfo, clearValue);
        }
    };

    const auto clearUintResource = [&commandList](PixelBuffer* resource, uint32_t clearValueScalar = 0u) {
        if (!resource) {
            return;
        }

        rhi::UavClearUint clearValue{};
        clearValue.v[0] = clearValueScalar;
        clearValue.v[1] = clearValueScalar;
        clearValue.v[2] = clearValueScalar;
        clearValue.v[3] = clearValueScalar;

        const unsigned int sliceCount = resource->GetNumUAVSlices();
        for (unsigned int sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
            rhi::UavClearInfo clearInfo{};
            clearInfo.cpuVisible = resource->GetUAVNonShaderVisibleInfo(0, sliceIndex).slot;
            clearInfo.shaderVisible = resource->GetUAVShaderVisibleInfo(0, sliceIndex).slot;
            clearInfo.resource = resource->GetAPIResource();
            commandList.ClearUavUint(clearInfo, clearValue);
        }
    };

    clearFloatResource(m_occupancyTexture.get());
    clearFloatResource(m_coverageTexture.get());
    clearFloatResource(m_integratedTransmittanceTexture.get(), 1.0f);
    clearUintResource(m_occupancySliceMaskTexture.get());
    clearUintResource(m_zeroTransmittanceSliceTexture.get(), CLodAVBOITDefaultSliceCount);

    if (m_accumulationTexture) {
        commandList.ClearRenderTargetView(
            m_accumulationTexture->GetRTVInfo(0).slot,
            m_accumulationTexture->GetClearColor());
    }
    if (m_normalizationTexture) {
        commandList.ClearRenderTargetView(
            m_normalizationTexture->GetRTVInfo(0).slot,
            m_normalizationTexture->GetClearColor());
    }
    if (m_shadingExtinctionTexture) {
        commandList.ClearRenderTargetView(
            m_shadingExtinctionTexture->GetRTVInfo(0).slot,
            m_shadingExtinctionTexture->GetClearColor());
    }
    return {};
}

void AVBOITSetupPass::Cleanup()
{
}