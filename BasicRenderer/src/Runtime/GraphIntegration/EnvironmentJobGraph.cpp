#include "Runtime/GraphIntegration/EnvironmentJobGraph.h"

#include "OpenRenderGraph/GraphDesc.h"
#include "Lighting/Environment/RenderPasses/EnvironmentConversionPass.h"
#include "Lighting/Environment/RenderPasses/EnvironmentFilterPass.h"
#include "Lighting/Environment/RenderPasses/EnvironmentSHPass.h"

#include <BasicTelemetry/Tracy.h>
#include <spdlog/spdlog.h>

namespace br::render {

EnvironmentJobGraph::EnvironmentJobGraph(std::shared_ptr<org::Runtime> runtime, EnvironmentWorkServices work,
    std::shared_ptr<org::Resource> environmentInfo, uint32_t framesInFlight)
    : m_work(work) {
    org::GraphDesc desc("EnvironmentJobs");
    desc.SetFramesInFlight(framesInFlight);
    // Conversion first: SH and prefiltering of an HDRI environment read the cubemap it writes.
    desc.Add(org::ExternalPassDesc::Compute("Environment Conversion Pass",
        std::make_shared<EnvironmentConversionPass>(work.conversion)));
    desc.Add(org::ExternalPassDesc::Compute("Environment Spherical Harmonics Pass",
        std::make_shared<EnvironmentSHPass>(work.sphericalHarmonics, std::move(environmentInfo))));
    desc.Add(org::ExternalPassDesc::Compute("Environment Prefilter Pass",
        std::make_shared<EnvironmentFilterPass>(work.prefilter)));
    m_graph = org::JobGraph::Create(std::move(runtime), std::move(desc));
}

EnvironmentJobGraph::~EnvironmentJobGraph() {
    // The job graph's instance waits for its work when it goes.
    m_graph.reset();
    spdlog::info("EnvironmentJobGraph: {} executions", m_executions);
}

void EnvironmentJobGraph::Execute() {
    BT_ZONE_SCOPE("BR.EnvironmentJobs.Execute");
    const bool work = m_work.conversion.ReadCounters().pending || m_work.prefilter.ReadCounters().pending
        || m_work.sphericalHarmonics.ReadCounters().pending;
    if (!work) {
        m_graph->TryRetire();
        return;
    }
    m_graph->Execute();
    ++m_executions;
}

} // namespace br::render
