#pragma once

// Merged meshes as static assets (ClusterLODMergedMesh.h).
//
// Wraps the host's asset source. A key the host's predicate accepts names a merged mesh: its CLod cache
// (CLodCache::TryLoadMergedMesh) lists the parts its portals open by host address, so the mesh is ready once every one
// of them is, through the wrapped source, and is then built from the cache with those parts and their materials.
// Every other key goes straight to the wrapped source.

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <BasicRenderer/StaticImport/StaticAssetSource.h>

struct ClusterLODPrebuiltData;

namespace br::staticimport
{
	class MergedMeshAssetSource final : public IStaticAssetSource
	{
	public:
		using KeyPredicate = std::function<bool(std::string_view assetKey)>;

		// `inner` must outlive this.
		MergedMeshAssetSource(IStaticAssetSource& inner, KeyPredicate isMergedMesh);

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
		[[nodiscard]] StaticMaterialVariant MakeMaterialVariant(
			std::uint64_t hash,
			std::vector<recipe::MaterialOverride>&& overrides) const override;
		[[nodiscard]] StaticMaterialVariant MakeMaterialVariant(
			const recipe::BatchRecipeView& recipe,
			const recipe::MappedGroup& group) const override;
		[[nodiscard]] std::uint64_t MaterialVariantContentHash(const StaticMaterialVariant& variant) const override;
		[[nodiscard]] std::size_t MaterialVariantOverrideCount(const StaticMaterialVariant& variant) const override;

	private:
		struct ResolvedPart
		{
			StaticMaterialVariant variant;
			bool hasOverrides = false;
		};
		struct MergedMesh
		{
			std::shared_ptr<const ::ClusterLODPrebuiltData> data; // from its cache, until it is built
			std::vector<ResolvedPart> parts;
			std::vector<br::import::RenderableAssetPart> renderable;
			bool ready = false;
			bool failed = false;
		};
		StaticAssetStatus PollMergedMesh(std::string_view assetKey);
		void Fail(std::string_view assetKey);

		IStaticAssetSource* _inner;
		KeyPredicate _isMergedMesh;
		mutable std::mutex _mutex;
		std::unordered_map<std::string, MergedMesh> _meshes;
	};
}
