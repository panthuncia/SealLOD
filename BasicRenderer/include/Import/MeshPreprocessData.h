#pragma once

// Common output type for the geometry-extraction + CLod-cache-building phase
// of all asset loaders.  This struct contains everything needed to
// either (a) save a CLod cache on disk (headless CLI tool) or (b) proceed to
// GPU Mesh creation (renderer).

#include <optional>
#include <memory>
#include <string>
#include <vector>

#include "Import/CLodCacheLoader.h"
#include "Import/RenderablePrototypeGeometry.h"
#include "Materials/MaterialDescription.h"
#include "Mesh/Mesh.h"
#include "Mesh/ClusterLODTypes.h"

struct MeshPreprocessResult {
	MeshIngestBuilder ingest;
	CLodCacheLoader::MeshCacheIdentity cacheIdentity;
	std::string sourcePrimPath;
	std::optional<ClusterLODPrebuiltData> prebuiltData;
	std::shared_ptr<const ClusterLODPrebuildArtifacts> transientArtifacts;
	br::import::RenderablePrototypeGeometry prototypeGeometry;
	bool forceDoubleSidedPreview = false;
	bool geometricDisplacementOptIn = false;
	ObjectSurfaceSamplingMode objectSurfaceSamplingMode = ObjectSurfaceSamplingMode::None;
	bool objectSurfaceUseTriplanarProjection = false;
	bool objectSurfaceUseTripleTapStochastic = false;
	bool clodCacheHit = false;
	bool clodCacheBuilt = false;
	bool clodCacheSkippedBuild = false;
	float objectSurfaceTexelDensity = 1.0f;
	bool objectAtlasBakedHeight = false;
	std::uint32_t objectAtlasHeightUvSetIndex = 0;
	std::uint32_t objectAtlasWidth = 0;
	std::uint32_t objectAtlasHeight = 0;
	float objectAtlasTexelsPerUnit = 1.0f;
	float objectAtlasBlendWidthObjectUnits = 8.0f;
	float objectAtlasDisplacementMin = -0.5f;
	float objectAtlasDisplacementMax = 0.5f;
	std::vector<std::uint32_t> objectAtlasTriangleMaterialIndices;
	std::vector<std::string> objectAtlasSourceMaterialNames;
	std::vector<MaterialDescription> objectAtlasSourceMaterials;
	std::shared_ptr<const Mesh::ObjectReyesAtlasBakeData> objectAtlasSharedBakeData;

	MeshPreprocessResult(
		MeshIngestBuilder&& ingestData,
		CLodCacheLoader::MeshCacheIdentity&& identity,
		std::optional<ClusterLODPrebuiltData>&& prebuilt,
		bool forceDoubleSidedPreviewMaterial = false,
		br::import::RenderablePrototypeGeometry prototype = {})
		: ingest(std::move(ingestData))
		, cacheIdentity(std::move(identity))
		, prebuiltData(std::move(prebuilt))
		, prototypeGeometry(std::move(prototype))
		, forceDoubleSidedPreview(forceDoubleSidedPreviewMaterial) {
	}
};
