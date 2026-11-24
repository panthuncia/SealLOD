#pragma once

#include <memory>

#include "Pipeline/PipelineState/PipelineLibrary.h"

// What lets the pipeline library build pipelines from descriptions: its shader compiler, shader cache and layouts.
std::shared_ptr<br::PipelineCompiler> CreateOrgPipelineCompiler();
