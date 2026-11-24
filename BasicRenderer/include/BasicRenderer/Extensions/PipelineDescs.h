#pragma once

#include <d3d12.h>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <BasicRenderer/Extensions/IndirectCommand.h>
#include <BasicRenderer/Extensions/ShaderBuffers.h>
#include <BasicRenderer/Pipeline/PipelineDesc.h>

// BasicRenderer's shared pipeline and command signature descriptions, and the declarations a pass makes with them.
namespace br::render {

// A pipeline of the pass, by description. BasicRenderer's pipeline library builds it on the primary device (ORG builds no
// pipelines); the renderer waits for every pipeline a graph build declared before the graph's first frame. Declaring the
// same description again returns the same pipeline, so call this from Declare and bind the handle in Record.
br::PipelineState DeclarePipeline(br::PipelineDesc desc);
// As DeclarePipeline, for a command signature: hand the handle to RecordingContext::ExecuteIndirect.
br::CommandSignatureState DeclareCommandSignature(br::CommandSignatureDesc desc);

// A compute pipeline on the standard compute layout.
inline br::ComputePipelineDesc ComputePipeline(std::wstring path, std::wstring entry, std::string name,
	std::vector<br::PipelineShaderDefine> defines = {}) {
	br::ComputePipelineDesc desc;
	desc.shader = {std::move(path), std::move(entry)};
	desc.name = std::move(name);
	desc.defines = std::move(defines);
	return desc;
}

inline br::CommandSignatureDesc RawDispatchCommandSignature() {
	br::CommandSignatureDesc desc;
	desc.args = {{.kind = rhi::IndirectArgKind::Dispatch}};
	desc.byteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
	desc.name = "RawDispatch";
	return desc;
}

// A dispatch with its leading root constants in the indirect root parameter, as DispatchIndirectCommand and its relatives lay out.
inline br::CommandSignatureDesc ConstantsDispatchCommandSignature(uint32_t constantCount, uint32_t byteStride, std::string name) {
	br::CommandSignatureDesc desc;
	desc.args = {
		{.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = {IndirectCommandSignatureRootSignatureIndex, 0, constantCount}}},
		{.kind = rhi::IndirectArgKind::Dispatch},
	};
	desc.byteStride = byteStride;
	desc.name = std::move(name);
	return desc;
}

inline br::CommandSignatureDesc DispatchCommandSignature() {
	return ConstantsDispatchCommandSignature(3, sizeof(DispatchIndirectCommand), "Dispatch");
}
inline br::CommandSignatureDesc MaterialEvaluationCommandSignature() {
	return ConstantsDispatchCommandSignature(4, sizeof(MaterialEvaluationIndirectCommand), "MaterialEvaluation");
}
// Mesh dispatch with leading root constants, on the graphics layout.
inline br::CommandSignatureDesc DispatchMeshCommandSignature() {
	auto desc = ConstantsDispatchCommandSignature(3, sizeof(DispatchMeshIndirectCommand), "DispatchMesh");
	desc.args.back().kind = rhi::IndirectArgKind::DispatchMesh;
	desc.layout = br::PipelineLayoutRef::StandardGraphics();
	return desc;
}
inline br::CommandSignatureDesc TerrainRegionMaterialEvaluationCommandSignature() {
	return ConstantsDispatchCommandSignature(5, sizeof(TerrainRegionMaterialEvaluationIndirectCommand), "TerrainRegionMaterialEvaluation");
}

} // namespace br::render
