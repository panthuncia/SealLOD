#pragma once

#include <cstddef>

#include <rhi.h>

#include "ThirdParty/FFX/ffx_api.hpp"
#include "ThirdParty/FFX/ffx_upscale.hpp"
#include "ThirdParty/FFX/host/ffx_sssr.h"

namespace org { class PixelBuffer; }
using org::PixelBuffer;

namespace fidelityfx_backend::api {
bool LoadModule(rhi::Backend backend);
void UnloadModule();
ffx::ReturnCode CreateContext(ffx::Context& context, ffxAllocationCallbacks* memCb, ffxCreateContextDescHeader* header);
ffx::ReturnCode DestroyContext(ffx::Context& context, const ffxAllocationCallbacks* memCb = nullptr);
ffx::ReturnCode Query(ffx::Context& context, ffxQueryDescHeader* header);
ffx::ReturnCode Dispatch(ffx::Context& context, ffxDispatchDescHeader* header);
bool CreateUpscaleContext(ffx::Context& context, rhi::Backend backend, rhi::Device device, ffx::CreateContextDescUpscale& createUpscaling);
FfxApiResource GetResource(rhi::Backend backend, PixelBuffer* resource, const wchar_t* name, FfxApiResourceState state);
void* GetCommandList(rhi::Backend backend, rhi::CommandList& commandList);

template<class... Desc>
ffx::ReturnCode CreateContext(ffx::Context& context, ffxAllocationCallbacks* memCb, Desc&... desc)
{
	auto* header = ffx::LinkHeaders(desc.header...);
	return CreateContext(context, memCb, header);
}

template<class... Desc>
ffx::ReturnCode Query(ffx::Context& context, Desc&... desc)
{
	auto* header = ffx::LinkHeaders(desc.header...);
	return Query(context, header);
}

template<class... Desc>
ffx::ReturnCode Dispatch(ffx::Context& context, Desc&... desc)
{
	auto* header = ffx::LinkHeaders(desc.header...);
	return Dispatch(context, header);
}
}

namespace fidelityfx_backend::host {
bool CreateBackendInterface(FfxInterface& backendInterface, void*& scratchMemory, rhi::Backend backend, rhi::Device device, size_t maxContexts);
FfxResource GetResource(rhi::Backend backend, PixelBuffer* resource, const wchar_t* name, FfxResourceStates state);
void* GetCommandList(rhi::Backend backend, rhi::CommandList& commandList);
}
