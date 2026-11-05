#pragma once

#include <memory>

#include <Render/PipelineDesc.h>

// What lets ORG build pipelines from descriptions in BasicRenderer: its shader compiler, shader cache and layouts.
std::shared_ptr<org::PipelineCompiler> CreateOrgPipelineCompiler();
