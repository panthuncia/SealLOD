#pragma once

enum PSOFlags {
	PSO_FLAGS_NONE = 0,
    // 1 << 1 was PSO_SHADOW, the retired shadow-map depth pass.
	PSO_IMAGE_BASED_LIGHTING = 1 << 2,
	PSO_CLUSTERED_LIGHTING = 1 << 3,
	PSO_PREPASS = 1 << 4,
	PSO_DEFERRED = 1 << 5,
	PSO_SCREENSPACE_REFLECTIONS = 1 << 6,
};