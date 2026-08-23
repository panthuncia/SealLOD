#include "Import/AssimpGeometryExtractor.h"

#include <algorithm>
#include <cstring>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <DirectXMath.h>
#include <spdlog/spdlog.h>

#include "Import/CLodCacheLoader.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Mesh/ClusterLODTypes.h"
#include "Mesh/VertexLayout.h"
#include "Mesh/VertexFlags.h"
#include "Mesh/DefaultCLodSettings.h"
#include "Utilities/CachePathUtilities.h"

namespace {

constexpr uint32_t kMaxSkinInfluences = 8u;

struct PackedSkinningInfluences
{
	DirectX::XMUINT4 joints0{ 0, 0, 0, 0 };
	DirectX::XMUINT4 joints1{ 0, 0, 0, 0 };
	DirectX::XMFLOAT4 weights0{ 0, 0, 0, 0 };
	DirectX::XMFLOAT4 weights1{ 0, 0, 0, 0 };
};

CLodCacheLoader::MeshCacheIdentity BuildAssimpCacheIdentity(
	const std::string& sourceFilePath,
	const aiMesh* mesh,
	unsigned int meshIndex)
{
	CLodCacheLoader::MeshCacheIdentity identity{};
	identity.sourceIdentifier = NormalizeCacheSourcePath(sourceFilePath);
	identity.primPath = "/Assimp/Mesh/" + std::to_string(meshIndex);
	if (mesh != nullptr && mesh->mName.length > 0) {
		identity.primPath += "/" + std::string(mesh->mName.C_Str());
	}
	identity.subsetName = "";
	return identity;
}

}

namespace AssimpGeometryExtractor {

ExtractionResult ExtractAll(const aiScene* pScene, const std::string& sourceFilePath) {
	ExtractionResult result;
	std::vector<std::optional<ExtractedMesh>> preprocessed(pScene->mNumMeshes);

	TaskSchedulerManager::GetInstance().ParallelFor("AssimpGeometryExtractor::PreprocessMeshes", pScene->mNumMeshes, [&](size_t meshIndex) {
		const unsigned int i = static_cast<unsigned int>(meshIndex);
		aiMesh* aMesh = pScene->mMeshes[i];

		const bool hasBones = aMesh->HasBones();
		const bool hasNormals = aMesh->HasNormals();
		const bool hasTexcoords = aMesh->HasTextureCoords(0);
		const bool hasColors = aMesh->HasVertexColors(0);
        std::vector<MeshUvSetData> uvSets;
        for (unsigned int uvSetIndex = 0; uvSetIndex < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++uvSetIndex) {
            if (!aMesh->HasTextureCoords(uvSetIndex)) {
                continue;
            }

            MeshUvSetData uvSet;
            uvSet.name = "UV" + std::to_string(uvSetIndex);
            uvSet.values.reserve(aMesh->mNumVertices);
            for (uint32_t v = 0; v < static_cast<uint32_t>(aMesh->mNumVertices); ++v) {
                uvSet.values.push_back(DirectX::XMFLOAT2{
                    aMesh->mTextureCoords[uvSetIndex][v].x,
                    -aMesh->mTextureCoords[uvSetIndex][v].y });
            }
            uvSets.push_back(std::move(uvSet));
        }

		unsigned int meshFlags = 0;
		if (hasNormals) {
			meshFlags |= VertexFlags::VERTEX_NORMALS;
		}
		if (hasTexcoords) {
			meshFlags |= VertexFlags::VERTEX_TEXCOORDS;
		}
		if (hasColors) {
			meshFlags |= VertexFlags::VERTEX_COLORS;
		}
		if (hasBones) {
			meshFlags |= VertexFlags::VERTEX_SKINNED;
		}

		const uint32_t numVertices = static_cast<uint32_t>(aMesh->mNumVertices);
		const unsigned int vertexSize = MeshVertexLayout::VertexSize(meshFlags);
		const unsigned int skinningVertexSize =
			sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3) + sizeof(PackedSkinningInfluences);

		// Pack vertex data
		std::vector<std::byte> rawData(static_cast<size_t>(numVertices) * vertexSize);
		std::vector<std::byte> skinningData;
		if (hasBones) {
			skinningData.resize(static_cast<size_t>(numVertices) * skinningVertexSize);
		}

		const DirectX::XMFLOAT3 defaultNormal{ 0.0f, 0.0f, 0.0f };
		for (uint32_t v = 0; v < numVertices; ++v) {
			const auto& pos = aMesh->mVertices[v];
			const DirectX::XMFLOAT3 position{ pos.x, pos.y, pos.z };
			const DirectX::XMFLOAT3 normal = hasNormals
				? DirectX::XMFLOAT3{ aMesh->mNormals[v].x, aMesh->mNormals[v].y, aMesh->mNormals[v].z }
				: defaultNormal;

			const size_t baseOffset = static_cast<size_t>(v) * vertexSize;
			std::memcpy(rawData.data() + baseOffset, &position, sizeof(position));
			std::memcpy(rawData.data() + baseOffset + MeshVertexLayout::NormalOffset, &normal, sizeof(normal));

			if (hasTexcoords) {
				const DirectX::XMFLOAT2 texcoord{ aMesh->mTextureCoords[0][v].x, -aMesh->mTextureCoords[0][v].y };
				std::memcpy(rawData.data() + baseOffset + MeshVertexLayout::TexcoordOffset(meshFlags), &texcoord, sizeof(texcoord));
			}

			if (hasColors) {
				const aiColor4D& color = aMesh->mColors[0][v];
				const DirectX::XMFLOAT3 packedColor{ color.r, color.g, color.b };
				std::memcpy(rawData.data() + baseOffset + MeshVertexLayout::ColorOffset(meshFlags), &packedColor, sizeof(packedColor));
			}

			if (hasBones) {
				const size_t skinBaseOffset = static_cast<size_t>(v) * skinningVertexSize;
				std::memcpy(skinningData.data() + skinBaseOffset, &position, sizeof(position));
				size_t skinOffset = sizeof(DirectX::XMFLOAT3);
				std::memcpy(skinningData.data() + skinBaseOffset + skinOffset, &normal, sizeof(normal));
			}
		}

		// Pack index data
		std::vector<UINT32> indices;
		indices.reserve(aMesh->mNumFaces * 3);
		for (unsigned int f = 0; f < aMesh->mNumFaces; f++) {
			const aiFace& face = aMesh->mFaces[f];
			if (face.mNumIndices != 3) {
				throw std::runtime_error("Assimp mesh contains non-triangle face; expected triangulated input");
			}
			for (unsigned int idx = 0; idx < face.mNumIndices; idx++) {
				indices.push_back(face.mIndices[idx]);
			}
		}

		if (hasBones) {
			struct VertexInfluence
			{
				uint32_t joint = 0;
				float weight = 0.0f;
			};

			std::vector<std::array<VertexInfluence, kMaxSkinInfluences>> vertexInfluences(aMesh->mNumVertices);
			std::vector<uint32_t> influenceCount(aMesh->mNumVertices, 0u);
			for (unsigned int b = 0; b < aMesh->mNumBones; b++) {
				aiBone* bone = aMesh->mBones[b];
				for (unsigned int w = 0; w < bone->mNumWeights; w++) {
					const aiVertexWeight& vw = bone->mWeights[w];
					unsigned int vertexID = vw.mVertexId;
					float weight = vw.mWeight;

					if (weight <= 0.0f) {
						continue;
					}

					auto& influences = vertexInfluences[vertexID];
					uint32_t& count = influenceCount[vertexID];
					if (count < kMaxSkinInfluences) {
						influences[count++] = VertexInfluence{ b, weight };
						continue;
					}

					uint32_t smallestIndex = 0u;
					for (uint32_t influenceIndex = 1; influenceIndex < kMaxSkinInfluences; ++influenceIndex) {
						if (influences[influenceIndex].weight < influences[smallestIndex].weight) {
							smallestIndex = influenceIndex;
						}
					}
					if (weight > influences[smallestIndex].weight) {
						influences[smallestIndex] = VertexInfluence{ b, weight };
					}
				}
			}

			for (uint32_t vertexID = 0; vertexID < numVertices; ++vertexID) {
				auto influences = vertexInfluences[vertexID];
				const uint32_t count = influenceCount[vertexID];
				std::sort(influences.begin(), influences.begin() + count, [](const VertexInfluence& lhs, const VertexInfluence& rhs) {
					return lhs.weight > rhs.weight;
				});

				float weightSum = 0.0f;
				for (uint32_t influenceIndex = 0; influenceIndex < count; ++influenceIndex) {
					weightSum += influences[influenceIndex].weight;
				}
				const float invWeightSum = weightSum > 0.0f ? (1.0f / weightSum) : 0.0f;

				PackedSkinningInfluences packed{};
				uint32_t jointValues[kMaxSkinInfluences] = {};
				float weightValues[kMaxSkinInfluences] = {};
				for (uint32_t influenceIndex = 0; influenceIndex < count; ++influenceIndex) {
					jointValues[influenceIndex] = influences[influenceIndex].joint;
					weightValues[influenceIndex] = influences[influenceIndex].weight * invWeightSum;
				}
				packed.joints0 = DirectX::XMUINT4(jointValues[0], jointValues[1], jointValues[2], jointValues[3]);
				packed.joints1 = DirectX::XMUINT4(jointValues[4], jointValues[5], jointValues[6], jointValues[7]);
				packed.weights0 = DirectX::XMFLOAT4(weightValues[0], weightValues[1], weightValues[2], weightValues[3]);
				packed.weights1 = DirectX::XMFLOAT4(weightValues[4], weightValues[5], weightValues[6], weightValues[7]);

				const size_t vertexBase = static_cast<size_t>(vertexID) * skinningVertexSize;
				const size_t influencesOffset = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3);
				std::memcpy(skinningData.data() + vertexBase + influencesOffset, &packed, sizeof(PackedSkinningInfluences));
			}
		}

		// CLod cache identity + try load
		auto cacheIdentity = BuildAssimpCacheIdentity(sourceFilePath, aMesh, i);
		auto prebuiltData = CLodCacheLoader::TryLoadPrebuilt(cacheIdentity);

		// Populate MeshIngestBuilder
		MeshIngestBuilder ingest(vertexSize, hasBones ? skinningVertexSize : 0, meshFlags,
			GetDefaultBuilderSettings(cacheIdentity.sourceIdentifier));
        ingest.SetUvSets(std::move(uvSets));
		ingest.ReserveVertices(numVertices);
		if (hasBones) {
			ingest.ReserveVertices(numVertices);
		}
		for (uint32_t v = 0; v < numVertices; ++v) {
			const size_t baseOffset = static_cast<size_t>(v) * vertexSize;
			ingest.AppendVertexBytes(rawData.data() + baseOffset, vertexSize);
			if (hasBones) {
				const size_t skinBaseOffset = static_cast<size_t>(v) * skinningVertexSize;
				ingest.AppendSkinningVertexBytes(skinningData.data() + skinBaseOffset, skinningVertexSize);
			}
		}

		ingest.ReserveIndices(indices.size());
		ingest.AppendIndices(indices.data(), indices.size());

		// Build CLod cache if needed
		if (!prebuiltData.has_value()) {
			ClusterLODPrebuildArtifacts artifacts = ingest.BuildClusterLODArtifacts();
			ClusterLODPrebuiltData savedPrebuiltData;

			auto loadedBeforeSave = CLodCacheLoader::TryLoadPrebuilt(cacheIdentity);
			if (loadedBeforeSave.has_value()) {
				prebuiltData = std::move(loadedBeforeSave);
			}
			else if (CLodCacheLoader::SavePrebuiltLocked(cacheIdentity, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload(), &savedPrebuiltData)) {
				auto diskBackedPrebuilt = CLodCacheLoader::TryLoadPrebuilt(cacheIdentity);
				if (diskBackedPrebuilt.has_value()) {
					prebuiltData = std::move(diskBackedPrebuilt);
				}
				else {
					spdlog::warn("Immediate CLOD cache reload missed after save for {} (mesh {}); using saved disk metadata directly.", sourceFilePath, i);
					prebuiltData = std::move(savedPrebuiltData);
				}
			}
			else {
				spdlog::warn("Failed to save CLOD cache for {} (mesh {})", sourceFilePath, i);
				prebuiltData = std::move(artifacts.prebuiltData);
			}
		}

		preprocessed[i].emplace(
			i,
			aMesh->mMaterialIndex,
			hasBones,
			MeshPreprocessResult(std::move(ingest), std::move(cacheIdentity), std::move(prebuiltData)));
		});

	result.meshes.reserve(pScene->mNumMeshes);
	for (auto& mesh : preprocessed) {
		if (!mesh.has_value()) {
			throw std::runtime_error("Missing preprocessed Assimp mesh data");
		}
		result.meshes.push_back(std::move(mesh.value()));
	}

	return result;
}

ExtractionResult ExtractAll(const std::string& filePath) {
	Assimp::Importer importer;
	constexpr unsigned int kAssimpProcessFlags =
		aiProcess_Triangulate |
		aiProcess_FlipUVs |
		aiProcess_OptimizeGraph |
		aiProcess_OptimizeMeshes;
	const aiScene* pScene = importer.ReadFile(filePath, kAssimpProcessFlags);

	if (!pScene || pScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !pScene->mRootNode) {
		spdlog::error("Assimp loading failed for {}. Error: {}", filePath, importer.GetErrorString());
		return {};
	}

	return ExtractAll(pScene, filePath);
}

} // namespace AssimpGeometryExtractor
