#include "CacheRoots.h"

#include <format>

#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/Geometry/Mesh.h>
#include <BasicRenderer/Assets/Geometry/VertexLayout.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Assets/Material.h>
#include <BasicRenderer/Assets/MaterialDescription.h>
#include <BasicRenderer/Diagnostics/CLodTelemetry.h>
#include <BasicRenderer/Scene/VertexFlags.h>

#include "Core/MergeHierarchy.h"

namespace clodcapture {

namespace {
	using br::staticimport::StaticAssetStatus;

	// The vertex layout the cache's pages carry. A cache has no record of the source's layout, and the pages decode
	// themselves, so this only has to agree with what they hold.
	unsigned int VertexFlagsOf(const clodeval::CachedMesh& mesh)
	{
		const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
		for (uint32_t page = 0; page < prebuilt.trianglePageCount && page < mesh.PageCount(); ++page) {
			CLodCache::TrianglePage parsed;
			if (mesh.IsVoxelPage(page) || !CLodCache::TrianglePage::Parse(mesh.Page(page), parsed)) {
				continue;
			}
			unsigned int flags = 0u;
			flags |= parsed.HasNormals() ? VertexFlags::VERTEX_NORMALS : 0u;
			flags |= parsed.UvSetCount() != 0u ? VertexFlags::VERTEX_TEXCOORDS : 0u;
			flags |= parsed.HasColors() ? VertexFlags::VERTEX_COLORS : 0u;
			return flags;
		}
		return VertexFlags::VERTEX_NORMALS;
	}

	std::shared_ptr<Mesh> LoadMesh(const clodeval::CachedMesh& cache, const std::shared_ptr<Material>& material, std::string& outError)
	{
		const ClusterLODCacheSource& source = cache.Prebuilt().cacheSource;
		auto loaded = CLodCache::TryLoad({ source.sourceIdentifier, source.primPath, source.subsetName }, cache.BuildConfigHash());
		if (!loaded) {
			outError = std::format("the renderer's cache lookup does not find '{}' (is SARP_CACHE_ROOT the tree it is in?)", cache.MetadataPath().string());
			return nullptr;
		}
		const unsigned int flags = VertexFlagsOf(cache);
		MeshIngestBuilder builder(MeshVertexLayout::VertexSize(flags), 0u, flags);
		auto mesh = builder.Build(material, std::move(loaded->prebuiltData));
		if (!mesh) {
			outError = std::format("'{}' did not make a mesh", cache.MetadataPath().string());
		}
		return mesh;
	}

	// One flat colour per part, so the image says which part a pixel's material came from.
	std::shared_ptr<Material> MakePaletteMaterial(uint32_t part, uint32_t slot)
	{
		uint32_t value = (part ^ 0x9E3779B9u) * 0x85EBCA6Bu;
		value ^= value >> 13u;
		value *= 0xC2B2AE35u;
		value ^= value >> 16u;
		const auto channel = [](uint32_t bits) { return 0.25f + 0.65f * static_cast<float>(bits & 0xFFu) / 255.0f; };
		MaterialDescription description;
		description.name = std::format("merge palette {} (part {})", slot, part);
		description.diffuseColor = { channel(value), channel(value >> 8u), channel(value >> 16u), 1.0f };
		description.sourceMaterialIdentity = (static_cast<uint64_t>(part) << 32u) | slot;
		return Material::CreateShared(description);
	}
}

bool CacheRootSource::Load(const std::filesystem::path& rootMetadata, std::string& outError)
{
	std::unique_ptr<clodeval::CachedMesh> rootCache = clodeval::CachedMesh::Open(rootMetadata, outError);
	if (!rootCache) {
		return false;
	}
	const std::unique_ptr<clodeval::MergeHierarchy> hierarchy = clodeval::ReadMergeHierarchy(*rootCache, true);
	if (!hierarchy) {
		outError = std::format("'{}' is not a merge-hierarchy root", rootMetadata.string());
		return false;
	}
	if (!hierarchy->partsOpen) {
		outError = std::format("'{}': not every part could be opened", rootMetadata.string());
		return false;
	}

	std::vector<std::shared_ptr<Material>> palette;
	std::vector<std::shared_ptr<Material>> partMaterial(hierarchy->parts.size());
	const size_t slots = std::max<size_t>(hierarchy->paletteParts.size(), hierarchy->materialSlots);
	for (uint32_t slot = 0; slot < slots; ++slot) {
		const uint32_t part = slot < hierarchy->paletteParts.size() ? hierarchy->paletteParts[slot] : 0u;
		palette.push_back(MakePaletteMaterial(part, slot));
		if (part < partMaterial.size() && !partMaterial[part]) {
			partMaterial[part] = palette.back();
		}
	}

	std::vector<std::shared_ptr<Mesh>> parts;
	for (uint32_t i = 0; i < hierarchy->parts.size(); ++i) {
		const clodeval::MergePart& part = hierarchy->parts[i];
		auto mesh = LoadMesh(*part.cache, partMaterial[i] ? partMaterial[i] : palette.front(), outError);
		if (!mesh) {
			outError = std::format("part {} ({}): {}", i, part.label, outError);
			return false;
		}
		const ClusterLODCacheSource& source = part.cache->Prebuilt().cacheSource;
		RecordCLodMeshSource(mesh->GetGlobalID(), part.label, "portal part", 0u, source.sourceIdentifier, source.primPath, source.subsetName);
		parts.push_back(std::move(mesh));
	}
	auto root = LoadMesh(*rootCache, palette.front(), outError);
	if (!root) {
		return false;
	}
	root->SetPortalParts(parts);
	root->SetMaterialPalette(std::move(palette));

	Root entry;
	br::import::RenderableAssetPart part;
	part.meshes.push_back(std::move(root));
	part.name = rootCache->Prebuilt().cacheSource.sourceIdentifier;
	entry.parts.push_back(std::move(part));
	spdlog::info("merge root '{}': {} nodes, {} portals, {} parts, palette {}", rootMetadata.string(), hierarchy->nodes.size(),
		hierarchy->portals.size(), hierarchy->parts.size(), slots);
	std::scoped_lock lock(m_mutex);
	m_roots[KeyFor(rootMetadata)] = std::move(entry);
	return true;
}

const CacheRootSource::Root* CacheRootSource::Find(std::string_view assetKey) const
{
	std::scoped_lock lock(m_mutex);
	const auto found = m_roots.find(std::string(assetKey));
	return found != m_roots.end() ? &found->second : nullptr;
}

std::string CacheRootSource::CanonicalAssetKey(std::string_view assetKey) const
{
	return assetKey.starts_with(kPrefix) ? std::string(assetKey) : m_inner.CanonicalAssetKey(assetKey);
}

const std::vector<br::import::RenderableAssetPart>* CacheRootSource::TryGetReadyParts(std::string_view assetKey, const br::staticimport::StaticMaterialVariant& variant) const
{
	if (!assetKey.starts_with(kPrefix)) {
		return m_inner.TryGetReadyParts(assetKey, variant);
	}
	const Root* root = Find(assetKey);
	return root ? &root->parts : nullptr;
}

StaticAssetStatus CacheRootSource::QueueOrPollRenderableAsset(std::string_view assetKey)
{
	if (!assetKey.starts_with(kPrefix)) {
		return m_inner.QueueOrPollRenderableAsset(assetKey);
	}
	return Find(assetKey) ? StaticAssetStatus::Ready : StaticAssetStatus::Failed;
}

StaticAssetStatus CacheRootSource::PublishRenderableAssetIfReady(std::string_view assetKey)
{
	if (!assetKey.starts_with(kPrefix)) {
		return m_inner.PublishRenderableAssetIfReady(assetKey);
	}
	return Find(assetKey) ? StaticAssetStatus::Ready : StaticAssetStatus::Failed;
}

bool CacheRootSource::RequestMaterialVariant(std::string_view assetKey, const br::staticimport::StaticMaterialVariant& variant)
{
	if (!assetKey.starts_with(kPrefix)) {
		return m_inner.RequestMaterialVariant(assetKey, variant);
	}
	return Find(assetKey) != nullptr;
}

bool CacheRootSource::HasKnownFailure(std::string_view assetKey) const
{
	if (!assetKey.starts_with(kPrefix)) {
		return m_inner.HasKnownFailure(assetKey);
	}
	return Find(assetKey) == nullptr;
}

std::optional<std::string> CacheRootSource::KnownUnsupportedStaticImportReason(std::string_view assetKey) const
{
	if (!assetKey.starts_with(kPrefix)) {
		return m_inner.KnownUnsupportedStaticImportReason(assetKey);
	}
	return std::nullopt;
}

}
