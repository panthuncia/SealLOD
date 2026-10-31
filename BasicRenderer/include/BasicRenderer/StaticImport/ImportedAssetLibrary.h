#pragma once

// An asset source for the static import pipeline backed by model files.
//
// Asset keys are model file paths (glTF/GLB, USD or NIF), canonicalized by
// MakeAssetKey. A key's file is loaded asynchronously with br::import::LoadImportedAsset
// on the task scheduler the first time the pipeline asks for it, and the loaded payload
// is published (turned into renderable parts) when the pipeline publishes it.
//
// A part key (MakePartAssetKey: "<file key>#part=<index>") names one part of a file as an
// asset of its own, with the part's local transform left to its placement. Every part key of
// a file reads the file's one parsed payload, and each publishes as soon as that file does, so
// a host can place a scene node by node and each node streams in on its own.
//
// A published asset is split in two:
// - its static parts (no skinned mesh, or only wind-simulated skins) are what the
//   static import pipeline imports through TryGetReadyParts;
// - its skinned parts never reach the static pipeline. They are kept here for the
//   dynamic scene (TryGetSkinnedParts, or the skinned-parts callback on publication).
// An asset with skinned parts only is reported as unsupported for static import.
//
// Material variants are not supported: the placement sources that use this library
// write recipes without material overrides, so every variant is the asset itself.
//
// Thread safety: every member may be called from any thread.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include <BasicRenderer/Assets/ImportSettings.h>
#include <BasicRenderer/Assets/RenderableAsset.h>
#include <BasicRenderer/StaticImport/StaticAssetSource.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>

namespace br::staticimport
{
	class ImportedAssetLibrary final : public IStaticAssetSource
	{
	public:
		// Runs on the publishing thread (a static import worker) once an asset with
		// skinned parts has published. `skinnedParts` stays valid for the library's
		// lifetime.
		using SkinnedPartsCallback = std::function<void(
			const std::string& assetKey,
			const std::vector<br::import::RenderableAssetPart>& skinnedParts)>;

		// A part of a loaded file, as listed before it publishes.
		struct LoadedPart
		{
			std::uint32_t index{ 0 };
			// The part's transform within the file; its part key's asset leaves it out.
			DirectX::XMFLOAT4X4 localMatrix{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			bool skinned{ false };
			std::string name;
		};

		explicit ImportedAssetLibrary(br::import::ImportSettings settings = {});
		~ImportedAssetLibrary() override;

		ImportedAssetLibrary(const ImportedAssetLibrary&) = delete;
		ImportedAssetLibrary& operator=(const ImportedAssetLibrary&) = delete;

		// The canonical key of a model file: its absolute, normalized path with forward
		// slashes, lower-cased (paths are case-insensitive on the platforms BR runs on).
		// The key is itself a path to the file.
		// A part key keeps its "#part=<index>" suffix.
		[[nodiscard]] static std::string MakeAssetKey(std::string_view filePath);
		// The key naming part `partIndex` of a file.
		[[nodiscard]] static std::string MakePartAssetKey(std::string_view filePath, std::uint32_t partIndex);
		// The file key of an asset key: the key itself, or a part key without its suffix.
		[[nodiscard]] static std::string_view FileKeyOf(std::string_view assetKey);

		// Starts loading a file if needed and lists its parts once the load finished:
		// nullopt while it loads, empty when it failed.
		[[nodiscard]] std::optional<std::vector<LoadedPart>> TryGetLoadedParts(std::string_view filePath);

		void SetSkinnedPartsCallback(SkinnedPartsCallback callback);

		// The skinned parts of a published asset; null while it is not published or
		// when it has none.
		[[nodiscard]] const std::vector<br::import::RenderableAssetPart>* TryGetSkinnedParts(std::string_view assetKey) const;

		// Cancels loads that have not started and waits for running ones. Idempotent;
		// the destructor calls it. Shut the static import runtime down first.
		void Shutdown();

		// ---- IStaticAssetSource ----

		[[nodiscard]] std::string CanonicalAssetKey(std::string_view assetKey) const override;
		void SetReadyRenderableAssetsCallback(ReadyAssetsCallback callback) override;
		const std::vector<br::import::RenderableAssetPart>* TryGetReadyParts(
			std::string_view assetKey,
			const StaticMaterialVariant& variant) const override;
		StaticAssetStatus QueueOrPollRenderableAsset(std::string_view assetKey) override;
		StaticAssetStatus PublishRenderableAssetIfReady(std::string_view assetKey) override;
		std::size_t PublishReadyRenderableAssets(std::size_t maxCount) override;
		[[nodiscard]] bool HasReadyRenderableAssets() const override;
		bool RequestMaterialVariant(std::string_view assetKey, const StaticMaterialVariant& variant) override;
		bool HasKnownFailure(std::string_view assetKey) const override;
		std::optional<std::string> KnownUnsupportedStaticImportReason(std::string_view assetKey) const override;

	private:
		enum class State
		{
			Loading,
			ReadyToPublish,
			Publishing,
			Ready,
			Failed,
			Unsupported
		};

		struct Entry
		{
			State state{ State::Loading };
			std::optional<br::import::ImportedAssetPayload> payload;
			// Listed when the load finishes, in payload order.
			std::vector<LoadedPart> loadedParts;
			// Kept alive for the parts, which reference its meshes and materials.
			br::import::RenderableAsset asset;
			std::vector<br::import::RenderableAssetPart> staticParts;
			std::vector<br::import::RenderableAssetPart> skinnedParts;
			// Each part key's asset: one static part with an identity local transform, or
			// empty for a skinned part. Indexed by part index.
			std::vector<std::vector<br::import::RenderableAssetPart>> partAssets;
			std::string reason;
		};

		[[nodiscard]] static StaticAssetStatus ToStatus(State state);
		[[nodiscard]] static std::optional<std::uint32_t> PartIndexOf(std::string_view assetKey);
		// The status of an asset key whose file entry is `entry`.
		[[nodiscard]] static StaticAssetStatus StatusOf(const Entry& entry, std::optional<std::uint32_t> partIndex);
		// Requires _mutex. Creates the entry and submits its load when it is unknown.
		Entry& FindOrQueueLocked(const std::string& key);
		// Publishes `key` if its load finished; returns the resulting status.
		StaticAssetStatus TryPublish(const std::string& key);
		void NotifyReady();

		br::import::ImportSettings _settings;
		br::TaskScope _loadScope;
		mutable std::mutex _mutex;
		std::unordered_map<std::string, std::unique_ptr<Entry>> _entries;
		std::atomic<std::size_t> _readyToPublish{ 0 };
		std::mutex _callbackMutex;
		ReadyAssetsCallback _readyCallback;
		SkinnedPartsCallback _skinnedCallback;
		bool _shutdown{ false };
	};
}
