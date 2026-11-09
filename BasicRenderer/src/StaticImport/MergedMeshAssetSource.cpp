#include <BasicRenderer/StaticImport/MergedMeshAssetSource.h>

#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/Geometry/Mesh.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Assets/Import/ClusterLODMergedMesh.h>

namespace br::staticimport
{
	MergedMeshAssetSource::MergedMeshAssetSource(IStaticAssetSource& inner, KeyPredicate isMergedMesh)
		: _inner(&inner), _isMergedMesh(std::move(isMergedMesh))
	{
	}

	void MergedMeshAssetSource::Fail(std::string_view assetKey)
	{
		std::lock_guard lock(_mutex);
		_meshes[std::string(assetKey)].failed = true;
	}

	// A merged mesh is ready once every part it opens is (and its material variant): its parts are those assets' meshes,
	// its palette their materials.
	StaticAssetStatus MergedMeshAssetSource::PollMergedMesh(std::string_view assetKey)
	{
		std::shared_ptr<const ClusterLODPrebuiltData> data;
		std::vector<ResolvedPart> resolved;
		{
			std::lock_guard lock(_mutex);
			auto& mesh = _meshes[std::string(assetKey)];
			if (mesh.ready) return StaticAssetStatus::Ready;
			if (mesh.failed) return StaticAssetStatus::Failed;
			data = mesh.data;
			resolved = mesh.parts;
		}
		if (!data) {
			// Read outside the lock: every scene preparation polls merged meshes, and a cold read costs milliseconds.
			auto cached = CLodCache::TryLoadMergedMesh(assetKey);
			std::vector<ResolvedPart> parts;
			bool valid = cached.has_value() && !cached->prebuiltData.externalParts.empty();
			if (valid) {
				parts.reserve(cached->prebuiltData.externalParts.size());
				for (const ClusterLODExternalPart& part : cached->prebuiltData.externalParts) {
					std::vector<recipe::MaterialOverride> overrides;
					if (!recipe::DecodeMaterialOverrides(part.variant, overrides)) {
						valid = false;
						break;
					}
					const bool hasOverrides = !overrides.empty();
					parts.push_back({ _inner->MakeMaterialVariant(part.variantHash, std::move(overrides)), hasOverrides });
				}
			}
			std::lock_guard lock(_mutex);
			auto& mesh = _meshes[std::string(assetKey)];
			if (!mesh.data) {
				if (!valid) {
					spdlog::warn("Merged mesh '{}': its cache is missing, stale or unreadable", assetKey);
					mesh.failed = true;
					return StaticAssetStatus::Failed;
				}
				mesh.data = std::make_shared<const ClusterLODPrebuiltData>(std::move(cached->prebuiltData));
				mesh.parts = std::move(parts);
			}
			data = mesh.data;
			resolved = mesh.parts;
		}

		bool allReady = true;
		std::vector<std::shared_ptr<Mesh>> partMeshes(data->externalParts.size());
		for (std::size_t i = 0; i < data->externalParts.size(); ++i) {
			const ClusterLODExternalPart& part = data->externalParts[i];
			const StaticAssetStatus status = _inner->QueueOrPollRenderableAsset(part.assetKey);
			if (status == StaticAssetStatus::Failed || status == StaticAssetStatus::Unsupported || status == StaticAssetStatus::Invalid) {
				spdlog::warn("Merged mesh '{}': part asset '{}' failed", assetKey, part.assetKey);
				Fail(assetKey);
				return StaticAssetStatus::Failed;
			}
			if (status == StaticAssetStatus::ReadyToPublish) {
				(void)_inner->PublishRenderableAssetIfReady(part.assetKey);
			}
			if (status != StaticAssetStatus::Ready && status != StaticAssetStatus::ReadyToPublish) {
				allReady = false;
				continue;
			}
			if (resolved[i].hasOverrides && !_inner->RequestMaterialVariant(part.assetKey, resolved[i].variant)) {
				allReady = false;
				continue;
			}
			const auto* renderable = _inner->TryGetReadyParts(part.assetKey, resolved[i].variant);
			if (!renderable) {
				allReady = false;
				continue;
			}
			if (part.partIndex >= renderable->size() || part.meshIndex >= (*renderable)[part.partIndex].meshes.size()) {
				spdlog::warn("Merged mesh '{}': part asset '{}' no longer has part {} mesh {}", assetKey, part.assetKey, part.partIndex, part.meshIndex);
				Fail(assetKey);
				return StaticAssetStatus::Failed;
			}
			partMeshes[i] = (*renderable)[part.partIndex].meshes[part.meshIndex];
		}
		if (!allReady) return StaticAssetStatus::Loading;

		std::string reason;
		auto built = CreateClusterLODMergedMesh(*data, partMeshes, &reason);
		std::lock_guard lock(_mutex);
		auto& mesh = _meshes[std::string(assetKey)];
		if (mesh.ready) return StaticAssetStatus::Ready; // another poll built it first; its parts may be in use
		if (!built) {
			spdlog::warn("Merged mesh '{}': {}", assetKey, reason);
			mesh.failed = true;
			return StaticAssetStatus::Failed;
		}
		br::import::RenderableAssetPart renderablePart;
		renderablePart.meshes.push_back(std::move(built));
		renderablePart.name = std::string(assetKey);
		mesh.renderable.assign(1, std::move(renderablePart));
		mesh.data.reset();
		mesh.parts.clear();
		mesh.ready = true;
		return StaticAssetStatus::Ready;
	}

	std::string MergedMeshAssetSource::CanonicalAssetKey(std::string_view assetKey) const
	{
		if (_isMergedMesh(assetKey)) return std::string(assetKey);
		return _inner->CanonicalAssetKey(assetKey);
	}

	void MergedMeshAssetSource::SetReadyRenderableAssetsCallback(ReadyAssetsCallback callback)
	{
		_inner->SetReadyRenderableAssetsCallback(std::move(callback));
	}

	const std::vector<br::import::RenderableAssetPart>* MergedMeshAssetSource::TryGetReadyParts(
		std::string_view assetKey,
		const StaticMaterialVariant& variant) const
	{
		if (_isMergedMesh(assetKey)) {
			std::lock_guard lock(_mutex);
			const auto it = _meshes.find(std::string(assetKey));
			return it != _meshes.end() && it->second.ready ? &it->second.renderable : nullptr;
		}
		return _inner->TryGetReadyParts(assetKey, variant);
	}

	StaticAssetStatus MergedMeshAssetSource::QueueOrPollRenderableAsset(std::string_view assetKey)
	{
		if (_isMergedMesh(assetKey)) return PollMergedMesh(assetKey);
		return _inner->QueueOrPollRenderableAsset(assetKey);
	}

	StaticAssetStatus MergedMeshAssetSource::PublishRenderableAssetIfReady(std::string_view assetKey)
	{
		if (_isMergedMesh(assetKey)) return PollMergedMesh(assetKey);
		return _inner->PublishRenderableAssetIfReady(assetKey);
	}

	std::size_t MergedMeshAssetSource::PublishReadyRenderableAssets(std::size_t maxCount)
	{
		return _inner->PublishReadyRenderableAssets(maxCount);
	}

	bool MergedMeshAssetSource::HasReadyRenderableAssets() const
	{
		return _inner->HasReadyRenderableAssets();
	}

	bool MergedMeshAssetSource::RequestMaterialVariant(std::string_view assetKey, const StaticMaterialVariant& variant)
	{
		if (_isMergedMesh(assetKey)) return PollMergedMesh(assetKey) == StaticAssetStatus::Ready;
		return _inner->RequestMaterialVariant(assetKey, variant);
	}

	bool MergedMeshAssetSource::HasKnownFailure(std::string_view assetKey) const
	{
		if (_isMergedMesh(assetKey)) {
			std::lock_guard lock(_mutex);
			const auto it = _meshes.find(std::string(assetKey));
			return it != _meshes.end() && it->second.failed;
		}
		return _inner->HasKnownFailure(assetKey);
	}

	std::optional<std::string> MergedMeshAssetSource::KnownUnsupportedStaticImportReason(std::string_view assetKey) const
	{
		if (_isMergedMesh(assetKey)) return std::nullopt;
		return _inner->KnownUnsupportedStaticImportReason(assetKey);
	}

	StaticMaterialVariant MergedMeshAssetSource::MakeMaterialVariant(std::uint64_t hash, std::vector<recipe::MaterialOverride>&& overrides) const
	{
		return _inner->MakeMaterialVariant(hash, std::move(overrides));
	}

	StaticMaterialVariant MergedMeshAssetSource::MakeMaterialVariant(const recipe::BatchRecipeView& recipe, const recipe::MappedGroup& group) const
	{
		return _inner->MakeMaterialVariant(recipe, group);
	}

	std::uint64_t MergedMeshAssetSource::MaterialVariantContentHash(const StaticMaterialVariant& variant) const
	{
		return _inner->MaterialVariantContentHash(variant);
	}

	std::size_t MergedMeshAssetSource::MaterialVariantOverrideCount(const StaticMaterialVariant& variant) const
	{
		return _inner->MaterialVariantOverrideCount(variant);
	}
}
