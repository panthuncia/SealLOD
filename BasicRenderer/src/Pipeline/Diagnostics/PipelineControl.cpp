#include <BasicRenderer/Diagnostics/PipelineControl.h>

#include <utility>

#include "Pipeline/PipelineState/PSOManager.h"
#include "VirtualGeometry/Culling/RenderPasses/HierarchicalCullingPass.h"

std::uint64_t br::diagnostics::GetPipelineEpoch()
{
	return PSOManager::GetInstance().GetPipelineEpoch();
}

br::diagnostics::PipelineGenerationSnapshot br::diagnostics::GetPipelineGenerationSnapshot()
{
	return PSOManager::GetInstance().GetPipelineGenerationSnapshot();
}

std::vector<br::diagnostics::LivePipelineInfo> br::diagnostics::ListPipelines()
{
	return PSOManager::GetInstance().ListPipelines();
}

std::optional<br::diagnostics::LiveJobInfo> br::diagnostics::GetLiveJob(std::uint64_t jobId)
{
	return PSOManager::GetInstance().GetLiveJob(jobId);
}

std::uint64_t br::diagnostics::RequestRecompile(
	const std::string& pipelineId,
	RecompileOptions options)
{
	return PSOManager::GetInstance().RequestRecompile(pipelineId, std::move(options));
}

std::uint64_t br::diagnostics::RequestActivation(
	const std::string& pipelineId,
	std::uint64_t generation)
{
	return PSOManager::GetInstance().RequestActivation(pipelineId, generation);
}

std::size_t br::diagnostics::ReloadCLodWorkGraphs()
{
	return HierarchicalCullingPass::ReloadAllWorkGraphs();
}
