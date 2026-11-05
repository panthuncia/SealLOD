#include "Analyzers/Analyzers.h"

#include "Core/Analyzer.h"

namespace clodeval {

const std::vector<std::unique_ptr<IAnalyzer>>& AllAnalyzers()
{
	static const std::vector<std::unique_ptr<IAnalyzer>> analyzers = [] {
		std::vector<std::unique_ptr<IAnalyzer>> list;
		list.push_back(MakeHierarchyStructureAnalyzer());
		list.push_back(MakeHierarchyValidityAnalyzer());
		list.push_back(MakeMemoryPagesAnalyzer());
		list.push_back(MakeVoxelDistributionAnalyzer());
		list.push_back(MakeGeometryTruthAnalyzer());
		list.push_back(MakeTransitionHierarchyAnalyzer());
		list.push_back(MakeVoxelFidelityAnalyzer());
		return list;
	}();
	return analyzers;
}

const IAnalyzer* FindAnalyzer(std::string_view id)
{
	for (const auto& analyzer : AllAnalyzers()) {
		if (analyzer->Info().id == id) {
			return analyzer.get();
		}
	}
	return nullptr;
}

}
