#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "Render/QueueKind.h"

// Pipelines described by value, and the handles a pass binds. ORG builds no pipelines: BasicRenderer's PipelineLibrary
// (src/Pipeline/PipelineState/PipelineLibrary.h) builds these descriptions, and PSOManager builds its permutation caches.
// A pass declares one with br::render::DeclarePipeline (Extensions/PipelineDescs.h) and binds the handle in Record.
namespace br {

// Devices a pipeline can have a build on: DeviceRegistry ids below this. Hosts have a primary and a peer today.
inline constexpr size_t kMaxPipelineDevices = 4;

struct PipelineShaderDefine {
	std::wstring name;
	std::wstring value;
};

// A shader of a pipeline. An empty target takes the stage's default (cs_6_6, vs_6_6, ...).
struct PipelineShaderSource {
	std::wstring path;
	std::wstring entry;
	std::wstring target;
};

enum class PipelineLayoutKind : uint8_t { StandardCompute, StandardGraphics, Custom };

// A layout the library creates on each device a pipeline needs it, shared by every pipeline with the same content.
struct PipelineLayoutSpec {
	std::vector<rhi::LayoutBindingRange> ranges;
	std::vector<rhi::PushConstantRangeDesc> pushConstants;
	std::vector<rhi::StaticSamplerDesc> staticSamplers;
	rhi::PipelineLayoutFlags flags = rhi::PF_None;
	std::string name;
};

struct PipelineLayoutRef {
	PipelineLayoutKind kind = PipelineLayoutKind::StandardCompute;
	std::optional<PipelineLayoutSpec> custom;

	static PipelineLayoutRef StandardCompute() { return {PipelineLayoutKind::StandardCompute, std::nullopt}; }
	static PipelineLayoutRef StandardGraphics() { return {PipelineLayoutKind::StandardGraphics, std::nullopt}; }
	static PipelineLayoutRef Custom(PipelineLayoutSpec spec) { return {PipelineLayoutKind::Custom, std::move(spec)}; }
};

struct ComputePipelineDesc {
	PipelineShaderSource shader;
	std::string name;
	std::vector<PipelineShaderDefine> defines;
	PipelineLayoutRef layout = PipelineLayoutRef::StandardCompute();
};

// An absent state is not given to the device at all, which leaves that state to the backend's defaults. Topology is the
// exception for vertex pipelines: backends disagree about an omitted one, so it is always given (TriangleList unless set).
struct GraphicsPipelineDesc {
	std::optional<PipelineShaderSource> vertex;
	std::optional<PipelineShaderSource> amplification;
	std::optional<PipelineShaderSource> mesh;
	std::optional<PipelineShaderSource> pixel;
	std::string name;
	std::vector<PipelineShaderDefine> defines;
	PipelineLayoutRef layout = PipelineLayoutRef::StandardGraphics();
	std::optional<rhi::RasterState> raster;
	std::optional<rhi::BlendState> blend;
	std::optional<rhi::DepthStencilState> depth;
	std::optional<rhi::RenderTargets> renderTargets;
	std::optional<rhi::Format> depthFormat;
	std::optional<rhi::SampleDesc> sample;
	std::optional<rhi::PrimitiveTopology> topology;
};

using PipelineDesc = std::variant<ComputePipelineDesc, GraphicsPipelineDesc>;

// A command signature by description: it belongs to a device and names a layout of that device.
struct CommandSignatureDesc {
	std::vector<rhi::IndirectArg> args;
	uint32_t byteStride = 0;
	PipelineLayoutRef layout = PipelineLayoutRef::StandardCompute();
	bool unorderedSequences = false;
	bool explicitPreprocess = false;
	std::string name;
};

class PipelineStateSlot {
public:
	PipelineStateSlot() = default;
	explicit PipelineStateSlot(std::shared_ptr<org::PipelineStatePayload> payload) :
		m_active(payload), m_peek(payload.get()) {}

	std::shared_ptr<org::PipelineStatePayload> Load() const {
		return m_active.load(std::memory_order_acquire);
	}
	// A non-owning identity of the active payload, for revision checks. Retired payloads stay alive through the recordings
	// that bound them, so the pointer is only compared.
	org::PipelineStatePayload* Peek() const noexcept { return m_peek.load(std::memory_order_acquire); }

	std::shared_ptr<org::PipelineStatePayload> Exchange(std::shared_ptr<org::PipelineStatePayload> payload) {
		m_peek.store(payload.get(), std::memory_order_release);
		return m_active.exchange(std::move(payload), std::memory_order_acq_rel);
	}

private:
	std::atomic<std::shared_ptr<org::PipelineStatePayload>> m_active;
	std::atomic<org::PipelineStatePayload*> m_peek{nullptr};
};

// A handle to a pipeline's builds, one slot per device. Copies share the slots. Every slot is atomic and the set is fixed,
// so a build can land on one device while recordings read another, without a lock.
//
// It converts to the primary device's payload, which is what org::RecordingContext::Bind*Pipeline takes. A handle from
// the PipelineLibrary whose build has not landed yet waits for it there (the library installs the wait).
class PipelineState {
public:
	PipelineState(rhi::PipelinePtr pso,
		uint64_t resourceIDsHash,
		org::PipelineResources resources,
		std::shared_ptr<const void> layoutOwner = {},
		rhi::PipelineLayoutHandle layout = {}) :
		m_slots(std::make_shared<Slots>()) {
		auto payload = std::make_shared<org::PipelineStatePayload>(std::move(pso), resourceIDsHash, std::move(resources));
		payload->layoutOwner = std::move(layoutOwner);
		payload->layout = layout;
		m_slots->devices[0].Exchange(std::move(payload));
	}
	PipelineState() :
		m_slots(std::make_shared<Slots>()) {
		m_slots->devices[0].Exchange(std::make_shared<org::PipelineStatePayload>(rhi::PipelinePtr{}, 0, org::PipelineResources{}));
	}

	const rhi::Pipeline& GetAPIPipelineState() const {
		return RequirePayload()->pso.Get();
	}
	const rhi::Pipeline& GetAPIPipelineState(org::BackendInstanceId backendInstance) const {
		return RequirePayload(backendInstance)->pso.Get();
	}
	bool HasBackendPipeline(org::BackendInstanceId backendInstance) const {
		const auto payload = GetPayload(backendInstance);
		return payload && payload->pso;
	}
	void AttachBackendPipeline(org::BackendInstanceId backendInstance, rhi::PipelinePtr pipeline,
		uint64_t resourceIDsHash, org::PipelineResources resources,
		std::shared_ptr<const void> layoutOwner = {}, rhi::PipelineLayoutHandle layout = {}) const {
		auto payload = std::make_shared<org::PipelineStatePayload>(
			std::move(pipeline), resourceIDsHash, std::move(resources));
		payload->layoutOwner = std::move(layoutOwner);
		payload->layout = layout;
		ReplacePayload(backendInstance, std::move(payload));
	}
	uint64_t GetResourceIDsHash() const {
		return RequirePayload()->resourceIDsHash;
	}
	const org::PipelineResources& GetResourceDescriptorSlots() const {
		return RequirePayload()->pipelineResources;
	}
	uint64_t GetGeneration() const {
		const auto payload = GetPayload();
		return payload ? payload->generation : 0;
	}
	uint64_t GetSourceHash() const {
		const auto payload = GetPayload();
		return payload ? payload->sourceHash : 0;
	}
	uint64_t GetBytecodeHash() const {
		const auto payload = GetPayload();
		return payload ? payload->bytecodeHash : 0;
	}
	std::string GetLabel() const {
		const auto payload = GetPayload();
		return payload ? payload->label : std::string{};
	}
	explicit operator bool() const {
		const auto payload = GetPayload();
		return payload && payload->pso;
	}

	std::shared_ptr<org::PipelineStatePayload> GetPayload() const {
		return m_slots ? m_slots->devices[0].Load() : nullptr;
	}
	org::PipelineStatePayload* PeekPayload() const noexcept { return m_slots ? m_slots->devices[0].Peek() : nullptr; }
	std::shared_ptr<org::PipelineStatePayload> GetPayload(org::BackendInstanceId backendInstance) const {
		const size_t index = static_cast<uint8_t>(backendInstance);
		return m_slots && index < kMaxPipelineDevices ? m_slots->devices[index].Load() : nullptr;
	}
	// The primary device's slot. Its address identifies the pipeline: copies of a handle share it.
	std::shared_ptr<PipelineStateSlot> GetSlot() const {
		return m_slots ? std::shared_ptr<PipelineStateSlot>(m_slots, &m_slots->devices[0]) : nullptr;
	}
	std::shared_ptr<org::PipelineStatePayload> ReplacePayload(std::shared_ptr<org::PipelineStatePayload> payload) const {
		return m_slots ? m_slots->devices[0].Exchange(std::move(payload)) : nullptr;
	}
	std::shared_ptr<org::PipelineStatePayload> ReplacePayload(org::BackendInstanceId backendInstance,
		std::shared_ptr<org::PipelineStatePayload> payload) const {
		const size_t index = static_cast<uint8_t>(backendInstance);
		if (index >= kMaxPipelineDevices) throw std::out_of_range("PipelineState: device instance out of range");
		return m_slots ? m_slots->devices[index].Exchange(std::move(payload)) : nullptr;
	}

	// The PipelineLibrary's: what Bound() calls when the primary build has not landed. Set once, before the handle is shared.
	void SetAwaitBuild(std::function<void()> await) const {
		if (m_slots) m_slots->awaitBuild = std::move(await);
	}

	// The primary device's built payload, waiting for a library build in flight. Throws when there is none.
	std::shared_ptr<const org::PipelineStatePayload> Bound() const {
		auto payload = GetPayload();
		if ((!payload || !payload->pso) && m_slots && m_slots->awaitBuild) {
			m_slots->awaitBuild();
			payload = GetPayload();
		}
		if (!payload || !payload->pso) throw std::runtime_error("Pipeline has no build on the primary device");
		return payload;
	}
	// What RecordingContext::Bind*Pipeline takes.
	operator std::shared_ptr<const org::PipelineStatePayload>() const { return Bound(); }

private:
	std::shared_ptr<org::PipelineStatePayload> RequirePayload() const {
		auto payload = GetPayload();
		if (!payload) {
			throw std::runtime_error("PipelineState has no active payload");
		}
		return payload;
	}
	std::shared_ptr<org::PipelineStatePayload> RequirePayload(org::BackendInstanceId backendInstance) const {
		auto payload = GetPayload(backendInstance);
		if (!payload || !payload->pso) {
			throw std::runtime_error("PipelineState has no payload for backend instance " +
				std::to_string(static_cast<uint8_t>(backendInstance)));
		}
		return payload;
	}

	struct Slots {
		std::array<PipelineStateSlot, kMaxPipelineDevices> devices;
		std::function<void()> awaitBuild;
	};
	std::shared_ptr<Slots> m_slots;
};

// A handle to a command signature's creations, one slot per device, as PipelineState is to a pipeline's builds. It
// converts to the primary device's signature, which org::RecordingContext::ExecuteIndirect takes.
class CommandSignatureState {
public:
	CommandSignatureState() : m_slots(std::make_shared<Slots>()) {}
	// A signature the host made, for the primary device only.
	explicit CommandSignatureState(std::shared_ptr<const rhi::CommandSignaturePtr> primary) : CommandSignatureState() {
		Attach(org::BackendInstanceId::Primary, std::move(primary));
	}
	std::shared_ptr<const rhi::CommandSignaturePtr> Get(org::BackendInstanceId device) const {
		const size_t index = static_cast<uint8_t>(device);
		return m_slots && index < kMaxPipelineDevices ? (*m_slots)[index].load(std::memory_order_acquire) : nullptr;
	}
	void Attach(org::BackendInstanceId device, std::shared_ptr<const rhi::CommandSignaturePtr> signature) const {
		const size_t index = static_cast<uint8_t>(device);
		if (index >= kMaxPipelineDevices) throw std::out_of_range("CommandSignatureState: device instance out of range");
		(*m_slots)[index].store(std::move(signature), std::memory_order_release);
	}
	std::shared_ptr<const rhi::CommandSignaturePtr> Primary() const {
		auto signature = Get(org::BackendInstanceId::Primary);
		if (!signature) throw std::runtime_error("Command signature has no creation on the primary device");
		return signature;
	}
	operator std::shared_ptr<const rhi::CommandSignaturePtr>() const { return Primary(); }

private:
	using Slots = std::array<std::atomic<std::shared_ptr<const rhi::CommandSignaturePtr>>, kMaxPipelineDevices>;
	std::shared_ptr<Slots> m_slots;
};

} // namespace br
