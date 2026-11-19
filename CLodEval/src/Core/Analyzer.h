#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Core/CachedMesh.h"
#include "Core/ErrorChannels.h"
#include "Core/MergeHierarchy.h"
#include "Core/Metrics.h"
#include "Core/SourceTextures.h"

namespace clodeval {

// What an analyzer is given. The capture bundle and reference will be added here when image
// analyzers arrive; an analyzer states what it needs so the runner can skip what it cannot run.
struct AnalysisContext {
	const CachedMesh& mesh;
	std::string_view asset;
	// Shared by every mesh of a run, so merge roots measure each part once. Null: each root opens its own.
	MergePartLibrary* mergeParts = nullptr;
	// The coarsest cut the renderer asks of a root: see MergeIssues.
	float mergeReachCut = 440.0f;
	// Selection rules quality.appearance sweeps besides the stored errors (a suite's "cut_rules"). Null or empty: none.
	const std::vector<CutRule>* cutRules = nullptr;
	// The source asset's materials (base colour textures), when it is a .glb the run could open: quality.texture needs them.
	const SourceMaterials* materials = nullptr;
	// Simulated appearance bakes quality.texture also sweeps (a suite's "texture_bakes"). Null or empty: none.
	const std::vector<TextureBake>* textureBakes = nullptr;
	// quality.specular's footprint in pixels of a one-pixel threshold at each segment's finest cut (a suite's "specular_footprint").
	double specularFootprint = 1.0;
	// quality.specular's widening cap, as clodNormalSpreadRoughnessCap (a suite's "specular_spread_cap"; default 1).
	double specularSpreadCap = 1.0;
};

struct AnalyzerInfo {
	std::string_view id;
	uint32_t version = 1;  // bump when a change makes earlier numbers incomparable
	std::string_view description;
};

class IAnalyzer {
public:
	virtual ~IAnalyzer() = default;
	virtual const AnalyzerInfo& Info() const = 0;
	virtual void Run(const AnalysisContext& context, MetricSink& sink) const = 0;
};

// Every analyzer the tool has. Add one by writing it and listing it in Analyzers.cpp.
const std::vector<std::unique_ptr<IAnalyzer>>& AllAnalyzers();
const IAnalyzer* FindAnalyzer(std::string_view id);

}
