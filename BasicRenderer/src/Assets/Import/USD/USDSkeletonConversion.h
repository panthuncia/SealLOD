#pragma once

#include "Assets/Import/USD/USDImportState.h"
#include <BasicRenderer/Scene/Components.h>
#include <DirectXMath.h>
#include <pxr/base/gf/transform.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>

namespace USDLoader {
    using namespace pxr;

static inline void SetEntityTransformFromUsdMatrix(
		flecs::entity entity,
		const GfMatrix4d& matrix,
		double metersPerUnit)
	{
		const GfTransform transform(matrix);
		const GfVec3d translation = transform.GetTranslation();
		const GfQuaternion rotation = transform.GetRotation().GetQuaternion();
		const GfVec3d scale = transform.GetScale();

		entity.set<Components::Position>({
			DirectX::XMFLOAT3(
				static_cast<float>(translation[0] * metersPerUnit),
				static_cast<float>(translation[1] * metersPerUnit),
				static_cast<float>(translation[2] * metersPerUnit))
			});
		entity.set<Components::Rotation>({
			DirectX::XMFLOAT4(
				static_cast<float>(rotation.GetImaginary()[0]),
				static_cast<float>(rotation.GetImaginary()[1]),
				static_cast<float>(rotation.GetImaginary()[2]),
				static_cast<float>(rotation.GetReal()))
			});
		entity.set<Components::Scale>({
			DirectX::XMFLOAT3(
				static_cast<float>(scale[0]),
				static_cast<float>(scale[1]),
				static_cast<float>(scale[2]))
		});
	}

static inline Components::Transform ComponentsTransformFromUsdMatrix(
		const GfMatrix4d& matrix,
		double metersPerUnit)
	{
		const GfTransform transform(matrix);
		const GfVec3d translation = transform.GetTranslation();
		const GfQuaternion rotation = transform.GetRotation().GetQuaternion();
		const GfVec3d scale = transform.GetScale();

		return Components::Transform(
			Components::Position(DirectX::XMFLOAT3(
				static_cast<float>(translation[0] * metersPerUnit),
				static_cast<float>(translation[1] * metersPerUnit),
				static_cast<float>(translation[2] * metersPerUnit))),
			Components::Rotation(DirectX::XMFLOAT4(
				static_cast<float>(rotation.GetImaginary()[0]),
				static_cast<float>(rotation.GetImaginary()[1]),
				static_cast<float>(rotation.GetImaginary()[2]),
				static_cast<float>(rotation.GetReal()))),
			Components::Scale(DirectX::XMFLOAT3(
				static_cast<float>(scale[0]),
				static_cast<float>(scale[1]),
				static_cast<float>(scale[2]))));
	}


static inline DirectX::XMMATRIX DirectXMatrixFromUsdMatrix(
		const GfMatrix4d& matrix,
		double metersPerUnit)
	{
		const GfTransform transform(matrix);
		const GfVec3d translation = transform.GetTranslation();
		const GfQuaternion rotation = transform.GetRotation().GetQuaternion();
		const GfVec3d scale = transform.GetScale();

		return DirectX::XMMatrixScaling(
			static_cast<float>(scale[0]),
			static_cast<float>(scale[1]),
			static_cast<float>(scale[2])) *
			DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(
				static_cast<float>(rotation.GetImaginary()[0]),
				static_cast<float>(rotation.GetImaginary()[1]),
				static_cast<float>(rotation.GetImaginary()[2]),
				static_cast<float>(rotation.GetReal()))) *
			DirectX::XMMatrixTranslation(
				static_cast<float>(translation[0] * metersPerUnit),
				static_cast<float>(translation[1] * metersPerUnit),
				static_cast<float>(translation[2] * metersPerUnit));
	}

static inline GfVec3d ExtractUsdMatrixTranslation(const GfMatrix4d& matrix)
	{
		return matrix.Transform(GfVec3d(0.0));
	}

static inline double UsdDistance(const GfVec3d& a, const GfVec3d& b)
	{
		const GfVec3d delta = a - b;
		return delta.GetLength();
	}

static inline std::string_view UsdJointLeafName(std::string_view jointName)
	{
		const size_t slash = jointName.find_last_of('/');
		return slash == std::string_view::npos ? jointName : jointName.substr(slash + 1u);
	}


std::shared_ptr<Skeleton> BuildBrNiflyTreeWindSkeleton(
    const pxr::UsdStageRefPtr& stage, const Mesh& mesh, std::string_view sourceIdentifier);
void AttachBrNiflyTreeWindSkeletons(
    const pxr::UsdStageRefPtr& stage, ImportedAssetPayload& payload, std::string_view sourceIdentifier);
std::shared_ptr<Skeleton> ProcessSkeleton(
    const pxr::UsdSkelSkeleton& skel, const pxr::VtTokenArray rawJointOrder,
    const pxr::UsdSkelSkeletonQuery& skelQuery, const std::shared_ptr<Scene>& scene, double metersPerUnit);
std::shared_ptr<Animation> ProcessAnimQuery(
    const pxr::UsdSkelAnimQuery& animQuery, const pxr::UsdStageRefPtr& stage,
    double metersPerUnit, const pxr::VtTokenArray& jointOrder);
std::shared_ptr<Skeleton> BuildPayloadSkeleton(
    const pxr::UsdSkelSkeleton& skel, const pxr::VtTokenArray& rawJointOrder,
    const pxr::UsdSkelSkeletonQuery& skelQuery, double metersPerUnit,
    bool sanitizeAssemblyHierarchy = false, const pxr::UsdStageRefPtr& stage = nullptr);
void AddPayloadSkeletonAnimation(
    const pxr::UsdSkelSkeleton& skel, pxr::UsdSkelCache& skelCache,
    const pxr::UsdStageRefPtr& stage, double metersPerUnit,
    const pxr::VtTokenArray& rawJointOrder, const std::shared_ptr<Skeleton>& skeleton);

}
