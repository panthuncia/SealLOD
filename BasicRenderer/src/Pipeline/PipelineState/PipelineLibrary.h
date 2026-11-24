#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <BasicRenderer/Pipeline/PipelineDesc.h>

namespace org::runtime { class ITaskScope; class ITaskService; }

// Builds pipelines from descriptions (br::PipelineDesc), for any device, on the task service. ORG used to own this; it now
// builds no pipelines, so BasicRenderer does. Passes declare pipelines with br::render::DeclarePipeline; the renderer waits
// for every pipeline a graph build declared before the graph's first frame (AwaitDeclared).
namespace br {

enum class PipelineBuildStatus : uint8_t { Idle, Building, Ready, Failed };

// What the library asks the host to compile: the shaders of one pipeline for one device. Defines apply to every stage.
struct PipelineCompileRequest {
	struct Stage {
		rhi::ShaderStage stage{};
		PipelineShaderSource source;
	};
	std::vector<Stage> stages;
	std::vector<PipelineShaderDefine> defines;
	std::string name;
	org::DeviceInstanceId device = org::DeviceInstanceId::Primary;
	rhi::Backend backend = rhi::Backend::Null;
	// A reload: the caller knows the source changed, so a cached artifact must not answer it.
	bool bypassCache = false;
};

// The compiled stages, parallel to the request's. A non-empty error means the compile failed; the rest is then unused.
struct CompiledPipeline {
	std::vector<rhi::ShaderBinary> stages;
	std::shared_ptr<const void> codeOwner; // Keeps the stages' code alive until the pipeline is created.
	org::PipelineResources resources;
	uint64_t resourceIDsHash = 0;
	uint64_t sourceHash = 0;
	uint64_t bytecodeHash = 0;
	std::string error;
};

struct PipelineLayoutBinding {
	rhi::PipelineLayoutHandle layout{};
	std::shared_ptr<const void> owner; // Must outlive every pipeline created with the layout.
};

// The shader compiler side of pipeline construction. Called on task service threads.
class PipelineCompiler {
public:
	using Completion = std::function<void(CompiledPipeline)>;
	virtual ~PipelineCompiler() = default;
	// Compiles for request.backend. Completion runs exactly once, on any thread; it may run before Compile returns. An
	// exception thrown before it runs counts as a failed compile.
	virtual void Compile(const PipelineCompileRequest& request, Completion completion) = 0;
	// The host's layout of that kind on a device. Throws when there is none.
	virtual PipelineLayoutBinding StandardLayout(PipelineLayoutKind kind, org::DeviceInstanceId device) = 0;
};

// A pipeline of the library: its description, its handle and its build on each device.
class PipelineLibraryEntry {
public:
	const PipelineDesc& Desc() const noexcept { return m_desc; }
	const std::string& Name() const noexcept { return m_name; }
	const PipelineState& State() const noexcept { return m_state; }
	uint64_t Key() const noexcept { return m_key; }
	PipelineBuildStatus Status(org::DeviceInstanceId device) const noexcept;
	// Why the device's build failed. Empty unless Status is Failed.
	std::string Error(org::DeviceInstanceId device) const;

private:
	friend class PipelineLibrary;
	struct DeviceBuild {
		std::atomic<uint8_t> status{static_cast<uint8_t>(PipelineBuildStatus::Idle)};
		std::string error; // Written before a Failed status is published.
	};
	PipelineDesc m_desc;
	std::string m_name;
	uint64_t m_key = 0;
	PipelineState m_state;
	std::array<DeviceBuild, kMaxPipelineDevices> m_builds;
	std::atomic<uint64_t> m_rebuildTicket{0}; // A newer Rebuild supersedes one still in flight.
	struct Generation {
		uint64_t generation = 0;
		std::array<std::shared_ptr<org::PipelineStatePayload>, kMaxPipelineDevices> payloads;
	};
	std::deque<Generation> m_history; // The retained generations, oldest first. Guarded by the library's mutex.
};
using PipelineLibraryEntryPtr = std::shared_ptr<PipelineLibraryEntry>;

class CommandSignatureLibraryEntry {
public:
	const CommandSignatureDesc& Desc() const noexcept { return m_desc; }
	const CommandSignatureState& State() const noexcept { return m_state; }

private:
	friend class PipelineLibrary;
	CommandSignatureDesc m_desc;
	CommandSignatureState m_state;
};
using CommandSignatureLibraryEntryPtr = std::shared_ptr<CommandSignatureLibraryEntry>;

struct PipelineRebuildOptions {
	std::vector<PipelineShaderDefine> defineOverrides; // Replace a define of the same name, or add one.
	bool bypassCache = true;
	std::string label; // The payloads' label. Empty: "generation-N" ("initial" for a first build).
	// A rebuild the host follows by declaring the passes again (a global reload rebuilds its graphs), so the new shader's
	// resource interface can differ.
	bool allowInterfaceChange = false;
	// Once every device's build ended, on the last build's thread. On success, the result publishes at the next PublishRebuilt.
	// Empty error: success. A superseded rebuild reports nothing.
	std::function<void(const std::string& error)> completed;
};

// Handles are valid from Acquire. A device's payload appears in the handle when its build ends, lock-free, so a recording
// reads it without coordinating with a build. Entries live as long as the library: a pass's handle may be bound by any
// later recording, and a reload rebuilds every live entry.
class PipelineLibrary {
public:
	PipelineLibrary(std::shared_ptr<org::runtime::ITaskService> tasks, std::shared_ptr<PipelineCompiler> compiler);
	PipelineLibrary(const PipelineLibrary&) = delete;
	PipelineLibrary& operator=(const PipelineLibrary&) = delete;
	// Joins every build.
	~PipelineLibrary();

	// A device pipelines can be built on. Registering one again is harmless.
	void RegisterDevice(org::DeviceInstanceId id, rhi::Backend backend, rhi::Device device);
	bool HasCompiler() const noexcept { return m_compiler != nullptr; }

	// The pipeline with this description; the same one while the library lives. Compiles nothing.
	PipelineLibraryEntryPtr Acquire(PipelineDesc desc);
	// Starts the device's build unless one started already. A failed build stays failed until a Rebuild succeeds.
	void Request(const PipelineLibraryEntryPtr& entry, org::DeviceInstanceId device);
	// Blocks until the device's build ended. Idle when it was never requested.
	PipelineBuildStatus Wait(const PipelineLibraryEntryPtr& entry, org::DeviceInstanceId device) const;

	// A pass's declaration: Acquire, then the primary device's build is requested, and the entry is noted for AwaitDeclared.
	PipelineState Declare(PipelineDesc desc);
	// The command signature with this description, created on the primary device.
	CommandSignatureState DeclareCommandSignature(CommandSignatureDesc desc);
	// Waits for every pipeline declared since the last call. Returns the failures, one line each.
	std::vector<std::string> AwaitDeclared();
	// Whether every pipeline declared since the last AwaitDeclared has finished building (ready or failed).
	bool DeclaredSettled();
	// Drops the declarations outside a scope whose builds ended, so the list stays short when nothing awaits it (a host that
	// awaits per build, PipelineDeclarationScope). Returns the failures among them, one line each. Never blocks.
	std::vector<std::string> DropSettledDeclared();
	// Declarations made outside a PipelineDeclarationScope since the library was made: on a host that builds its graphs in
	// scopes, pipelines a running graph declared (a pass redeclared at preparation), which its recording may wait for.
	uint64_t DeclaredOutsideScopes() const noexcept { return m_declaredOutsideScopes.load(std::memory_order_relaxed); }

	// The command signature with this description; the same one while the library lives. Creates nothing.
	CommandSignatureLibraryEntryPtr AcquireCommandSignature(CommandSignatureDesc desc);
	// Creates it for the device unless it exists. Quick: nothing compiles. Throws on failure.
	void EnsureCommandSignature(const CommandSignatureLibraryEntryPtr& entry, org::DeviceInstanceId device);

	// Live entries, for a host's reload (which shader changed is its knowledge).
	std::vector<PipelineLibraryEntryPtr> LiveEntries();
	// Builds the pipeline again on every device it was requested for. Nothing changes until PublishRebuilt, and on all of its
	// devices at once; a failure on any keeps the old payloads and reports through options.completed. So does a changed resource
	// interface. Returns the generation it will publish as, the same on every device.
	uint64_t Rebuild(const PipelineLibraryEntryPtr& entry, PipelineRebuildOptions options);
	// Queues a retained generation to become active at the next PublishRebuilt. False when it is no longer retained.
	bool Activate(const PipelineLibraryEntryPtr& entry, uint64_t generation);
	std::vector<uint64_t> RetainedGenerations(const PipelineLibraryEntryPtr& entry);
	// Publishes the finished rebuilds and activations into their handles; returns how many. Recordings retain what they bound,
	// so the old payloads go when the last one retires. Called between frames.
	size_t PublishRebuilt();
	// Builds requested and not yet finished, queued ones included.
	uint32_t BuildsPending() const noexcept { return m_buildsPending.load(std::memory_order_acquire); }

private:
	friend class PipelineDeclarationScope;
	struct DeviceInfo {
		rhi::Backend backend = rhi::Backend::Null;
		rhi::Device device{};
	};
	struct FinishedRebuild {
		PipelineLibraryEntryPtr entry;
		uint64_t ticket = 0;
		uint64_t generation = 0;
		bool activation = false; // A retained generation again: already in the history, and no build to be superseded.
		std::array<std::shared_ptr<org::PipelineStatePayload>, kMaxPipelineDevices> payloads;
	};
	using BuildCompletion = std::function<void(std::shared_ptr<org::PipelineStatePayload>, std::string error)>;

	DeviceInfo FindDevice(org::DeviceInstanceId id);
	void StartBuild(PipelineLibraryEntryPtr entry, org::DeviceInstanceId id, std::vector<PipelineShaderDefine> overrides,
		bool bypassCache, uint64_t generation, std::string label, BuildCompletion completion);
	std::shared_ptr<org::PipelineStatePayload> CreatePipeline(const PipelineLibraryEntry& entry, const PipelineCompileRequest& request,
		const DeviceInfo& device, const CompiledPipeline& compiled, uint64_t generation, std::string label);
	PipelineLayoutBinding ResolveLayout(const PipelineLayoutRef& layout, org::DeviceInstanceId id, const DeviceInfo& device);

	std::shared_ptr<org::runtime::ITaskService> m_tasks;
	std::shared_ptr<PipelineCompiler> m_compiler;
	std::shared_ptr<org::runtime::ITaskScope> m_scope;
	std::atomic<uint32_t> m_compilesInFlight{0};
	std::atomic<uint32_t> m_buildsPending{0};

	std::mutex m_mutex; // Declarations and device registration, and the build's start. Never held across a compile.
	std::array<DeviceInfo, kMaxPipelineDevices> m_devices;
	std::unordered_map<uint64_t, PipelineLibraryEntryPtr> m_entries;
	std::unordered_map<uint64_t, CommandSignatureLibraryEntryPtr> m_commandSignatures;
	std::map<std::pair<uint8_t, uint64_t>, std::weak_ptr<rhi::PipelineLayoutPtr>> m_customLayouts;
	std::vector<PipelineLibraryEntryPtr> m_declared; // Since the last AwaitDeclared. Guarded by m_mutex.
	std::atomic<uint64_t> m_declaredOutsideScopes{0};

	std::atomic<bool> m_rebuildsFinished{false};
	std::vector<FinishedRebuild> m_finishedRebuilds; // Guarded by m_mutex.
};

// The pipelines one graph build declares. While a scope is open on a thread, the library's Declare on that thread notes its
// pipelines here instead of in the library-wide list. So a build waits for its own pipelines only, on its own thread, and
// declarations made elsewhere meanwhile (the render thread's) are neither waited on nor taken. A graph build declares its
// pipelines on the thread that builds it (pass Setup is serial).
class PipelineDeclarationScope {
public:
	// A null library opens a scope that notes nothing.
	explicit PipelineDeclarationScope(PipelineLibrary* library) noexcept;
	~PipelineDeclarationScope();
	PipelineDeclarationScope(const PipelineDeclarationScope&) = delete;
	PipelineDeclarationScope& operator=(const PipelineDeclarationScope&) = delete;

	// Waits for the pipelines declared in the scope so far. Returns the failures, one line each.
	std::vector<std::string> Await();
	size_t Declared() const noexcept { return m_declared.size(); }

private:
	friend class PipelineLibrary;
	PipelineLibrary* m_library = nullptr;
	PipelineDeclarationScope* m_previous = nullptr;
	std::vector<PipelineLibraryEntryPtr> m_declared;
};

// A description's identity: everything that affects the pipeline.
uint64_t PipelineContentHash(const PipelineDesc& desc);
uint64_t CommandSignatureContentHash(const CommandSignatureDesc& desc);

} // namespace br
