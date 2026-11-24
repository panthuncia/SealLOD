#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "Render/RenderGraph/RenderGraph.h"
#include "Render/RenderGraph/RenderGraphExtension.h"

namespace br::render {

// Runs the renderer's own declarations as part of a graph build: in PrepareForBuild, where an extension may register
// providers and resources and build passes on the graph. The renderer registers two: one first, for its providers (the
// other extensions' PrepareForBuild reads them), and one last, for its passes (they read what the extensions registered).
class DeclarationGraphExtension final : public org::IRenderGraphExtension {
public:
	using Declare = std::function<void(org::RenderGraph&)>;
	DeclarationGraphExtension(Declare prepare, Declare finish = {}) : m_prepare(std::move(prepare)), m_finish(std::move(finish)) {}

	void PrepareForBuild(org::RenderGraph& graph) override { if (m_prepare) m_prepare(graph); }
	void FinishBuild(org::RenderGraph& graph) override { if (m_finish) m_finish(graph); }
	void GatherStructuralPasses(org::RenderGraph&, std::vector<org::ExternalPassDesc>&) override {}

private:
	Declare m_prepare;
	Declare m_finish;
};

} // namespace br::render
