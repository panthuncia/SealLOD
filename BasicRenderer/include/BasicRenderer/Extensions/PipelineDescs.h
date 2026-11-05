#pragma once

#include <d3d12.h>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <BasicRenderer/Extensions/IndirectCommand.h>
#include <BasicRenderer/Extensions/ShaderBuffers.h>
#include <Render/PipelineDesc.h>

// BasicRenderer's shared pipeline and command signature descriptions, for org::RenderPassBuilder::Pipeline and CommandSignature.
namespace br::render {

// A compute pipeline on the standard compute layout.
inline org::ComputePipelineDesc ComputePipeline(std::wstring path, std::wstring entry, std::string name,
	std::vector<org::PipelineShaderDefine> defines = {}) {
	org::ComputePipelineDesc desc;
	desc.shader = {std::move(path), std::move(entry)};
	desc.name = std::move(name);
	desc.defines = std::move(defines);
	return desc;
}

inline org::CommandSignatureDesc RawDispatchCommandSignature() {
	org::CommandSignatureDesc desc;
	desc.args = {{.kind = rhi::IndirectArgKind::Dispatch}};
	desc.byteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
	desc.name = "RawDispatch";
	return desc;
}

// A dispatch with its leading root constants in the indirect root parameter, as DispatchIndirectCommand and its relatives lay out.
inline org::CommandSignatureDesc ConstantsDispatchCommandSignature(uint32_t constantCount, uint32_t byteStride, std::string name) {
	org::CommandSignatureDesc desc;
	desc.args = {
		{.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = {IndirectCommandSignatureRootSignatureIndex, 0, constantCount}}},
		{.kind = rhi::IndirectArgKind::Dispatch},
	};
	desc.byteStride = byteStride;
	desc.name = std::move(name);
	return desc;
}

inline org::CommandSignatureDesc DispatchCommandSignature() {
	return ConstantsDispatchCommandSignature(3, sizeof(DispatchIndirectCommand), "Dispatch");
}
inline org::CommandSignatureDesc MaterialEvaluationCommandSignature() {
	return ConstantsDispatchCommandSignature(4, sizeof(MaterialEvaluationIndirectCommand), "MaterialEvaluation");
}
// Mesh dispatch with leading root constants, on the graphics layout.
inline org::CommandSignatureDesc DispatchMeshCommandSignature() {
	auto desc = ConstantsDispatchCommandSignature(3, sizeof(DispatchMeshIndirectCommand), "DispatchMesh");
	desc.args.back().kind = rhi::IndirectArgKind::DispatchMesh;
	desc.layout = org::PipelineLayoutRef::StandardGraphics();
	return desc;
}
inline org::CommandSignatureDesc TerrainRegionMaterialEvaluationCommandSignature() {
	return ConstantsDispatchCommandSignature(5, sizeof(TerrainRegionMaterialEvaluationIndirectCommand), "TerrainRegionMaterialEvaluation");
}

} // namespace br::render
