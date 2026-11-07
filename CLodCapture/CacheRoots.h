#pragma once

// Merge-hierarchy roots for the viewer, built from the cache tree alone: the root's cache, the caches its portal parts
// file names, and a flat material per palette slot (coloured by the part it belongs to). Nothing else about the asset
// that made them is needed, so any root a builder saved can be looked at.

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <BasicRenderer/StaticImport/StaticAssetSource.h>

namespace clodcapture {

// Serves "clod-root:<metadata path>" keys and passes every other key to the source it wraps.
class CacheRootSource final : public br::staticimport::IStaticAssetSource {
public:
	explicit CacheRootSource(br::staticimport::IStaticAssetSource& inner) : m_inner(inner) {}

	static constexpr std::string_view kPrefix = "clod-root:";
	static std::string KeyFor(const std::filesystem::path& rootMetadata) { return std::string(kPrefix) + rootMetadata.generic_string(); }

	// Builds the root now. False, with the reason, if it or a part cannot be loaded.
	bool Load(const std::filesystem::path& rootMetadata, std::string& outError);

	std::string CanonicalAssetKey(std::string_view assetKey) const override;
	void SetReadyRenderableAssetsCallback(ReadyAssetsCallback callback) override { m_inner.SetReadyRenderableAssetsCallback(std::move(callback)); }
	const std::vector<br::import::RenderableAssetPart>* TryGetReadyParts(std::string_view assetKey, const br::staticimport::StaticMaterialVariant& variant) const override;
	br::staticimport::StaticAssetStatus QueueOrPollRenderableAsset(std::string_view assetKey) override;
	br::staticimport::StaticAssetStatus PublishRenderableAssetIfReady(std::string_view assetKey) override;
	std::size_t PublishReadyRenderableAssets(std::size_t maxCount) override { return m_inner.PublishReadyRenderableAssets(maxCount); }
	bool HasReadyRenderableAssets() const override { return m_inner.HasReadyRenderableAssets(); }
	bool RequestMaterialVariant(std::string_view assetKey, const br::staticimport::StaticMaterialVariant& variant) override;
	bool HasKnownFailure(std::string_view assetKey) const override;
	std::optional<std::string> KnownUnsupportedStaticImportReason(std::string_view assetKey) const override;

private:
	struct Root {
		std::vector<br::import::RenderableAssetPart> parts;
	};
	const Root* Find(std::string_view assetKey) const;

	br::staticimport::IStaticAssetSource& m_inner;
	mutable std::mutex m_mutex;
	std::unordered_map<std::string, Root> m_roots;
};

}
