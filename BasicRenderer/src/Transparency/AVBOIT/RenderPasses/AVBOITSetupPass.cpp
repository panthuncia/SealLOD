#include "Transparency/AVBOIT/RenderPasses/AVBOITSetupPass.h"

#include "Scene/Views/ViewManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Resources/GloballyIndexedResource.h"

AVBOITSetupPass::AVBOITSetupPass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::Buffer> fitStateBuffer,
    std::shared_ptr<org::Buffer> depthWarpLUTBuffer,
    std::shared_ptr<org::PixelBuffer> occupancyTexture,
    std::shared_ptr<org::PixelBuffer> coverageTexture,
    std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
    std::shared_ptr<org::PixelBuffer> scalarExtinctionTexture,
    std::shared_ptr<org::PixelBuffer> chromaticExtinctionTexture,
    std::shared_ptr<org::PixelBuffer> integratedTransmittanceTexture,
    std::shared_ptr<org::PixelBuffer> zeroTransmittanceSliceTexture,
    std::shared_ptr<org::PixelBuffer> accumulationTexture,
    std::shared_ptr<org::PixelBuffer> normalizationTexture,
    std::shared_ptr<org::PixelBuffer> shadingExtinctionTexture)
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

AVBOITSetupBindings AVBOITSetupPass::Declare(org::PassBuilder& builder)
{
    if (m_configBuffer) {
        builder.ShaderResource(m_configBuffer);
    }
    if (m_fitStateBuffer) {
        builder.ShaderResource(m_fitStateBuffer);
    }
    AVBOITSetupBindings bindings;
    if (m_depthWarpLUTBuffer) bindings.depthWarp = builder.ShaderResource(m_depthWarpLUTBuffer).View();
    for (const auto& resource : {m_occupancyTexture, m_coverageTexture, m_occupancySliceMaskTexture,
        m_integratedTransmittanceTexture, m_zeroTransmittanceSliceTexture})
        if (resource) {
            AVBOITSetupBindings::Clear clear{};
            for (uint32_t slice = 0; slice < resource->GetArraySize(); ++slice) {
                auto use = builder.UnorderedAccessClear(resource, org::UavView{UINT32_MAX, 0, slice});
                clear.shaderViews.push_back(use.View(0));
                clear.cpuViews.push_back(use.View(1));
            }
            bindings.clears.push_back(std::move(clear));
        }

    const org::UavView fullUav{static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)};
    const org::SrvView fullSrv{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)};
    bindings.scalarExtinction = builder.UnorderedAccess(m_scalarExtinctionTexture, fullUav).View();
    bindings.chromaticExtinction = builder.UnorderedAccess(m_chromaticExtinctionTexture, fullUav).View();
    bindings.integratedTransmittance = builder.UnorderedAccess(m_integratedTransmittanceTexture, fullUav).View();
    bindings.shadingTransmittance = builder.ShaderResource(m_integratedTransmittanceTexture, fullSrv).View();

    if (m_accumulationTexture) {
        bindings.targets.push_back(builder.RenderTargetClear(m_accumulationTexture).View());
    }
    if (m_normalizationTexture) {
        bindings.targets.push_back(builder.RenderTargetClear(m_normalizationTexture).View());
    }
    if (m_shadingExtinctionTexture) {
        bindings.targets.push_back(builder.RenderTargetClear(m_shadingExtinctionTexture).View());
    }

    m_config.sliceCount = CLodAVBOITDefaultSliceCount;
    m_config.virtualSliceCount = CLodAVBOITDefaultVirtualSliceCount;
    m_config.lowResolutionWidth = m_occupancyTexture ? m_occupancyTexture->GetWidth() : 0u;
    m_config.lowResolutionHeight = m_occupancyTexture ? m_occupancyTexture->GetHeight() : 0u;
    m_config.depthDistributionExponent = CLodAVBOITDefaultDepthDistributionExponent;
    m_config.lookupDepthBiasInSlices = CLodAVBOITDefaultLookupDepthBiasInSlices;
    m_config.zeroTransmittanceThreshold = CLodAVBOITDefaultZeroTransmittanceThreshold;
    return bindings;
}

void AVBOITSetupPass::Update(const org::UpdateExecutionContext& executionContext)
{
    if (!m_configBuffer) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;

    for (const auto& view : context.Views()) if (view.primary) {
        m_config.viewNearDepth = view.cameraInfo.zNear;
        m_config.viewFarDepth = view.cameraInfo.zFar;
        break;
    }

    if (m_fitStateBuffer && !m_fitStateInitialized) {
        const CLodAVBOITFitState fitState{};
        UploadBufferData(&fitState, sizeof(CLodAVBOITFitState), org::runtime::UploadTarget::FromShared(m_fitStateBuffer), 0);
        m_fitStateInitialized = true;
    }
}

AVBOITSetupFrameData AVBOITSetupPass::Prepare(
    const AVBOITSetupBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    AVBOITSetupFrameData data;
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    const auto index = [&](org::DeclaredViewToken token) {
        return token.layout ? preparation.Resolve(token).index : 0xFFFFFFFFu;
    };
    auto config = m_config;
    config.occupancyUAVDescriptorIndex = index(bindings.clears.at(0).shaderViews.at(0));
    config.coverageUAVDescriptorIndex = index(bindings.clears.at(1).shaderViews.at(0));
    config.occupancySliceMaskUAVDescriptorIndex = index(bindings.clears.at(2).shaderViews.at(0));
    config.depthWarpLUTSRVDescriptorIndex = index(bindings.depthWarp);
    config.scalarExtinctionUAVDescriptorIndex = index(bindings.scalarExtinction);
    config.chromaticExtinctionUAVDescriptorIndex = index(bindings.chromaticExtinction);
    config.integratedTransmittanceUAVDescriptorIndex = index(bindings.integratedTransmittance);
    config.shadingTransmittanceSRVDescriptorIndex = index(bindings.shadingTransmittance);
    config.zeroTransmittanceSliceUAVDescriptorIndex = index(bindings.clears.at(4).shaderViews.at(0));
    UploadBufferData(&config, sizeof(config), org::runtime::UploadTarget::FromShared(m_configBuffer), 0);
    const auto append = [&](const AVBOITSetupBindings::Clear& binding, bool isFloat, float floatValue, uint32_t uintValue) {
        if (binding.shaderViews.size() != binding.cpuViews.size()) throw std::logic_error("AVBOIT clear view declaration is incomplete");
        for (size_t slice = 0; slice < binding.shaderViews.size(); ++slice) {
            data.clears.push_back({preparation.DeclaredReference(binding.shaderViews[slice]),
                preparation.Capture(binding.cpuViews[slice]),
                preparation.Capture(binding.shaderViews[slice]),
                floatValue, uintValue, isFloat});
        }
    };
    if (bindings.clears.size() != 5) throw std::logic_error("AVBOIT clear declaration is incomplete");
    append(bindings.clears[0], true, 0, 0);
    append(bindings.clears[1], true, 0, 0);
    append(bindings.clears[2], false, 0, 0);
    append(bindings.clears[3], true, 1, 0);
    append(bindings.clears[4], false, 0, CLodAVBOITDefaultSliceCount);
    for (const auto target : bindings.targets)
        data.targets.push_back({preparation.Capture(target), preparation.ClearValue(target.Resource())});
    return data;
}

void AVBOITSetupPass::Record(const AVBOITSetupBindings&,
    const AVBOITSetupFrameData& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedResourceClears(data, recording);
}
