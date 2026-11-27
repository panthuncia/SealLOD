#ifndef HAIR_STRUCTS_HLSLI
#define HAIR_STRUCTS_HLSLI

// The groom structs the CPU builder writes and the runtime structs the plugin uploads, shared with C++. The build
// stages these headers into a Hair/ folder beside the hair shaders (Plugins/Hair/CMakeLists.txt). HairLodShared.h is
// the LOD rules, the same source the CPU compiles.
#include "Hair/HairGPUTypes.h"
#include "Hair/HairRuntimeGPUTypes.h"
#include "Hair/HairLodShared.h"

#endif // HAIR_STRUCTS_HLSLI
