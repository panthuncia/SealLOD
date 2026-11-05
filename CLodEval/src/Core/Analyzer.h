#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Core/CachedMesh.h"
#include "Core/Metrics.h"

namespace clodeval {

// What an analyzer is given. The capture bundle and reference will be added here when image
// analyzers arrive; an analyzer states what it needs so the runner can skip what it cannot run.
struct AnalysisContext {
	const CachedMesh& mesh;
	std::string_view asset;
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
