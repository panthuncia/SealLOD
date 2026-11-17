// CORTAO Reference Implementation v1.0.3
//
// MIT License
//
// Copyright (c) 2026 Daniel Isheden
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <cstdint>
#include <cmath>

namespace CORTAO {

struct CORTAOConfigurationParameters {
	uint32_t width;
	uint32_t height;
	uint32_t mipCount;
	uint32_t randomSeed;

	bool flipY;
	bool isPerspective;
	float projectionMatrix[4][4];
	float inverseProjectionMatrix[4][4];
	float nearPlane;
	float farPlane;

	float maxRadius;
	float maxScreenspaceRadiusHeightRatio = 0.2f;
	float mipBias = -4.0f;
	float maxSlopeCompensation = 4.0f;
	float backfaceThreshold = 0.5f;
	float backfaceBoost = 0.0f;

	//Filter parameters
	float filterDepthSensitivity = 512.0f;
	float filterNormalSensitivity = 128.0f;
};

struct CORTAOPrepassParameters {
	uint32_t packedResolution;
	float depthScale;
	float depthOffset;
	uint32_t randomSeed;

	uint32_t mipCount;
	uint32_t _padding0;
	uint32_t _padding1;
	uint32_t _padding2;
};

inline void populatePrepassParameters(const CORTAOConfigurationParameters& configuration, CORTAOPrepassParameters* parameters) {

	parameters->packedResolution = configuration.width | (configuration.height << 16u);

	float nearPlane = configuration.nearPlane;
	float farPlane = configuration.farPlane;
	if (configuration.isPerspective) {
		if (std::isinf(nearPlane)) {
			//infinite nearPlane plane (reverse depth)
			parameters->depthScale = 1.0f / farPlane;
			parameters->depthOffset = 0.0f;
		}
		else if (std::isinf(farPlane)) {
			//infinite farPlane plane
			parameters->depthScale = -1.0f / nearPlane;
			parameters->depthOffset = 1.0f / nearPlane;
		}
		else {
			//standard case with finite nearPlane and farPlane planes (reverse depth is supported).
			parameters->depthScale = (nearPlane - farPlane) / (nearPlane * farPlane);
			parameters->depthOffset = 1.0f / nearPlane;
		}
	} else {
		parameters->depthScale = farPlane - nearPlane;
		parameters->depthOffset = nearPlane;
	}

	parameters->randomSeed = configuration.randomSeed;
	parameters->mipCount = configuration.mipCount;
}

struct CORTAOMainPassParameters {

	uint32_t packedResolution;
	uint32_t randomSeed;
	float normalBias;
	float radius;

	float unprojectParamsX;
	float unprojectParamsY;
	float unprojectParamsZ;
	float unprojectParamsW;

	float projectionParamsX;
	float projectionParamsY;
	float projectionParamsZ;
	float projectionParamsW;

	float maxApparentCircleRadiusCoefficient;
	float mipBias;
	float inverseMaxSlopeCompensation;
	float backfaceThreshold;

	float backfaceBoost;
	uint32_t _padding0;
	uint32_t _padding1;
	uint32_t _padding2;
};

inline void populateMainPassParameters(const CORTAOConfigurationParameters& configuration, CORTAOMainPassParameters* parameters) {

	float projX = configuration.projectionMatrix[0][0];
	float projY = configuration.projectionMatrix[1][1];
	float projOffsetX = configuration.projectionMatrix[2][0];
	float projOffsetY = configuration.projectionMatrix[2][1];

	float invX = configuration.inverseProjectionMatrix[0][0];
	float invY = configuration.inverseProjectionMatrix[1][1];
	float invOffsetX = configuration.inverseProjectionMatrix[3][0];
	float invOffsetY = configuration.inverseProjectionMatrix[3][1];

	parameters->packedResolution = configuration.width | (configuration.height << 16u);
	parameters->randomSeed = configuration.randomSeed;
	parameters->normalBias = invX / (float)configuration.width * std::sqrtf(2.0f);
	parameters->radius = configuration.maxRadius;

	if (configuration.flipY) {
		parameters->unprojectParamsX = +2.0f * invX;
		parameters->unprojectParamsY = -2.0f * invY;
		parameters->unprojectParamsZ = -invX + invOffsetX;
		parameters->unprojectParamsW = +invY + invOffsetY;

		parameters->projectionParamsX = +0.5f * projX;
		parameters->projectionParamsY = -0.5f * projY;
		parameters->projectionParamsZ = -0.5f + 0.5f * projOffsetX;
		parameters->projectionParamsW = -0.5f - 0.5f * projOffsetY;
	} else {
		parameters->unprojectParamsX = +2.0f * invX;
		parameters->unprojectParamsY = +2.0f * invY;
		parameters->unprojectParamsZ = -invX + invOffsetX;
		parameters->unprojectParamsW = -invY + invOffsetY;

		parameters->projectionParamsX = +0.5f * projX;
		parameters->projectionParamsY = +0.5f * projY;
		parameters->projectionParamsZ = -0.5f + 0.5f * projOffsetX;
		parameters->projectionParamsW = -0.5f + 0.5f * projOffsetY;
	}

	float maxApparentCircleRadius = invY * configuration.maxScreenspaceRadiusHeightRatio * 2.0f;
	parameters->maxApparentCircleRadiusCoefficient = maxApparentCircleRadius / std::sqrtf(maxApparentCircleRadius * maxApparentCircleRadius + 1.0f);
	parameters->mipBias = std::log2f(projY * (float)configuration.height * 0.5f) + configuration.mipBias;
	parameters->inverseMaxSlopeCompensation = 1.0f / configuration.maxSlopeCompensation;
	parameters->backfaceThreshold = configuration.backfaceThreshold;

	parameters->backfaceBoost = configuration.backfaceBoost;
}


struct CORTAOFilterPassParameters {
	uint32_t packedResolution;
	float depthSensitivity;
	float normalSensitivity;
};

inline void populateFilterPassParameters(const CORTAOConfigurationParameters& configuration, CORTAOFilterPassParameters* parameters) {
	parameters->packedResolution = configuration.width | (configuration.height << 16u);
	parameters->depthSensitivity = configuration.filterDepthSensitivity;
	parameters->normalSensitivity = configuration.filterNormalSensitivity;
}
}