#include "Pipeline/PipelineState/PipelineLibrary.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <type_traits>

#include <spdlog/spdlog.h>

#include "Render/Runtime/ITaskService.h"

namespace br {

using org::DeviceInstanceId;
using org::PipelineStatePayload;
namespace runtime = org::runtime;

namespace {

// The declaration scope open on this thread (PipelineDeclarationScope), innermost first.
thread_local PipelineDeclarationScope* t_declarationScope = nullptr;

class ContentHasher {
public:
	template<class T> requires std::is_arithmetic_v<T> || std::is_enum_v<T>
	void Add(T value) {
		// Bytes of the value alone, widened, so padding around it never reaches the hash. -0.0f and 0.0f hash apart, which at
		// worst builds one pipeline twice.
		static_assert(sizeof(T) <= sizeof(uint64_t));
		uint64_t bits = 0;
		std::memcpy(&bits, &value, sizeof(T));
		Mix(bits);
	}
	void Add(const std::string& text) { AddRange(text); }
	void Add(const std::wstring& text) { AddRange(text); }
	uint64_t Value() const noexcept { return m_hash; }

private:
	template<class Text> void AddRange(const Text& text) {
		Mix(text.size());
		for (const auto c : text) Mix(static_cast<uint64_t>(c));
	}
	void Mix(uint64_t value) noexcept { m_hash = (m_hash ^ value) * 0x100000001B3ull; m_hash ^= m_hash >> 29; }
	uint64_t m_hash = 0xCBF29CE484222325ull;
};

void HashShader(ContentHasher& hash, const PipelineShaderSource& shader) {
	hash.Add(shader.path);
	hash.Add(shader.entry);
	hash.Add(shader.target);
}

void HashShader(ContentHasher& hash, const std::optional<PipelineShaderSource>& shader) {
	hash.Add(shader.has_value());
	if (shader) HashShader(hash, *shader);
}

void HashDefines(ContentHasher& hash, const std::vector<PipelineShaderDefine>& defines) {
	hash.Add(defines.size());
	for (const auto& define : defines) {
		hash.Add(define.name);
		hash.Add(define.value);
	}
}

void HashSampler(ContentHasher& hash, const rhi::SamplerDesc& s) {
	hash.Add(s.minFilter); hash.Add(s.magFilter); hash.Add(s.mipFilter);
	hash.Add(s.addressU); hash.Add(s.addressV); hash.Add(s.addressW);
	hash.Add(s.mipLodBias); hash.Add(s.minLod); hash.Add(s.maxLod);
	hash.Add(s.maxAnisotropy); hash.Add(s.compareEnable); hash.Add(s.compareOp); hash.Add(s.reduction);
	hash.Add(s.borderPreset);
	for (const auto channel : s.borderColor) hash.Add(channel);
	hash.Add(s.unnormalizedCoordinates);
}

void HashLayoutSpec(ContentHasher& hash, const PipelineLayoutSpec& spec) {
	hash.Add(spec.flags);
	hash.Add(spec.ranges.size());
	for (const auto& r : spec.ranges) {
		hash.Add(r.set); hash.Add(r.binding); hash.Add(r.count); hash.Add(r.readOnly); hash.Add(r.visibility); hash.Add(r.source);
		hash.Add(r.addressRootIndex); hash.Add(r.addressOffset32); hash.Add(r.recordOffset); hash.Add(r.samplers);
	}
	hash.Add(spec.pushConstants.size());
	for (const auto& p : spec.pushConstants) {
		hash.Add(p.visibility); hash.Add(p.num32BitValues); hash.Add(p.set); hash.Add(p.binding); hash.Add(p.type);
	}
	hash.Add(spec.staticSamplers.size());
	for (const auto& s : spec.staticSamplers) {
		HashSampler(hash, s.sampler);
		hash.Add(s.visibility); hash.Add(s.set); hash.Add(s.binding); hash.Add(s.arrayCount);
	}
}

void HashLayout(ContentHasher& hash, const PipelineLayoutRef& layout) {
	hash.Add(layout.kind);
	if (layout.custom) HashLayoutSpec(hash, *layout.custom);
}

template<class T, class Each>
void HashOptional(ContentHasher& hash, const std::optional<T>& value, Each&& each) {
	hash.Add(value.has_value());
	if (value) each(*value);
}

std::wstring DefaultTarget(rhi::ShaderStage stage) {
	switch (stage) {
	case rhi::ShaderStage::Compute: return L"cs_6_6";
	case rhi::ShaderStage::Vertex: return L"vs_6_6";
	case rhi::ShaderStage::Pixel: return L"ps_6_6";
	case rhi::ShaderStage::Mesh: return L"ms_6_6";
	case rhi::ShaderStage::Task: return L"as_6_6";
	default: throw std::logic_error("No default shader target for this stage");
	}
}

PipelineCompileRequest MakeRequest(const PipelineLibraryEntry& entry, DeviceInstanceId id, rhi::Backend backend,
	const std::vector<PipelineShaderDefine>& overrides, bool bypassCache) {
	PipelineCompileRequest request;
	request.name = entry.Name();
	request.device = id;
	request.backend = backend;
	request.bypassCache = bypassCache;
	const auto addStage = [&](rhi::ShaderStage stage, const PipelineShaderSource& source) {
		auto& added = request.stages.emplace_back(PipelineCompileRequest::Stage{stage, source});
		if (added.source.target.empty()) added.source.target = DefaultTarget(stage);
	};
	std::visit([&](const auto& desc) {
		using T = std::decay_t<decltype(desc)>;
		request.defines = desc.defines;
		if constexpr (std::is_same_v<T, ComputePipelineDesc>) {
			addStage(rhi::ShaderStage::Compute, desc.shader);
		} else {
			if (desc.vertex) addStage(rhi::ShaderStage::Vertex, *desc.vertex);
			if (desc.amplification) addStage(rhi::ShaderStage::Task, *desc.amplification);
			if (desc.mesh) addStage(rhi::ShaderStage::Mesh, *desc.mesh);
			if (desc.pixel) addStage(rhi::ShaderStage::Pixel, *desc.pixel);
		}
	}, entry.Desc());
	for (const auto& define : overrides) {
		const auto existing = std::ranges::find_if(request.defines, [&](const auto& d) { return d.name == define.name; });
		if (existing == request.defines.end()) request.defines.push_back(define);
		else existing->value = define.value;
	}
	return request;
}

size_t CheckedDevice(DeviceInstanceId id) {
	const size_t index = static_cast<uint8_t>(id);
	if (index >= kMaxPipelineDevices) throw std::out_of_range("PipelineLibrary: device instance out of range");
	return index;
}

} // namespace

uint64_t PipelineContentHash(const PipelineDesc& desc) {
	ContentHasher hash;
	hash.Add(desc.index());
	std::visit([&](const auto& value) {
		using T = std::decay_t<decltype(value)>;
		hash.Add(value.name);
		HashDefines(hash, value.defines);
		HashLayout(hash, value.layout);
		if constexpr (std::is_same_v<T, ComputePipelineDesc>) {
			HashShader(hash, value.shader);
		} else {
			HashShader(hash, value.vertex);
			HashShader(hash, value.amplification);
			HashShader(hash, value.mesh);
			HashShader(hash, value.pixel);
			HashOptional(hash, value.raster, [&](const rhi::RasterState& r) {
				hash.Add(r.fill); hash.Add(r.cull); hash.Add(r.frontCCW); hash.Add(r.depthBias);
				hash.Add(r.depthBiasClamp); hash.Add(r.slopeScaledDepthBias); hash.Add(r.conservative);
			});
			HashOptional(hash, value.blend, [&](const rhi::BlendState& b) {
				hash.Add(b.alphaToCoverage); hash.Add(b.independentBlend); hash.Add(b.numAttachments);
				for (const auto& a : b.attachments) {
					hash.Add(a.enable); hash.Add(a.srcColor); hash.Add(a.dstColor); hash.Add(a.colorOp);
					hash.Add(a.srcAlpha); hash.Add(a.dstAlpha); hash.Add(a.alphaOp); hash.Add(a.writeMask);
				}
			});
			HashOptional(hash, value.depth, [&](const rhi::DepthStencilState& d) {
				hash.Add(d.depthEnable); hash.Add(d.depthWrite); hash.Add(d.depthFunc);
			});
			HashOptional(hash, value.renderTargets, [&](const rhi::RenderTargets& t) {
				hash.Add(t.count);
				for (const auto format : t.formats) hash.Add(format);
			});
			HashOptional(hash, value.depthFormat, [&](rhi::Format f) { hash.Add(f); });
			HashOptional(hash, value.sample, [&](const rhi::SampleDesc& s) { hash.Add(s.count); hash.Add(s.quality); });
			HashOptional(hash, value.topology, [&](rhi::PrimitiveTopology t) { hash.Add(t); });
		}
	}, desc);
	return hash.Value();
}

uint64_t CommandSignatureContentHash(const CommandSignatureDesc& desc) {
	ContentHasher hash;
	hash.Add(desc.name);
	hash.Add(desc.byteStride);
	hash.Add(desc.unorderedSequences);
	hash.Add(desc.explicitPreprocess);
	HashLayout(hash, desc.layout);
	hash.Add(desc.args.size());
	for (const auto& arg : desc.args) {
		hash.Add(arg.kind);
		if (arg.kind == rhi::IndirectArgKind::VertexBuffer) hash.Add(arg.u.vertexBuffer.slot);
		if (arg.kind == rhi::IndirectArgKind::Constant) {
			hash.Add(arg.u.rootConstants.rootIndex);
			hash.Add(arg.u.rootConstants.destOffset32);
			hash.Add(arg.u.rootConstants.num32);
		}
	}
	return hash.Value();
}

PipelineBuildStatus PipelineLibraryEntry::Status(DeviceInstanceId device) const noexcept {
	const size_t index = static_cast<uint8_t>(device);
	if (index >= kMaxPipelineDevices) return PipelineBuildStatus::Idle;
	return static_cast<PipelineBuildStatus>(m_builds[index].status.load(std::memory_order_acquire));
}

std::string PipelineLibraryEntry::Error(DeviceInstanceId device) const {
	return Status(device) == PipelineBuildStatus::Failed ? m_builds[static_cast<uint8_t>(device)].error : std::string{};
}

PipelineLibrary::PipelineLibrary(std::shared_ptr<runtime::ITaskService> tasks, std::shared_ptr<PipelineCompiler> compiler)
	: m_tasks(std::move(tasks)), m_compiler(std::move(compiler)) {
	if (m_tasks) m_scope = m_tasks->CreateScope("BR.Pipelines");
}

PipelineLibrary::~PipelineLibrary() {
	if (m_scope) m_scope->CancelAndWait();
	// A compiler may complete on threads of its own.
	for (auto inFlight = m_compilesInFlight.load(); inFlight; inFlight = m_compilesInFlight.load())
		m_compilesInFlight.wait(inFlight);
}

void PipelineLibrary::RegisterDevice(DeviceInstanceId id, rhi::Backend backend, rhi::Device device) {
	const auto index = CheckedDevice(id);
	std::scoped_lock lock(m_mutex);
	m_devices[index] = {backend, device};
}

PipelineLibrary::DeviceInfo PipelineLibrary::FindDevice(DeviceInstanceId id) {
	const auto index = CheckedDevice(id);
	std::scoped_lock lock(m_mutex);
	return m_devices[index];
}

PipelineLibraryEntryPtr PipelineLibrary::Acquire(PipelineDesc desc) {
	if (!m_compiler) throw std::logic_error("Declaring a pipeline needs a PipelineCompiler");
	const auto key = PipelineContentHash(desc);
	std::scoped_lock lock(m_mutex);
	auto& slot = m_entries[key];
	if (slot) return slot;
	auto entry = std::make_shared<PipelineLibraryEntry>();
	entry->m_name = std::visit([](const auto& value) { return value.name; }, desc);
	entry->m_desc = std::move(desc);
	entry->m_key = key;
	// A recording that binds the handle before its build landed waits for it (PipelineState::Bound). The library outlives
	// every recording, and the entry outlives its handle's slots.
	std::weak_ptr<PipelineLibraryEntry> weak = entry;
	entry->m_state.SetAwaitBuild([this, weak] {
		if (auto locked = weak.lock()) {
			Request(locked, DeviceInstanceId::Primary);
			Wait(locked, DeviceInstanceId::Primary);
		}
	});
	slot = entry;
	return entry;
}

PipelineState PipelineLibrary::Declare(PipelineDesc desc) {
	auto entry = Acquire(std::move(desc));
	Request(entry, DeviceInstanceId::Primary);
	if (auto* scope = t_declarationScope; scope && scope->m_library == this) {
		scope->m_declared.push_back(entry);
		return entry->State();
	}
	m_declaredOutsideScopes.fetch_add(1, std::memory_order_relaxed);
	{
		std::scoped_lock lock(m_mutex);
		m_declared.push_back(entry);
	}
	return entry->State();
}

CommandSignatureState PipelineLibrary::DeclareCommandSignature(CommandSignatureDesc desc) {
	auto entry = AcquireCommandSignature(std::move(desc));
	EnsureCommandSignature(entry, DeviceInstanceId::Primary);
	return entry->State();
}

std::vector<std::string> PipelineLibrary::AwaitDeclared() {
	std::vector<PipelineLibraryEntryPtr> declared;
	{
		std::scoped_lock lock(m_mutex);
		declared.swap(m_declared);
	}
	std::vector<std::string> failures;
	for (const auto& entry : declared) {
		if (Wait(entry, DeviceInstanceId::Primary) == PipelineBuildStatus::Failed)
			failures.push_back("Pipeline '" + entry->Name() + "' failed to build: " + entry->Error(DeviceInstanceId::Primary));
	}
	return failures;
}

std::vector<std::string> PipelineLibrary::DropSettledDeclared() {
	std::vector<std::string> failures;
	std::scoped_lock lock(m_mutex);
	std::erase_if(m_declared, [&](const PipelineLibraryEntryPtr& entry) {
		const auto status = entry->Status(DeviceInstanceId::Primary);
		if (status == PipelineBuildStatus::Building) return false;
		if (status == PipelineBuildStatus::Failed)
			failures.push_back("Pipeline '" + entry->Name() + "' failed to build: " + entry->Error(DeviceInstanceId::Primary));
		return true;
	});
	return failures;
}

PipelineDeclarationScope::PipelineDeclarationScope(PipelineLibrary* library) noexcept
	: m_library(library), m_previous(t_declarationScope) {
	t_declarationScope = this;
}

PipelineDeclarationScope::~PipelineDeclarationScope() {
	t_declarationScope = m_previous;
}

std::vector<std::string> PipelineDeclarationScope::Await() {
	std::vector<std::string> failures;
	if (!m_library) return failures;
	for (const auto& entry : m_declared) {
		if (m_library->Wait(entry, DeviceInstanceId::Primary) == PipelineBuildStatus::Failed)
			failures.push_back("Pipeline '" + entry->Name() + "' failed to build: " + entry->Error(DeviceInstanceId::Primary));
	}
	return failures;
}

bool PipelineLibrary::DeclaredSettled() {
	std::scoped_lock lock(m_mutex);
	for (const auto& entry : m_declared)
		if (entry->Status(DeviceInstanceId::Primary) == PipelineBuildStatus::Building) return false;
	return true;
}

CommandSignatureLibraryEntryPtr PipelineLibrary::AcquireCommandSignature(CommandSignatureDesc desc) {
	if (!m_compiler) throw std::logic_error("Declaring a command signature needs a PipelineCompiler");
	const auto key = CommandSignatureContentHash(desc);
	std::scoped_lock lock(m_mutex);
	auto& slot = m_commandSignatures[key];
	if (slot) return slot;
	auto entry = std::make_shared<CommandSignatureLibraryEntry>();
	entry->m_desc = std::move(desc);
	slot = entry;
	return entry;
}

void PipelineLibrary::EnsureCommandSignature(const CommandSignatureLibraryEntryPtr& entry, DeviceInstanceId id) {
	if (!entry) throw std::invalid_argument("PipelineLibrary::EnsureCommandSignature: no command signature");
	if (entry->m_state.Get(id)) return;
	const auto device = FindDevice(id);
	if (!device.device) throw std::runtime_error("Command signature '" + entry->m_desc.name + "': device instance is not registered");
	const auto& desc = entry->m_desc;
	const auto layout = ResolveLayout(desc.layout, id, device);
	struct Holder {
		PipelineLayoutBinding layout;
		rhi::CommandSignaturePtr signature; // Destroyed before the layout it names.
	};
	auto holder = std::make_shared<Holder>();
	holder->layout = layout;
	rhi::CommandSignatureDesc create{};
	create.args = rhi::Span<rhi::IndirectArg>(desc.args.data(), static_cast<uint32_t>(desc.args.size()));
	create.byteStride = desc.byteStride;
	create.unorderedSequences = desc.unorderedSequences;
	create.explicitPreprocess = desc.explicitPreprocess;
	auto mutableDevice = device.device;
	if (rhi::Failed(mutableDevice.CreateCommandSignature(create, layout.layout, holder->signature)))
		throw std::runtime_error("Failed to create command signature '" + desc.name + "'");
	entry->m_state.Attach(id, std::shared_ptr<const rhi::CommandSignaturePtr>(holder, &holder->signature));
}

void PipelineLibrary::Request(const PipelineLibraryEntryPtr& entry, DeviceInstanceId device) {
	if (!entry) throw std::invalid_argument("PipelineLibrary::Request: no pipeline");
	auto& status = entry->m_builds[CheckedDevice(device)].status;
	auto expected = static_cast<uint8_t>(PipelineBuildStatus::Idle);
	if (!status.compare_exchange_strong(expected, static_cast<uint8_t>(PipelineBuildStatus::Building), std::memory_order_acq_rel))
		return;
	StartBuild(entry, device, {}, false, 1, {}, [entry, device, &status](std::shared_ptr<PipelineStatePayload> payload, std::string error) {
		if (payload) {
			entry->m_state.ReplacePayload(device, std::move(payload));
			status.store(static_cast<uint8_t>(PipelineBuildStatus::Ready), std::memory_order_release);
		} else {
			entry->m_builds[static_cast<uint8_t>(device)].error = std::move(error);
			status.store(static_cast<uint8_t>(PipelineBuildStatus::Failed), std::memory_order_release);
		}
		status.notify_all();
	});
}

PipelineBuildStatus PipelineLibrary::Wait(const PipelineLibraryEntryPtr& entry, DeviceInstanceId device) const {
	const auto& status = entry->m_builds[CheckedDevice(device)].status;
	status.wait(static_cast<uint8_t>(PipelineBuildStatus::Building), std::memory_order_acquire);
	return static_cast<PipelineBuildStatus>(status.load(std::memory_order_acquire));
}

void PipelineLibrary::StartBuild(PipelineLibraryEntryPtr entry, DeviceInstanceId id, std::vector<PipelineShaderDefine> overrides,
	bool bypassCache, uint64_t generation, std::string label, BuildCompletion completion) {
	auto run = [this, entry, id, overrides = std::move(overrides), bypassCache, generation, label = std::move(label),
		completion = std::move(completion)]() mutable {
		auto shared = std::make_shared<BuildCompletion>(std::move(completion));
		m_compilesInFlight.fetch_add(1, std::memory_order_acq_rel);
		const auto finish = [this, shared](std::shared_ptr<PipelineStatePayload> payload, std::string error) {
			(*shared)(std::move(payload), std::move(error));
			m_buildsPending.fetch_sub(1, std::memory_order_acq_rel);
			if (m_compilesInFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) m_compilesInFlight.notify_all();
		};
		std::shared_ptr<std::atomic<bool>> completed = std::make_shared<std::atomic<bool>>(false);
		try {
			const auto device = FindDevice(id);
			if (!device.device) throw std::runtime_error("device instance " + std::to_string(static_cast<uint8_t>(id)) + " is not registered");
			auto request = std::make_shared<PipelineCompileRequest>(MakeRequest(*entry, id, device.backend, overrides, bypassCache));
			m_compiler->Compile(*request, [this, entry, device, request, finish, completed, generation, label](CompiledPipeline compiled) {
				completed->store(true, std::memory_order_release);
				std::shared_ptr<PipelineStatePayload> payload;
				std::string error = std::move(compiled.error);
				if (error.empty()) {
					try {
						payload = CreatePipeline(*entry, *request, device, compiled, generation, label);
					} catch (const std::exception& e) {
						error = e.what();
					}
				}
				finish(std::move(payload), std::move(error));
			});
		} catch (const std::exception& e) {
			if (!completed->load(std::memory_order_acquire)) finish(nullptr, e.what());
			else spdlog::error("PipelineCompiler threw after completing '{}': {}", entry->Name(), e.what());
		}
	};
	auto job = std::make_shared<decltype(run)>(std::move(run));
	m_buildsPending.fetch_add(1, std::memory_order_acq_rel); // Until finish: from here, so a queued build counts.
	if (!m_tasks || !m_tasks->Submit(m_scope, runtime::TaskPriority::Streaming, "BR.Pipeline.Build", [job] { (*job)(); })) (*job)();
}

PipelineLayoutBinding PipelineLibrary::ResolveLayout(const PipelineLayoutRef& layout, DeviceInstanceId id, const DeviceInfo& device) {
	if (layout.kind != PipelineLayoutKind::Custom) return m_compiler->StandardLayout(layout.kind, id);
	if (!layout.custom) throw std::invalid_argument("A custom pipeline layout has no description");
	const auto& spec = *layout.custom;
	ContentHasher hash;
	HashLayoutSpec(hash, spec);
	const std::pair key{static_cast<uint8_t>(id), hash.Value()};
	std::scoped_lock lock(m_mutex);
	auto& cached = m_customLayouts[key];
	auto owner = cached.lock();
	if (!owner) {
		owner = std::make_shared<rhi::PipelineLayoutPtr>();
		rhi::PipelineLayoutDesc desc{};
		desc.ranges = rhi::Span<rhi::LayoutBindingRange>(spec.ranges.data(), static_cast<uint32_t>(spec.ranges.size()));
		desc.pushConstants = rhi::Span<rhi::PushConstantRangeDesc>(spec.pushConstants.data(), static_cast<uint32_t>(spec.pushConstants.size()));
		desc.staticSamplers = rhi::Span<rhi::StaticSamplerDesc>(spec.staticSamplers.data(), static_cast<uint32_t>(spec.staticSamplers.size()));
		desc.flags = spec.flags;
		auto mutableDevice = device.device;
		if (rhi::Failed(mutableDevice.CreatePipelineLayout(desc, *owner))) throw std::runtime_error("Failed to create a custom pipeline layout");
		if (!spec.name.empty()) (*owner)->SetName(spec.name.c_str());
		cached = owner;
	}
	const auto handle = owner->Get().GetHandle();
	return {handle, std::move(owner)};
}

std::shared_ptr<PipelineStatePayload> PipelineLibrary::CreatePipeline(const PipelineLibraryEntry& entry, const PipelineCompileRequest& request,
	const DeviceInfo& device, const CompiledPipeline& compiled, uint64_t generation, std::string label) {
	if (compiled.stages.size() != request.stages.size()) throw std::logic_error("PipelineCompiler returned the wrong number of stages");
	const auto layout = std::visit([&](const auto& desc) { return ResolveLayout(desc.layout, request.device, device); }, entry.Desc());
	if (!layout.layout.valid()) throw std::runtime_error("The pipeline's layout is not valid on this device");

	rhi::SubobjLayout layoutItem{layout.layout};
	std::vector<rhi::SubobjShader> shaders;
	shaders.reserve(request.stages.size());
	std::vector<rhi::PipelineStreamItem> items;
	items.push_back(rhi::Make(layoutItem));
	for (size_t i = 0; i < request.stages.size(); ++i) {
		const auto& stage = request.stages[i];
		std::string entryPoint;
		entryPoint.reserve(stage.source.entry.size());
		for (const auto c : stage.source.entry) entryPoint.push_back(static_cast<char>(c)); // Entry points are ASCII identifiers.
		shaders.push_back({stage.stage, compiled.stages[i], std::move(entryPoint)});
		items.push_back(rhi::Make(shaders.back()));
	}

	rhi::SubobjRaster raster;
	rhi::SubobjBlend blend;
	rhi::SubobjDepth depth;
	rhi::SubobjRTVs targets;
	rhi::SubobjDSV depthFormat;
	rhi::SubobjSample sample;
	rhi::SubobjPrimitiveTopology topology;
	if (const auto* graphics = std::get_if<GraphicsPipelineDesc>(&entry.Desc())) {
		if (graphics->raster) { raster.rs = *graphics->raster; items.push_back(rhi::Make(raster)); }
		if (graphics->blend) { blend.bs = *graphics->blend; items.push_back(rhi::Make(blend)); }
		if (graphics->depth) { depth.ds = *graphics->depth; items.push_back(rhi::Make(depth)); }
		if (graphics->renderTargets) { targets.rt = *graphics->renderTargets; items.push_back(rhi::Make(targets)); }
		if (graphics->depthFormat) { depthFormat.dsv = *graphics->depthFormat; items.push_back(rhi::Make(depthFormat)); }
		if (graphics->sample) { sample.sd = *graphics->sample; items.push_back(rhi::Make(sample)); }
		if (graphics->topology || !graphics->mesh) {
			topology.pt = graphics->topology.value_or(rhi::PrimitiveTopology::TriangleList);
			items.push_back(rhi::Make(topology));
		}
	}

	rhi::PipelinePtr pipeline;
	auto mutableDevice = device.device;
	if (rhi::Failed(mutableDevice.CreatePipeline(items.data(), static_cast<uint32_t>(items.size()), pipeline)))
		throw std::runtime_error("CreatePipeline failed");
	const std::string debugName = entry.Name().empty() ? std::string("BR pipeline") : entry.Name();
	pipeline->SetName(debugName.c_str());

	auto payload = std::make_shared<PipelineStatePayload>(std::move(pipeline), compiled.resourceIDsHash, compiled.resources,
		generation, compiled.sourceHash, compiled.bytecodeHash, !label.empty() ? std::move(label) : generation == 1 ? std::string("initial") : "generation-" + std::to_string(generation));
	payload->layoutOwner = layout.owner;
	payload->layout = layout.layout;
	return payload;
}

std::vector<PipelineLibraryEntryPtr> PipelineLibrary::LiveEntries() {
	std::vector<PipelineLibraryEntryPtr> live;
	std::scoped_lock lock(m_mutex);
	live.reserve(m_entries.size());
	for (const auto& [key, entry] : m_entries) live.push_back(entry);
	return live;
}

uint64_t PipelineLibrary::Rebuild(const PipelineLibraryEntryPtr& entry, PipelineRebuildOptions options) {
	if (!entry) throw std::invalid_argument("PipelineLibrary::Rebuild: no pipeline");
	std::vector<DeviceInstanceId> devices;
	for (size_t i = 0; i < kMaxPipelineDevices; ++i) {
		const DeviceInstanceId id{static_cast<uint8_t>(i)};
		const auto status = entry->Status(id);
		if (status != PipelineBuildStatus::Idle) devices.push_back(id);
	}
	const auto ticket = entry->m_rebuildTicket.fetch_add(1, std::memory_order_acq_rel) + 1;
	const auto generation = ticket + 1; // The first build is generation 1.
	if (devices.empty()) {
		if (options.completed) options.completed({});
		return generation;
	}
	struct Group {
		FinishedRebuild finished;
		std::atomic<uint32_t> remaining{0};
		std::mutex errorMutex;
		std::string error;
		std::function<void(const std::string&)> completed;
		std::vector<DeviceInstanceId> devices;
	};
	auto group = std::make_shared<Group>();
	group->finished.entry = entry;
	group->finished.ticket = ticket;
	group->finished.generation = generation;
	group->remaining.store(static_cast<uint32_t>(devices.size()));
	group->completed = std::move(options.completed);
	group->devices = devices;
	for (const auto id : devices) {
		StartBuild(entry, id, options.defineOverrides, options.bypassCache, generation, options.label,
			[this, group, id, allowInterfaceChange = options.allowInterfaceChange](std::shared_ptr<PipelineStatePayload> payload, std::string error) {
				if (payload) {
					const auto active = group->finished.entry->m_state.GetPayload(id);
					if (!allowInterfaceChange && active && active->pso && (active->resourceIDsHash != payload->resourceIDsHash
						|| active->pipelineResources.mandatoryResourceDescriptorSlots != payload->pipelineResources.mandatoryResourceDescriptorSlots
						|| active->pipelineResources.optionalResourceDescriptorSlots != payload->pipelineResources.optionalResourceDescriptorSlots))
						error = "the shader's resource interface changed; a rebuild cannot change it";
					else group->finished.payloads[static_cast<uint8_t>(id)] = std::move(payload);
				}
				if (!error.empty()) {
					std::scoped_lock lock(group->errorMutex);
					if (group->error.empty()) group->error = std::move(error);
				}
				if (group->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
				auto& entry = *group->finished.entry;
				if (entry.m_rebuildTicket.load(std::memory_order_acquire) != group->finished.ticket) return; // Superseded.
				if (group->error.empty()) {
					std::scoped_lock lock(m_mutex);
					m_finishedRebuilds.push_back(std::move(group->finished));
					m_rebuildsFinished.store(true, std::memory_order_release);
				} else {
					spdlog::error("Pipeline '{}' failed to rebuild, keeping the old one: {}", entry.Name(), group->error);
				}
				if (group->completed) group->completed(group->error);
			});
	}
	return generation;
}

bool PipelineLibrary::Activate(const PipelineLibraryEntryPtr& entry, uint64_t generation) {
	if (!entry) throw std::invalid_argument("PipelineLibrary::Activate: no pipeline");
	std::scoped_lock lock(m_mutex);
	const auto found = std::ranges::find_if(entry->m_history, [&](const auto& retained) { return retained.generation == generation; });
	if (found == entry->m_history.end()) return false;
	FinishedRebuild activation;
	activation.entry = entry;
	activation.generation = generation;
	activation.activation = true;
	activation.payloads = found->payloads;
	m_finishedRebuilds.push_back(std::move(activation));
	m_rebuildsFinished.store(true, std::memory_order_release);
	return true;
}

std::vector<uint64_t> PipelineLibrary::RetainedGenerations(const PipelineLibraryEntryPtr& entry) {
	std::vector<uint64_t> generations;
	std::scoped_lock lock(m_mutex);
	for (const auto& retained : entry->m_history) generations.push_back(retained.generation);
	return generations;
}

size_t PipelineLibrary::PublishRebuilt() {
	if (!m_rebuildsFinished.load(std::memory_order_acquire)) return 0;
	std::vector<FinishedRebuild> finished;
	{
		std::scoped_lock lock(m_mutex);
		finished.swap(m_finishedRebuilds);
		m_rebuildsFinished.store(false, std::memory_order_release);
	}
	size_t published = 0;
	constexpr size_t kRetainedGenerations = 8;
	for (auto& rebuild : finished) {
		auto& entry = *rebuild.entry;
		if (!rebuild.activation && entry.m_rebuildTicket.load(std::memory_order_acquire) != rebuild.ticket) continue;
		if (!rebuild.activation) {
			std::scoped_lock lock(m_mutex);
			if (entry.m_history.empty()) {
				// The first rebuild's old payloads are the generation it can be rolled back to.
				PipelineLibraryEntry::Generation first;
				for (size_t i = 0; i < kMaxPipelineDevices; ++i) {
					const auto active = entry.m_state.GetPayload(DeviceInstanceId{static_cast<uint8_t>(i)});
					if (active && active->pso) {
						first.payloads[i] = active;
						first.generation = active->generation;
					}
				}
				if (first.generation) entry.m_history.push_back(std::move(first));
			}
			entry.m_history.push_back({rebuild.generation, rebuild.payloads});
			while (entry.m_history.size() > kRetainedGenerations) entry.m_history.pop_front();
		}
		for (size_t i = 0; i < kMaxPipelineDevices; ++i) {
			if (!rebuild.payloads[i]) continue;
			const DeviceInstanceId id{static_cast<uint8_t>(i)};
			entry.m_state.ReplacePayload(id, std::move(rebuild.payloads[i]));
			auto& status = entry.m_builds[i].status;
			status.store(static_cast<uint8_t>(PipelineBuildStatus::Ready), std::memory_order_release);
			status.notify_all();
		}
		++published;
	}
	return published;
}

} // namespace br

#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "Pipeline/PipelineState/PSOManager.h"

namespace br::render {

br::PipelineState DeclarePipeline(br::PipelineDesc desc) {
	auto* library = PSOManager::GetInstance().GetPipelineLibrary();
	if (!library) throw std::logic_error("DeclarePipeline: the renderer has no pipeline library yet");
	return library->Declare(std::move(desc));
}

br::CommandSignatureState DeclareCommandSignature(br::CommandSignatureDesc desc) {
	auto* library = PSOManager::GetInstance().GetPipelineLibrary();
	if (!library) throw std::logic_error("DeclareCommandSignature: the renderer has no pipeline library yet");
	return library->DeclareCommandSignature(std::move(desc));
}

} // namespace br::render
