#pragma once

#include <cstdint>
#include <cstddef>
#include <BasicRenderer/Pipeline/PipelineKind.h>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace br::diagnostics {


enum class LiveJobState : std::uint8_t {
	Queued,
	Compiling,
	ReadyToPublish,
	Published,
	Failed
};

struct LivePipelineInfo {
	std::string id;
	std::string displayName;
	br::extensions::PipelineKind kind = br::extensions::PipelineKind::Compute;
	std::vector<std::string> shaderPaths;
	std::uint64_t activeGeneration = 0;
	std::uint64_t sourceHash = 0;
	std::uint64_t bytecodeHash = 0;
	std::string label;
	std::map<std::string, std::string> defineOverrides;
	bool compiling = false;
};

struct LiveJobInfo {
	std::uint64_t id = 0;
	std::string pipelineId;
	LiveJobState state = LiveJobState::Queued;
	std::uint64_t generation = 0;
	std::string error;
};

struct RecompileOptions {
	std::string label;
	std::map<std::wstring, std::wstring> defineOverrides;
};

struct PipelineGenerationSnapshot {
	std::uint64_t epoch = 0;
	std::vector<LivePipelineInfo> pipelines;
	std::uint64_t digest = 0;
};

std::uint64_t GetPipelineEpoch();
PipelineGenerationSnapshot GetPipelineGenerationSnapshot();
std::vector<LivePipelineInfo> ListPipelines();
std::optional<LiveJobInfo> GetLiveJob(std::uint64_t jobId);
std::uint64_t RequestRecompile(const std::string& pipelineId, RecompileOptions options = {});
std::uint64_t RequestActivation(const std::string& pipelineId, std::uint64_t generation);
std::size_t ReloadCLodWorkGraphs();

} // namespace br::diagnostics
