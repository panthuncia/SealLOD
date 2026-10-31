#include <BasicRenderer/StaticImport/ImportedAssetLibrary.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/Import/ModelLoader.h>
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include <BasicRenderer/Scene/Animation/Skeleton.h>

namespace br::staticimport
{
	namespace
	{
		// The static import pipeline skips a part with any skinned mesh that is not
		// wind-simulated (StaticImportPreparation.h); such parts belong to the dynamic scene.
		bool IsSkinnedPart(const std::vector<std::shared_ptr<Mesh>>& meshes)
		{
			return std::ranges::any_of(meshes, [](const std::shared_ptr<Mesh>& mesh) {
				return mesh && mesh->HasBaseSkin() && !mesh->GetBaseSkin()->HasWindSimulationGroups();
			});
		}

		constexpr std::string_view kPartSuffix = "#part=";
	}

	ImportedAssetLibrary::ImportedAssetLibrary(br::import::ImportSettings settings)
		: _settings(std::move(settings))
		, _loadScope(TaskSchedulerManager::GetInstance().CreateScope("BR.ImportedAssetLibrary"))
	{
	}

	ImportedAssetLibrary::~ImportedAssetLibrary()
	{
		Shutdown();
	}

	void ImportedAssetLibrary::Shutdown()
	{
		{
			std::lock_guard lock(_mutex);
			if (_shutdown) return;
			_shutdown = true;
		}
		if (_loadScope.Valid()) _loadScope.CancelAndWait();
		std::lock_guard callbackLock(_callbackMutex);
		_readyCallback = {};
		_skinnedCallback = {};
	}

	std::string ImportedAssetLibrary::MakeAssetKey(std::string_view filePath)
	{
		if (filePath.empty()) return {};
		if (const auto part = PartIndexOf(filePath)) return MakePartAssetKey(FileKeyOf(filePath), *part);
		std::error_code error;
		auto path = std::filesystem::absolute(std::filesystem::path(filePath), error);
		if (error) path = std::filesystem::path(filePath);
		auto key = path.lexically_normal().generic_string();
		std::ranges::transform(key, key.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return key;
	}

	std::string ImportedAssetLibrary::MakePartAssetKey(std::string_view filePath, std::uint32_t partIndex)
	{
		auto key = MakeAssetKey(FileKeyOf(filePath));
		if (key.empty()) return {};
		key += kPartSuffix;
		key += std::to_string(partIndex);
		return key;
	}

	std::string_view ImportedAssetLibrary::FileKeyOf(std::string_view assetKey)
	{
		const auto suffix = assetKey.rfind(kPartSuffix);
		return suffix == std::string_view::npos ? assetKey : assetKey.substr(0, suffix);
	}

	std::optional<std::uint32_t> ImportedAssetLibrary::PartIndexOf(std::string_view assetKey)
	{
		const auto suffix = assetKey.rfind(kPartSuffix);
		if (suffix == std::string_view::npos) return std::nullopt;
		const auto digits = assetKey.substr(suffix + kPartSuffix.size());
		std::uint32_t index = 0;
		const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
		if (error != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
		return index;
	}

	std::string ImportedAssetLibrary::CanonicalAssetKey(std::string_view assetKey) const
	{
		return MakeAssetKey(assetKey);
	}

	void ImportedAssetLibrary::SetReadyRenderableAssetsCallback(ReadyAssetsCallback callback)
	{
		std::lock_guard lock(_callbackMutex);
		_readyCallback = std::move(callback);
	}

	void ImportedAssetLibrary::SetSkinnedPartsCallback(SkinnedPartsCallback callback)
	{
		std::lock_guard lock(_callbackMutex);
		_skinnedCallback = std::move(callback);
	}

	void ImportedAssetLibrary::NotifyReady()
	{
		ReadyAssetsCallback callback;
		{
			std::lock_guard lock(_callbackMutex);
			callback = _readyCallback;
		}
		if (callback) callback();
	}

	StaticAssetStatus ImportedAssetLibrary::ToStatus(State state)
	{
		switch (state) {
		case State::Loading:
		case State::Publishing:
			return StaticAssetStatus::Loading;
		case State::ReadyToPublish:
			return StaticAssetStatus::ReadyToPublish;
		case State::Ready:
			return StaticAssetStatus::Ready;
		case State::Failed:
			return StaticAssetStatus::Failed;
		case State::Unsupported:
			return StaticAssetStatus::Unsupported;
		}
		return StaticAssetStatus::Invalid;
	}

	StaticAssetStatus ImportedAssetLibrary::StatusOf(const Entry& entry, std::optional<std::uint32_t> partIndex)
	{
		if (!partIndex || entry.state != State::Ready) return ToStatus(entry.state);
		return *partIndex < entry.partAssets.size() && !entry.partAssets[*partIndex].empty()
			? StaticAssetStatus::Ready
			: StaticAssetStatus::Unsupported;
	}

	ImportedAssetLibrary::Entry& ImportedAssetLibrary::FindOrQueueLocked(const std::string& key)
	{
		if (const auto it = _entries.find(key); it != _entries.end()) return *it->second;
		auto& entry = *_entries.emplace(key, std::make_unique<Entry>()).first->second;
		if (_shutdown) {
			entry.state = State::Failed;
			entry.reason = "asset library is shut down";
			return entry;
		}
		const bool submitted = TaskSchedulerManager::GetInstance().Submit(
			_loadScope, TaskLane::Streaming, TaskDomain::AssetImport, "BR::ImportedAssetLibrary::Load",
			[this, key, entryPtr = &entry](const br::TaskContext& context) {
				if (context.StopRequested()) return;
				std::optional<br::import::ImportedAssetPayload> payload;
				std::string failure;
				const auto loadBegin = std::chrono::steady_clock::now();
				try {
					payload = br::import::LoadImportedAsset(key, _settings);
					if (!payload) failure = "import failed";
				} catch (const std::exception& ex) {
					failure = std::string("import threw: ") + ex.what();
				}
				spdlog::info("ImportedAssetLibrary: loaded '{}' in {} ms", key,
					std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - loadBegin).count());
				{
					std::lock_guard lock(_mutex);
					if (payload) {
						entryPtr->loadedParts.reserve(payload->parts.size());
						for (std::uint32_t index = 0; index < payload->parts.size(); ++index) {
							const auto& part = payload->parts[index];
							LoadedPart loaded{ .index = index, .skinned = IsSkinnedPart(part.meshes), .name = part.name };
							DirectX::XMStoreFloat4x4(&loaded.localMatrix, part.localMatrix);
							entryPtr->loadedParts.push_back(std::move(loaded));
						}
						entryPtr->payload = std::move(payload);
						entryPtr->state = State::ReadyToPublish;
						_readyToPublish.fetch_add(1, std::memory_order_release);
					} else {
						entryPtr->state = State::Failed;
						entryPtr->reason = failure;
					}
				}
				if (!failure.empty()) spdlog::warn("ImportedAssetLibrary: '{}' failed: {}", key, failure);
				NotifyReady();
			});
		if (!submitted) {
			entry.state = State::Failed;
			entry.reason = "could not submit the asset load";
		}
		return entry;
	}

	StaticAssetStatus ImportedAssetLibrary::QueueOrPollRenderableAsset(std::string_view assetKey)
	{
		const auto key = MakeAssetKey(assetKey);
		if (key.empty()) return StaticAssetStatus::Invalid;
		std::lock_guard lock(_mutex);
		return StatusOf(FindOrQueueLocked(std::string(FileKeyOf(key))), PartIndexOf(key));
	}

	std::optional<std::vector<ImportedAssetLibrary::LoadedPart>> ImportedAssetLibrary::TryGetLoadedParts(std::string_view filePath)
	{
		const auto key = MakeAssetKey(FileKeyOf(filePath));
		if (key.empty()) return std::vector<LoadedPart>{};
		std::lock_guard lock(_mutex);
		const auto& entry = FindOrQueueLocked(key);
		if (entry.state == State::Loading) return std::nullopt;
		if (entry.state == State::Failed) return std::vector<LoadedPart>{};
		return entry.loadedParts;
	}

	StaticAssetStatus ImportedAssetLibrary::TryPublish(const std::string& key)
	{
		Entry* entry = nullptr;
		std::optional<br::import::ImportedAssetPayload> payload;
		{
			std::lock_guard lock(_mutex);
			entry = &FindOrQueueLocked(key);
			if (entry->state != State::ReadyToPublish) return ToStatus(entry->state);
			payload = std::move(entry->payload);
			entry->payload.reset();
			entry->state = State::Publishing;
			_readyToPublish.fetch_sub(1, std::memory_order_acq_rel);
		}

		auto asset = br::import::RenderableAssetFromPayload(std::move(*payload));
		std::vector<br::import::RenderableAssetPart> staticParts;
		std::vector<br::import::RenderableAssetPart> skinnedParts;
		std::vector<std::vector<br::import::RenderableAssetPart>> partAssets(asset.parts.size());
		for (std::size_t index = 0; index < asset.parts.size(); ++index) {
			const auto& part = asset.parts[index];
			if (part.meshes.empty()) continue;
			if (IsSkinnedPart(part.meshes)) {
				skinnedParts.push_back(part);
				continue;
			}
			staticParts.push_back(part);
			auto& partAsset = partAssets[index].emplace_back(part);
			partAsset.localMatrix = DirectX::XMMatrixIdentity();
		}

		State state = State::Ready;
		std::string reason;
		if (staticParts.empty()) {
			state = skinnedParts.empty() ? State::Failed : State::Unsupported;
			reason = skinnedParts.empty() ? "asset has no renderable parts" : "asset has only skinned parts";
		}
		spdlog::info("ImportedAssetLibrary: published '{}' static_parts={} skinned_parts={} meshes={}",
			key, staticParts.size(), skinnedParts.size(), asset.meshes.size());

		const std::vector<br::import::RenderableAssetPart>* publishedSkinned = nullptr;
		{
			std::lock_guard lock(_mutex);
			entry->asset = std::move(asset);
			entry->staticParts = std::move(staticParts);
			entry->skinnedParts = std::move(skinnedParts);
			entry->partAssets = std::move(partAssets);
			entry->reason = std::move(reason);
			// The parts are immutable from here on; readers take pointers to them.
			entry->state = state;
			if (!entry->skinnedParts.empty()) publishedSkinned = &entry->skinnedParts;
		}
		if (publishedSkinned) {
			SkinnedPartsCallback callback;
			{
				std::lock_guard lock(_callbackMutex);
				callback = _skinnedCallback;
			}
			if (callback) callback(key, *publishedSkinned);
		}
		return ToStatus(state);
	}

	StaticAssetStatus ImportedAssetLibrary::PublishRenderableAssetIfReady(std::string_view assetKey)
	{
		const auto key = MakeAssetKey(assetKey);
		if (key.empty()) return StaticAssetStatus::Invalid;
		const auto fileKey = std::string(FileKeyOf(key));
		const auto status = TryPublish(fileKey);
		const auto part = PartIndexOf(key);
		if (!part || status != StaticAssetStatus::Ready) return status;
		std::lock_guard lock(_mutex);
		return StatusOf(*_entries.at(fileKey), part);
	}

	std::size_t ImportedAssetLibrary::PublishReadyRenderableAssets(std::size_t maxCount)
	{
		if (maxCount == 0 || !HasReadyRenderableAssets()) return 0;
		std::vector<std::string> ready;
		{
			std::lock_guard lock(_mutex);
			for (const auto& [key, entry] : _entries) {
				if (entry->state != State::ReadyToPublish) continue;
				ready.push_back(key);
				if (ready.size() >= maxCount) break;
			}
		}
		std::size_t published = 0;
		for (const auto& key : ready) {
			const auto status = TryPublish(key);
			if (status == StaticAssetStatus::Ready || status == StaticAssetStatus::Unsupported ||
				status == StaticAssetStatus::Failed) {
				++published;
			}
		}
		return published;
	}

	bool ImportedAssetLibrary::HasReadyRenderableAssets() const
	{
		return _readyToPublish.load(std::memory_order_acquire) != 0;
	}

	const std::vector<br::import::RenderableAssetPart>* ImportedAssetLibrary::TryGetReadyParts(
		std::string_view assetKey,
		const StaticMaterialVariant& variant) const
	{
		(void)variant;
		const auto key = MakeAssetKey(assetKey);
		const auto part = PartIndexOf(key);
		std::lock_guard lock(_mutex);
		const auto it = _entries.find(std::string(FileKeyOf(key)));
		if (it == _entries.end() || StatusOf(*it->second, part) != StaticAssetStatus::Ready) return nullptr;
		return part ? &it->second->partAssets[*part] : &it->second->staticParts;
	}

	const std::vector<br::import::RenderableAssetPart>* ImportedAssetLibrary::TryGetSkinnedParts(std::string_view assetKey) const
	{
		const auto key = MakeAssetKey(FileKeyOf(assetKey));
		std::lock_guard lock(_mutex);
		const auto it = _entries.find(key);
		if (it == _entries.end()) return nullptr;
		const auto state = it->second->state;
		if ((state != State::Ready && state != State::Unsupported) || it->second->skinnedParts.empty()) return nullptr;
		return &it->second->skinnedParts;
	}

	bool ImportedAssetLibrary::RequestMaterialVariant(std::string_view assetKey, const StaticMaterialVariant& variant)
	{
		if (variant.overrides) {
			spdlog::warn("ImportedAssetLibrary: material variants are not supported; '{}' uses its own materials", assetKey);
		}
		return QueueOrPollRenderableAsset(assetKey) == StaticAssetStatus::Ready;
	}

	bool ImportedAssetLibrary::HasKnownFailure(std::string_view assetKey) const
	{
		const auto key = MakeAssetKey(FileKeyOf(assetKey));
		std::lock_guard lock(_mutex);
		const auto it = _entries.find(key);
		return it != _entries.end() && it->second->state == State::Failed;
	}

	std::optional<std::string> ImportedAssetLibrary::KnownUnsupportedStaticImportReason(std::string_view assetKey) const
	{
		const auto key = MakeAssetKey(assetKey);
		const auto part = PartIndexOf(key);
		std::lock_guard lock(_mutex);
		const auto it = _entries.find(std::string(FileKeyOf(key)));
		if (it == _entries.end() || StatusOf(*it->second, part) != StaticAssetStatus::Unsupported) return std::nullopt;
		if (it->second->state == State::Unsupported) return it->second->reason;
		return std::string(*part < it->second->partAssets.size() ? "part is skinned" : "part does not exist");
	}
}
