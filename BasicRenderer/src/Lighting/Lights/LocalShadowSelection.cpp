#include "Lighting/Lights/LocalShadowSelection.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "BasicRenderer/Scene/Components.h"
#include "Utilities/Utilities.h"

namespace br::lighting {

	using namespace DirectX;

	namespace {

		CameraInfo MakeFaceCamera(XMVECTOR position, XMVECTOR forward, XMVECTOR up, float fov, float zNear, float zFar,
			uint32_t virtualResolution, uint32_t lodResolution) {
			CameraInfo info{};
			XMStoreFloat4(&info.positionWorldSpace, XMVectorSetW(position, 1.0f));
			info.view = XMMatrixLookToRH(position, forward, up);
			info.viewInverse = XMMatrixInverse(nullptr, info.view);
			// A [0, 1] depth range, not the primary camera's reversed infinite one: the virtual shadow map stores linear
			// view depth and treats projected z outside [0, 1] as outside the face.
			info.unjitteredProjection = XMMatrixPerspectiveFovRH(fov, 1.0f, zNear, zFar);
			info.jitteredProjection = info.unjitteredProjection;
			info.projectionInverse = XMMatrixInverse(nullptr, info.unjitteredProjection);
			info.viewProjection = XMMatrixMultiply(info.view, info.unjitteredProjection);
			info.prevView = info.view;
			info.prevJitteredProjection = info.jitteredProjection;
			info.prevUnjitteredProjection = info.unjitteredProjection;
			const auto planes = GetFrustumPlanesPerspective(1.0f, fov, zNear, zFar);
			for (size_t i = 0; i < planes.size(); ++i) {
				info.clippingPlanes[i] = planes[i];
			}
			info.fov = fov;
			info.aspectRatio = 1.0f;
			info.zNear = zNear;
			info.zFar = zFar;
			info.depthBufferArrayIndex = -1;
			info.depthResX = virtualResolution;
			info.depthResY = virtualResolution;
			info.lodResY = lodResolution;
			info.numDepthMips = 1;
			info.isOrtho = 0;
			return info;
		}

		XMVECTOR AnyPerpendicular(XMVECTOR direction) {
			const XMVECTOR yAxis = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
			const XMVECTOR zAxis = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
			const float alignment = std::abs(XMVectorGetX(XMVector3Dot(direction, yAxis)));
			return alignment > 0.99f ? zAxis : yAxis;
		}

		float SpotHalfAngle(const LightInfo& light) {
			return std::acos(std::clamp(light.outerConeCos, -1.0f, 1.0f));
		}

	} // namespace

	uint32_t LocalShadowFaceCount(const LightInfo& light) {
		if (light.type == static_cast<unsigned int>(Components::LightType::Spot) && SpotHalfAngle(light) <= kLocalShadowMaxSpotHalfAngleRadians) {
			return 1u;
		}
		return kLocalShadowCubeFaceCount;
	}

	float LocalShadowNearPlane(const LightInfo& light) {
		// Far enough that depth precision holds across the range, near enough that a lamp's own fitting does not
		// sit inside it.
		return std::max(light.range * 0.002f, 0.01f);
	}

	std::vector<CameraInfo> BuildLocalShadowFaceCameras(const LightInfo& light, uint32_t virtualResolution, uint32_t lodResolution) {
		const XMVECTOR position = XMVectorSetW(light.posWorldSpace, 1.0f);
		const float zNear = LocalShadowNearPlane(light);
		const float zFar = std::max(light.range, zNear * 2.0f);
		std::vector<CameraInfo> faces;
		if (LocalShadowFaceCount(light) == 1u) {
			const XMVECTOR forward = XMVector3Normalize(light.dirWorldSpace);
			// A degree of margin past the cone, so its edge is not on the face's last texels.
			const float fov = 2.0f * SpotHalfAngle(light) + XMConvertToRadians(2.0f);
			faces.push_back(MakeFaceCamera(position, forward, AnyPerpendicular(forward), fov, zNear, zFar, virtualResolution, lodResolution));
			return faces;
		}
		const XMVECTOR forwards[kLocalShadowCubeFaceCount] = {
			XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), XMVectorSet(-1.0f, 0.0f, 0.0f, 0.0f),
			XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f),
			XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f),
		};
		const XMVECTOR ups[kLocalShadowCubeFaceCount] = {
			XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
			XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
			XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		};
		// The 90 degree face plus its guard band: tan(half) grows so the guard texels sit outside [-1, 1] of the cube.
		const float guardTan = kLocalShadowCoarsestMipTexels / (kLocalShadowCoarsestMipTexels - 2.0f * kLocalShadowCubeGuardTexels);
		const float cubeFov = 2.0f * std::atan(guardTan);
		for (uint32_t face = 0; face < kLocalShadowCubeFaceCount; ++face) {
			faces.push_back(MakeFaceCamera(position, forwards[face], ups[face], cubeFov, zNear, zFar, virtualResolution, lodResolution));
		}
		return faces;
	}

	std::vector<uint32_t> SelectLocalShadowLights(
		std::span<const LocalShadowCandidate> candidates, uint32_t faceBudget, float hysteresis) {
		std::vector<uint32_t> order(candidates.size());
		std::iota(order.begin(), order.end(), 0u);
		const auto effectiveScore = [&](uint32_t index) {
			const auto& candidate = candidates[index];
			return candidate.selectedLastFrame ? candidate.score * hysteresis : candidate.score;
		};
		std::stable_sort(order.begin(), order.end(),
			[&](uint32_t lhs, uint32_t rhs) { return effectiveScore(lhs) > effectiveScore(rhs); });
		std::vector<uint32_t> selected;
		uint32_t remaining = faceBudget;
		for (const uint32_t index : order) {
			const auto& candidate = candidates[index];
			if (candidate.faceCount == 0u || candidate.score <= 0.0f || candidate.faceCount > remaining) {
				continue;
			}
			remaining -= candidate.faceCount;
			selected.push_back(index);
		}
		return selected;
	}

	uint32_t AllocateLocalShadowFaces(std::span<bool> used, uint32_t count) {
		const auto size = static_cast<uint32_t>(used.size());
		if (count == 0u || count > size) {
			return size;
		}
		for (uint32_t first = 0; first + count <= size; ++first) {
			uint32_t run = 0;
			while (run < count && !used[first + run]) {
				++run;
			}
			if (run == count) {
				std::fill_n(used.begin() + first, count, true);
				return first;
			}
			first += run;
		}
		return size;
	}

	void FreeLocalShadowFaces(std::span<bool> used, uint32_t first, uint32_t count) {
		for (uint32_t face = first; face < first + count && face < used.size(); ++face) {
			used[face] = false;
		}
	}

	float LocalShadowScore(const LightInfo& light, const XMFLOAT3& cameraPosition) {
		if (light.range <= 0.0f) {
			return 0.0f;
		}
		const XMVECTOR toLight = XMVectorSubtract(light.posWorldSpace, XMLoadFloat3(&cameraPosition));
		const float distance = XMVectorGetX(XMVector3Length(XMVectorSetW(toLight, 0.0f)));
		return light.range / std::max(distance, light.range * 0.05f);
	}

} // namespace br::lighting
