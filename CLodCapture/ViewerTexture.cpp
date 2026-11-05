#include "ViewerTexture.h"

#include <Windows.h>

#include <stdexcept>
#include <utility>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include <BasicRenderer/Diagnostics/ExternalUi.h>

namespace clodcapture {
	namespace {
		using Microsoft::WRL::ComPtr;

		// A texture about to be replaced may still be in a frame in flight. Frames in flight are few, so keeping the
		// last several covers it, and replacing them only happens on a resize.
		constexpr size_t kRetiredTextures = 8u;

		struct Retired {
			ComPtr<ID3D12Resource> resource;
			uint32_t descriptor = 0;
		};
		std::vector<Retired> g_retired;
	}

	struct ViewerTexture::Impl {
		ComPtr<ID3D12Resource> resource;
		uint32_t descriptor = 0;
		bool hasDescriptor = false;
		uint32_t width = 0, height = 0;
		ImTextureID id = 0;
	};

	ViewerTexture::ViewerTexture() : m_impl(std::make_unique<Impl>()) {}

	ViewerTexture::~ViewerTexture()
	{
		if (m_impl->hasDescriptor) {
			g_retired.push_back({ std::move(m_impl->resource), m_impl->descriptor });
		}
	}

	bool ViewerTexture::Valid() const { return m_impl->resource != nullptr; }
	ImTextureID ViewerTexture::Id() const { return m_impl->id; }
	uint32_t ViewerTexture::Width() const { return m_impl->width; }
	uint32_t ViewerTexture::Height() const { return m_impl->height; }

	// CPU pages the GPU reads directly. Nothing else in the frame has to know about it: no copy list, no
	// barrier and no fence, because a texture in the common state promotes itself on a read. A frame in
	// flight may see a half-written image, which only matters for the one frame it takes to replace it.
	void ViewerTexture::Upload(uint32_t width, uint32_t height, const std::vector<uint32_t>& pixels)
	{
		if (m_impl->width != width || m_impl->height != height || !m_impl->resource) {
			ID3D12Device* device = br::diagnostics::GetExternalUiDevice();
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_CUSTOM;
			heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
			heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			description.Width = width;
			description.Height = height;
			description.DepthOrArraySize = 1;
			description.MipLevels = 1;
			description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			description.SampleDesc.Count = 1;
			description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			ComPtr<ID3D12Resource> resource;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)))) {
				throw std::runtime_error("the viewer could not create a CPU-writable texture");
			}
			if (m_impl->hasDescriptor) {
				g_retired.push_back({ std::move(m_impl->resource), m_impl->descriptor });
				m_impl->hasDescriptor = false;
			}
			while (g_retired.size() > kRetiredTextures) {
				br::diagnostics::FreeExternalUiDescriptor(g_retired.front().descriptor);
				g_retired.erase(g_retired.begin());
			}
			m_impl->resource = std::move(resource);
			const br::diagnostics::ExternalUiDescriptor descriptor = br::diagnostics::AllocateExternalUiDescriptor();
			m_impl->descriptor = descriptor.index;
			m_impl->hasDescriptor = true;
			D3D12_CPU_DESCRIPTOR_HANDLE handle{};
			handle.ptr = static_cast<SIZE_T>(descriptor.cpuHandle);
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(m_impl->resource.Get(), &view, handle);
			m_impl->id = static_cast<ImTextureID>(descriptor.gpuHandle);
			m_impl->width = width;
			m_impl->height = height;
			if (FAILED(m_impl->resource->Map(0, nullptr, nullptr))) {
				throw std::runtime_error("the viewer could not map its texture");
			}
		}
		m_impl->resource->WriteToSubresource(0, nullptr, pixels.data(), width * 4u, width * height * 4u);
	}

}
