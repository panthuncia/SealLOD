#pragma once

#include <directx/d3d12.h>
#include <wrl.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "Render/PipelineState.h"

#pragma warning(push, 0)
#include "ThirdParty/DirectX/dxcapi.h"
#pragma warning(pop)

struct ShaderInfo {
    std::wstring filename;
    std::wstring entryPoint;
    std::wstring target;
    ShaderInfo(const std::wstring& file, const std::wstring& entry, const std::wstring& tgt)
        : filename(file), entryPoint(entry), target(tgt) {}
};

struct ShaderLibraryInfo {
    std::wstring filename;
	std::wstring target;
    ShaderLibraryInfo(const std::wstring& file, const std::wstring& tgt) : filename(file), target(tgt) {}
};

struct ShaderInfoBundle {
    ShaderInfoBundle(const std::vector<DxcDefine>& defs, bool debug = false, bool warnings = true) : 
        defines(defs), enableDebugInfo(debug), warningsAsErrors(warnings) {}
	ShaderInfoBundle() = default;
    std::optional<ShaderInfo> vertexShader;
    std::optional<ShaderInfo> pixelShader;
    std::optional<ShaderInfo> amplificationShader;
    std::optional<ShaderInfo> meshShader;
    std::optional<ShaderInfo> computeShader;

    std::vector<DxcDefine> defines;
    bool enableDebugInfo = false;
    bool warningsAsErrors = true;
};

struct ShaderVariantRequest;

struct ShaderBundle {
    Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
    Microsoft::WRL::ComPtr<ID3DBlob> amplificationShader;
    Microsoft::WRL::ComPtr<ID3DBlob> meshShader;
    Microsoft::WRL::ComPtr<ID3DBlob> computeShader;
	org::PipelineResources resourceDescriptorSlots;
	uint64_t resourceIDsHash = 0;
	// The shader store artifact this bytecode was read from; zero when it was compiled for this request. A device that
	// rejects the bytecode hands it to PSOManager::DiscardStoredShaders.
	uint64_t storeKeyHigh = 0;
	uint64_t storeKeyLow = 0;
};

struct ShaderLibraryBundle {
    Microsoft::WRL::ComPtr<ID3DBlob> libraryBlob;
    org::PipelineResources resourceDescriptorSlots;
    uint64_t resourceIDsHash = 0;
    // As ShaderBundle's.
    uint64_t storeKeyHigh = 0;
    uint64_t storeKeyLow = 0;
};

