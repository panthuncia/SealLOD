#pragma once

#include <array>

#include <BasicRenderer/Extensions/ShaderBuffers.h>

std::array<ClippingPlane, 6> GetFrustumPlanesPerspective(
	const float aspectRatio, const float fovRad, const float nearClip, const float farClip);

std::array<ClippingPlane, 6> GetFrustumPlanesOrthographic(
	const float left, const float right, const float top, const float bottom,
	const float nearClip, const float farClip, DirectX::XMFLOAT3 cameraPosWorld);
