#pragma once

#include <algorithm>
#include <functional>
#include <mutex>
#include <memory>
#include <utility>
#include <vector>

#include "Render/RenderGraph/RenderGraph.h"
#include "Render/RenderGraph/RenderGraphExtension.h"
#include "Runtime/GraphIntegration/FrameBuild.h"

#include <spdlog/spdlog.h>

namespace br::render {

// Runs the renderer's own declarations as part of a graph build: in PrepareForBuild, where an extension may register
// providers and resources and build passes on the graph. The renderer registers two: one first, for its providers (the
// other extensions' PrepareForBuild reads them), and one last, for its passes (they read what the extensions registered).
class DeclarationGraphExtension final : public org::IRenderGraphExtension {
public:
	using Declare = std::function<void(org::RenderGraph&)>;
	using Gather = std::function<void(org::RenderGraph&, std::vector<org::ExternalPassDesc>&)>;
	DeclarationGraphExtension(Declare prepare, Declare finish = {}, Gather gather = {})
		: m_prepare(std::move(prepare)), m_finish(std::move(finish)), m_gather(std::move(gather)) {}

	void PrepareForBuild(org::RenderGraph& graph) override { if (m_prepare) m_prepare(graph); }
	void FinishBuild(org::RenderGraph& graph) override { if (m_finish) m_finish(graph); }
	void GatherStructuralPasses(org::RenderGraph& graph, std::vector<org::ExternalPassDesc>& out) override {
		if (m_gather) m_gather(graph, out);
	}

private:
	Declare m_prepare;
	Declare m_finish;
	Gather m_gather;
};

// The passes an earlier host epoch's graph of the build forwarded to this graph's host epoch (ForwardedPasses), taken.
inline void GatherForwardedPasses(org::RenderGraph& graph, std::vector<org::ExternalPassDesc>& out) {
	auto* build = CurrentFrameBuild();
	if (!build || !build->forwarded) return;
	std::scoped_lock lock(build->forwarded->mutex);
	const auto found = build->forwarded->byHostEpoch.find(graph.PassEpochFilter());
	if (found == build->forwarded->byHostEpoch.end()) return;
	for (auto& pass : found->second) out.push_back(std::move(pass));
	build->forwarded->byHostEpoch.erase(found);
}

// Drops a pass's anchors to passes of another host epoch's graph (M4 step 7). ORG cannot find them in this graph and would
// ignore each with a warning. The epoch order already meets them: Before a pass of a later host epoch, or After one of an
// earlier host epoch. An anchor the order cannot meet (Before an earlier epoch's pass, After a later one's) is logged as an
// error and dropped too. An anchor to a pass no graph is known to have (not yet built, or not in the recipe) is kept: ORG
// resolves it in the graph, or warns.
inline void DropCrossEpochAnchors(org::ExternalPassDesc& pass, uint32_t passHost, const FramePassEpochs& passEpochs) {
	if (!pass.where) return;
	const auto& hosts = FrameHostEpochs();
	const auto order = [&](uint32_t host) { return std::find(hosts.begin(), hosts.end(), host) - hosts.begin(); };
	const auto drop = [&](std::vector<std::string>& anchors, bool before) {
		std::erase_if(anchors, [&](const std::string& anchor) {
			const uint32_t anchorEpoch = passEpochs.KnownEpochOf(anchor);
			if (!IsFrameEpoch(anchorEpoch)) return false;
			const uint32_t anchorHost = HostEpochOf(anchorEpoch);
			if (anchorHost == passHost) return false;
			const bool met = before ? order(anchorHost) > order(passHost) : order(anchorHost) < order(passHost);
			if (!met)
				spdlog::error("Frame graph: pass '{}' asks to run {} '{}' (host epoch {}), but it runs in host epoch {}: the epoch order "
					"cannot meet it", pass.name, before ? "before" : "after", anchor, anchorHost, passHost);
			return true;
		});
	};
	drop(pass.where->before, true);
	drop(pass.where->after, false);
}

// A frame graph extension with its frame epoch (docs/frame_graph_composition_plan.md, M4). It forwards every call to the
// extension, with the epoch current on the thread (ScopedDeclarationEpoch) so what the extension records goes to its epoch's
// outputs. Each pass it gathers gets the extension's epoch unless the pass names its own (ExternalPassDesc::Epoch with a
// frame epoch). The pass is tagged in the build's pass epochs and given its host epoch's ORG tag (OrgPassEpochOf).
// kNoFrameEpoch: the extension's passes take the epoch of the pass each follows (readback captures).
class EpochTaggedExtension final : public org::IRenderGraphExtension {
public:
	EpochTaggedExtension(std::unique_ptr<org::IRenderGraphExtension> inner, uint32_t epoch, std::shared_ptr<FramePassEpochs> passEpochs)
		: m_inner(std::move(inner)), m_epoch(epoch), m_passEpochs(std::move(passEpochs)) {}

	void PrepareForBuild(org::RenderGraph& graph) override { ScopedDeclarationEpoch scope(m_epoch); m_inner->PrepareForBuild(graph); }
	void FinishBuild(org::RenderGraph& graph) override { ScopedDeclarationEpoch scope(m_epoch); m_inner->FinishBuild(graph); }
	void Initialize(org::RenderGraph& graph) override { ScopedDeclarationEpoch scope(m_epoch); m_inner->Initialize(graph); }
	void Shutdown(org::RenderGraph& graph) override { m_inner->Shutdown(graph); }
	void OnRegistryReset(org::ResourceRegistry* registry) override { m_inner->OnRegistryReset(registry); }
	void AfterExecute(org::RenderGraph& graph) override { m_inner->AfterExecute(graph); }
	// The graph keeps the passes of its own host epoch (RenderGraph::PassEpochFilter). A pass of a later host epoch goes to that
	// epoch's graph of the same build (ForwardedPasses); one of an earlier epoch cannot run in order and is dropped. A follower
	// (a readback capture) takes the host epoch of its anchor, so each epoch's graph copies only after its own passes.
	void GatherStructuralPasses(org::RenderGraph& graph, std::vector<org::ExternalPassDesc>& out) override {
		const size_t first = out.size();
		{
			ScopedDeclarationEpoch scope(m_epoch);
			m_inner->GatherStructuralPasses(graph, out);
		}
		const uint32_t graphHost = graph.PassEpochFilter();
		const auto& hosts = FrameHostEpochs();
		const auto order = [&](uint32_t host) { return std::find(hosts.begin(), hosts.end(), host) - hosts.begin(); };
		size_t kept = first;
		for (size_t index = first; index < out.size(); ++index) {
			auto& pass = out[index];
			const uint32_t epoch = IsFrameEpoch(pass.epoch) ? pass.epoch : m_epoch;
			uint32_t passEpoch = epoch;
			if (m_passEpochs && !pass.name.empty()) {
				if (IsFrameEpoch(epoch)) m_passEpochs->Tag(pass.name, epoch);
				else if (pass.where && !pass.where->after.empty()) {
					m_passEpochs->Follows(pass.name, pass.where->after.front());
					passEpoch = m_passEpochs->EpochOf(pass.name);
				}
			}
			pass.epoch = IsFrameEpoch(epoch) ? OrgPassEpochOf(epoch) : kNoFrameEpoch;
			const uint32_t passHost = HostEpochOf(IsFrameEpoch(passEpoch) ? passEpoch : kVisibilityEpoch);
			// A follower keeps its anchor: it is placed by it (and in its epoch's graph).
			if (m_passEpochs && graphHost != kNoFrameEpoch && IsFrameEpoch(epoch)) DropCrossEpochAnchors(pass, passHost, *m_passEpochs);
			if (graphHost == kNoFrameEpoch || passHost == graphHost) {
				if (kept != index) out[kept] = std::move(pass);
				++kept;
				continue;
			}
			if (!IsFrameEpoch(epoch)) continue;  // a follower of another epoch's pass: that epoch's graph has it
			auto* build = CurrentFrameBuild();
			if (order(passHost) > order(graphHost) && build && build->forwarded) {
				std::scoped_lock lock(build->forwarded->mutex);
				build->forwarded->byHostEpoch[passHost].push_back(std::move(pass));
				build->forwarded->hostEpochMask |= HostEpochBit(passHost);
				continue;
			}
			spdlog::error("Frame graph: pass '{}' of epoch {} is not built into host epoch {}'s graph ({})", pass.name,
				FrameEpochName(epoch), graphHost, build ? "an earlier epoch" : "gathered outside a frame build");
		}
		out.erase(out.begin() + static_cast<std::ptrdiff_t>(kept), out.end());
	}

	uint32_t Epoch() const noexcept { return m_epoch; }

private:
	std::unique_ptr<org::IRenderGraphExtension> m_inner;
	uint32_t m_epoch = kNoFrameEpoch;
	std::shared_ptr<FramePassEpochs> m_passEpochs;
};

} // namespace br::render
