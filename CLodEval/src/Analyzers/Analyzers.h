#pragma once

#include <memory>

#include "Core/Analyzer.h"

namespace clodeval {

std::unique_ptr<IAnalyzer> MakeHierarchyStructureAnalyzer();
std::unique_ptr<IAnalyzer> MakeHierarchyValidityAnalyzer();
std::unique_ptr<IAnalyzer> MakeMemoryPagesAnalyzer();
std::unique_ptr<IAnalyzer> MakeVoxelDistributionAnalyzer();
std::unique_ptr<IAnalyzer> MakeVoxelFidelityAnalyzer();
std::unique_ptr<IAnalyzer> MakeGeometryTruthAnalyzer();
std::unique_ptr<IAnalyzer> MakeTransitionHierarchyAnalyzer();

}
