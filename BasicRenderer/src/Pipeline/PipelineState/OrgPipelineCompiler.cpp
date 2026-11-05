#include "Pipeline/PipelineState/OrgPipelineCompiler.h"

#include <stdexcept>
#include <vector>

#include "Pipeline/PipelineState/PSOManager.h"

namespace {

Microsoft::WRL::ComPtr<ID3DBlob>& StageBlob(ShaderBundle& bundle, rhi::ShaderStage stage) {
	switch (stage) {
	case rhi::ShaderStage::Compute: return bundle.computeShader;
	case rhi::ShaderStage::Vertex: return bundle.vertexShader;
	case rhi::ShaderStage::Pixel: return bundle.pixelShader;
	case rhi::ShaderStage::Mesh: return bundle.meshShader;
	case rhi::ShaderStage::Task: return bundle.amplificationShader;
	default: throw std::invalid_argument("A pipeline description names a stage BasicRenderer does not compile");
	}
}

class OrgPipelineCompiler final : public org::PipelineCompiler {
public:
	void Compile(const org::PipelineCompileRequest& request, Completion completion) override {
		std::vector<DxcDefine> defines;
		defines.reserve(request.defines.size());
		for (const auto& define : request.defines) defines.push_back({define.name.c_str(), define.value.c_str()});

		ShaderInfoBundle info;
		info.defines = std::move(defines);
		for (const auto& stage : request.stages) {
			const ShaderInfo shader{stage.source.path, stage.source.entry, stage.source.target};
			switch (stage.stage) {
			case rhi::ShaderStage::Compute: info.computeShader = shader; break;
			case rhi::ShaderStage::Vertex: info.vertexShader = shader; break;
			case rhi::ShaderStage::Pixel: info.pixelShader = shader; break;
			case rhi::ShaderStage::Mesh: info.meshShader = shader; break;
			case rhi::ShaderStage::Task: info.amplificationShader = shader; break;
			default: throw std::invalid_argument("A pipeline description names a stage BasicRenderer does not compile");
			}
		}

		auto bundle = std::make_shared<ShaderBundle>(
			PSOManager::GetInstance().CompileShadersForPipelineLibrary(info, request.device, request.bypassCache));
		org::CompiledPipeline compiled;
		compiled.stages.reserve(request.stages.size());
		for (const auto& stage : request.stages) compiled.stages.push_back(rhi::DXIL(StageBlob(*bundle, stage.stage).Get()));
		compiled.resources = bundle->resourceDescriptorSlots;
		compiled.resourceIDsHash = bundle->resourceIDsHash;
		compiled.codeOwner = bundle;
		completion(std::move(compiled));
	}

	org::PipelineLayoutBinding StandardLayout(org::PipelineLayoutKind kind, org::DeviceInstanceId device) override {
		auto& manager = PSOManager::GetInstance();
		const auto& layout = kind == org::PipelineLayoutKind::StandardGraphics
			? manager.GetRootSignature(device) : manager.GetComputeRootSignature(device);
		const auto handle = layout.GetHandle();
		return {handle, manager.CaptureLayoutOwner(handle)};
	}
};

} // namespace

std::shared_ptr<org::PipelineCompiler> CreateOrgPipelineCompiler() {
	return std::make_shared<OrgPipelineCompiler>();
}
