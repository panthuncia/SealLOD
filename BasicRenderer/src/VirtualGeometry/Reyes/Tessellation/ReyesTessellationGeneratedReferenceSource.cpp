#include "VirtualGeometry/Reyes/Tessellation/ReyesTessellationReferenceSource.h"

#define tessellation_table generated_tessellation_table
#include "tessellation_table_generated.hpp"
#undef tessellation_table

const ReyesPackedTessellationTableSource& GetGeneratedReyesPackedTessellationTableSource()
{
    static const ReyesPackedTessellationTableSource source{
        generated_tessellation_table::max_edge_segments,
        generated_tessellation_table::max_vertices,
        generated_tessellation_table::max_configs,
        generated_tessellation_table::vertices,
        generated_tessellation_table::triangles,
        generated_tessellation_table::configs,
    };

    return source;
}