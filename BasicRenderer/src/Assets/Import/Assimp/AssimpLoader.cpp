#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <spdlog/spdlog.h>
#include <DirectXMath.h>
#include <filesystem>
#include <vector>
#include <cstring>
#include <rhi.h>
#include <algorithm>

#include "BasicRenderer/Assets/Material.h"
#include <BasicRenderer/Assets/MaterialFlags.h>
#include "Resources/Sampler.h"
#include "BasicRenderer/Assets/Import/Filetypes.h"
#include "BasicRenderer/Scene/Scene.h"
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include <BasicRenderer/Assets/Import/CLodCacheLoader.h>
#include "BasicRenderer/Scene/Animation/Skeleton.h"
#include "Assets/Textures/Processing/TextureProcessingManager.h"
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/Animation/AnimationController.h"
#include "Resources/PixelBuffer.h"
#include "Assets/Import/Assimp/AssimpLoader.h"
#include <BasicRenderer/Assets/Import/AssimpGeometryExtractor.h>
#include "Utilities/Utilities.h"

namespace AssimpLoader {
    constexpr uint32_t kMaterialTextureMaxAnisotropy = 16;

    static std::string NormalizeTextureIdentity(const std::filesystem::path& path) {
        std::error_code ec;
        auto normalized = std::filesystem::weakly_canonical(path, ec);
        if (ec) {
            normalized = path.lexically_normal();
        }

        auto key = normalized.generic_string();
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return key;
    }

    static std::filesystem::path ResolveAssimpTexturePath(
        const std::filesystem::path& sourceFilePath,
        const std::string& texPath) {
        const std::filesystem::path candidate(texPath);
        if (candidate.is_absolute()) {
            return candidate;
        }

        return sourceFilePath.parent_path() / candidate;
    }

    static std::string BuildAssimpTextureSourceIdentity(
        const std::filesystem::path& sourceFilePath,
        const std::string& texPath) {
        if (!texPath.empty() && texPath[0] == '*') {
            return NormalizeTextureIdentity(sourceFilePath) + "#embedded:" + texPath;
        }

        return NormalizeTextureIdentity(ResolveAssimpTexturePath(sourceFilePath, texPath));
    }

    static std::string BuildAssimpCacheProbePath(
        const std::filesystem::path& sourceFilePath,
        const std::string& texPath) {
        if (!texPath.empty() && texPath[0] == '*') {
            return (sourceFilePath.string() + "#" + texPath);
        }

        return ResolveAssimpTexturePath(sourceFilePath, texPath).string();
    }

    rhi::AddressMode aiTextureMapModeToRHI(aiTextureMapMode mode) {
        switch (mode) {
        case aiTextureMapMode_Wrap:   return rhi::AddressMode::Wrap;
        case aiTextureMapMode_Clamp:  return rhi::AddressMode::Clamp;
        case aiTextureMapMode_Mirror: return rhi::AddressMode::Mirror;
        default:                      return rhi::AddressMode::Wrap;
        }
    }

    static bool isSRGBTextureType(aiTextureType t) {
        switch (t) {
        case aiTextureType_DIFFUSE:
        case aiTextureType_BASE_COLOR:
        case aiTextureType_EMISSIVE:
        case aiTextureType_EMISSION_COLOR:
            return true; // color space textures
        default:
            return false; // data textures: normals, metalness, roughness, ao, height, etc.
        }
    }

    static TextureSemantic aiTextureTypeToSemantic(aiTextureType type) {
        switch (type) {
        case aiTextureType_DIFFUSE:
        case aiTextureType_BASE_COLOR:
            return TextureSemantic::BaseColor;
        case aiTextureType_NORMALS:
            return TextureSemantic::Normal;
        case aiTextureType_METALNESS:
            return TextureSemantic::Metallic;
        case aiTextureType_DIFFUSE_ROUGHNESS:
            return TextureSemantic::Roughness;
        case aiTextureType_AMBIENT_OCCLUSION:
        case aiTextureType_LIGHTMAP:
            return TextureSemantic::AO;
        case aiTextureType_EMISSIVE:
        case aiTextureType_EMISSION_COLOR:
            return TextureSemantic::Emissive;
        case aiTextureType_HEIGHT:
        case aiTextureType_DISPLACEMENT:
            return TextureSemantic::Height;
        default:
            return TextureSemantic::Unknown;
        }
    }

    static std::shared_ptr<TextureAsset> loadAiTexture(
        const aiScene* scene,
        const std::filesystem::path& sourceFilePath,
        const std::string& texPath,          // "*0" for embedded or file path
        const TextureProcessingSettings& processingSettings,
        std::shared_ptr<org::Sampler> sampler,
        bool preferSRGB
    )
    {
        TextureFileMeta cacheProbeMeta{};
        cacheProbeMeta.filePath = BuildAssimpCacheProbePath(sourceFilePath, texPath);
        cacheProbeMeta.preferSRGB = preferSRGB;
        cacheProbeMeta.processing = processingSettings;

        const std::wstring cachePath = TextureProcessingManager::GetInstance().GetExistingCachePathForFile(cacheProbeMeta);
        if (!cachePath.empty()) {
            auto cachedTexture = LoadTextureFromFile(cachePath, sampler, preferSRGB);
            cachedTexture->Meta().filePath = cacheProbeMeta.filePath;
            cachedTexture->Meta().isProcessingCacheArtifact = true;
            cachedTexture->Meta().preferSRGB = preferSRGB;
            cachedTexture->SetProcessingSettings(processingSettings);
            spdlog::info("AssimpLoader: texture processing cache hit for '{}' -> '{}'", cacheProbeMeta.filePath, ws2s(cachePath));
            return cachedTexture;
        }

        // Embedded?
        if (!texPath.empty() && texPath[0] == '*') {
            unsigned int textureIndex = std::atoi(texPath.c_str() + 1);
            if (textureIndex >= scene->mNumTextures) {
                throw std::runtime_error("Embedded texture index out of range: " + texPath);
            }

            aiTexture* aiTex = scene->mTextures[textureIndex];
            if (!aiTex) {
                throw std::runtime_error("Null embedded texture at index: " + std::to_string(textureIndex));
            }

            // Compressed (mHeight==0): directly feed container bytes to LoadTextureFromMemory
            if (aiTex->mHeight == 0) {
                const void* bytes = reinterpret_cast<const void*>(aiTex->pcData);
                const size_t byteCount = static_cast<size_t>(aiTex->mWidth);
                LoadFlags lf{};
                // WIC decode is most common; FORCE_* handles PNG/JPG without relying on file metadata.
                lf.wic = preferSRGB ? DirectX::WIC_FLAGS_FORCE_SRGB : DirectX::WIC_FLAGS_FORCE_LINEAR;
                auto texture = LoadTextureFromMemory(bytes, byteCount, sampler, lf, preferSRGB);
                texture->Meta().filePath = cacheProbeMeta.filePath;
                texture->Meta().preferSRGB = preferSRGB;
                texture->SetProcessingSettings(processingSettings);
                return texture;
            }

            // Raw BGRA (mHeight != 0): create PixelBuffer directly; choose sRGB format when requested
            {
                const unsigned int width = aiTex->mWidth;
                const unsigned int height = aiTex->mHeight;
                const unsigned int channels = 4;

                org::TextureDescription desc;
                org::ImageDimensions dims;
                dims.width = width;
                dims.height = height;
                dims.rowPitch = width * 4;
                dims.slicePitch = width * height * 4;
                desc.imageDimensions.push_back(dims);
                desc.channels = static_cast<unsigned short>(channels);
                desc.format = preferSRGB ? rhi::Format::R8G8B8A8_UNorm_sRGB : rhi::Format::R8G8B8A8_UNorm;

                // aiTex->pcData is BGRA
                auto rawData = std::make_shared<std::vector<uint8_t>>(
                    static_cast<size_t>(width) * static_cast<size_t>(height) * channels
                );

                const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
                for (size_t i = 0; i < pixelCount; ++i) {
                    const auto& p = aiTex->pcData[i]; // aiTex->pcData is aiTexel*

                    (*rawData)[i * 4 + 0] = p.b;
                    (*rawData)[i * 4 + 1] = p.g;
                    (*rawData)[i * 4 + 2] = p.r;
                    (*rawData)[i * 4 + 3] = p.a;
                }

				std::vector vec = { rawData };

				desc.generateMipMaps = true;
            	
            	TextureFileMeta meta;
				meta.filePath = cacheProbeMeta.filePath;
				meta.preferSRGB = preferSRGB;

                auto texture = TextureAsset::CreateShared(desc, vec, sampler, meta);
                texture->SetProcessingSettings(processingSettings);
                return texture;
            }
        }

        // External file: load from disk
        {
            auto texture = LoadTextureFromFile(s2ws(cacheProbeMeta.filePath), sampler, preferSRGB);
            texture->Meta().preferSRGB = preferSRGB;
            texture->SetProcessingSettings(processingSettings);
            return texture;
        }
    }

    std::vector<std::shared_ptr<Material>> LoadMaterialsFromAssimpScene(
        const aiScene* scene,
        const std::string& sourceFilePath
    )
    {
        const std::filesystem::path sourcePath(sourceFilePath);
        std::vector<std::shared_ptr<TextureAsset>> textures;
        std::vector<std::shared_ptr<Material>> materials;

        // To avoid reloading the same texture multiple times:
        std::unordered_map<std::string, std::shared_ptr<TextureAsset>> loadedTextures;

        // Iterate over every material, check each texture slot
        for (unsigned int mIndex = 0; mIndex < scene->mNumMaterials; ++mIndex)
        {
            aiMaterial* mat = scene->mMaterials[mIndex];
            if (!mat) continue;

            // Ttexture types we might care about:
            static const aiTextureType textureTypes[] = {
                aiTextureType_DIFFUSE,
                //aiTextureType_SPECULAR,
                aiTextureType_NORMALS,
                aiTextureType_METALNESS,       // for PBR extension
                aiTextureType_DIFFUSE_ROUGHNESS, // for PBR extension
                aiTextureType_EMISSIVE,
                aiTextureType_AMBIENT_OCCLUSION,
                aiTextureType_LIGHTMAP,
                aiTextureType_EMISSIVE,
                aiTextureType_EMISSION_COLOR,
                aiTextureType_HEIGHT,
                aiTextureType_DISPLACEMENT,
            };
            std::unordered_map<aiTextureType, std::shared_ptr<TextureAsset>> materialTextures;
            auto textureFor = [&](aiTextureType type) -> std::shared_ptr<TextureAsset> {
                auto it = materialTextures.find(type);
                return it != materialTextures.end() ? it->second : nullptr;
            };

            for (aiTextureType tType : textureTypes)
            {
                unsigned int texCount = mat->GetTextureCount(tType);
                if (texCount > 1) {
                    throw std::exception("More than one texture per slot not yet supported");
                }
                for (unsigned int tIndex = 0; tIndex < texCount; ++tIndex)
                {
                    aiTextureMapping mapping = {};
                    ai_real blend = {};
                    aiTextureOp op = {};
                    aiTextureMapMode mapmode[2] = {};
                    aiString aiTexPath = {};
                    unsigned int uvIndex = 0;

                    if (aiGetMaterialTexture(mat, tType, tIndex, &aiTexPath, &mapping, &uvIndex, &blend, &op, mapmode, nullptr) == AI_SUCCESS)
                    {
                        std::string texPath = aiTexPath.C_Str(); // e.g. "*0" or "texture.png"
                        // Check if we already loaded it:
                        const bool preferSRGB = isSRGBTextureType(tType);
                        const TextureSemantic semantic = aiTextureTypeToSemantic(tType);
                        const NormalMapConvention normalConvention = semantic == TextureSemantic::Normal
                            ? NormalMapConvention::DirectX
                            : NormalMapConvention::DirectX;
                        const std::string textureSourceIdentity = BuildAssimpTextureSourceIdentity(sourcePath, texPath);
                        const std::string textureCacheKey = textureSourceIdentity + "|semantic:" + std::to_string(static_cast<uint32_t>(semantic)) +
                            (preferSRGB ? "|srgb" : "|linear") +
                            "|normalconv:" + std::to_string(static_cast<uint32_t>(normalConvention));
                        auto it = loadedTextures.find(textureCacheKey);

                        if (it == loadedTextures.end())
                        {
							rhi::SamplerDesc samplerDesc = {};
							samplerDesc.magFilter = rhi::Filter::Linear;
							samplerDesc.minFilter = rhi::Filter::Linear;
							samplerDesc.mipFilter = rhi::MipFilter::Linear;
							samplerDesc.addressU = aiTextureMapModeToRHI(mapmode[0]);
							samplerDesc.addressV = aiTextureMapModeToRHI(mapmode[1]);
							samplerDesc.addressW = rhi::AddressMode::Wrap; // 3D textures not supported
							samplerDesc.mipLodBias = 0.0f;
							samplerDesc.maxAnisotropy = kMaterialTextureMaxAnisotropy;
							samplerDesc.compareEnable = false;
							samplerDesc.compareOp = rhi::CompareOp::Never;
							samplerDesc.borderPreset = rhi::BorderPreset::OpaqueWhite;
							samplerDesc.minLod = 0.0f;
							samplerDesc.maxLod = FLT_MAX;


                            std::shared_ptr<org::Sampler> sampler = org::Sampler::CreateSampler(samplerDesc);

                            // Not loaded yet, load now
                            try {
                                const TextureProcessingSettings processingSettings = MakeMaterialTextureProcessingSettings(
                                    semantic,
                                    preferSRGB,
                                    textureCacheKey,
                                    false,
                                    normalConvention);
                                std::shared_ptr<TextureAsset> newTex = loadAiTexture(
                                    scene,
                                    sourcePath,
                                    texPath,
                                    processingSettings,
                                    sampler,
                                    preferSRGB
                                );
                                if (newTex) {
                                    loadedTextures[textureCacheKey] = newTex;
                                    materialTextures[tType] = newTex;
                                }
                            }
                            catch (const std::exception& e) {
                                spdlog::error("Failed loading texture {}: {}", texPath, e.what());
                            }
                        }
                        else
                        {
                            // Already loaded, just use the existing one
                            materialTextures[tType] = it->second;
                        }
                    }
                }
            }

            UINT materialFlags = 0;
            UINT psoFlags = 0;

            // Basic properties
            aiColor4D diffuse(1.f, 1.f, 1.f, 1.f);
            mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);

            // Emissive factor (if available)
            aiColor3D emissive(0.f, 0.f, 0.f);
            mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive);

            float metallicFactor = 0.f;
            mat->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor);

            float roughnessFactor = 1.f;
            mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor);

            materialFlags |= MaterialFlags::MATERIAL_PBR; // TODO: Non-PBR materials

            // For alpha, blending, doubleSided
            float alphaCutoff = 0.5f;
            BlendState blendMode = BlendState::BLEND_STATE_OPAQUE;

            std::shared_ptr<TextureAsset> baseColorTexture = nullptr;
            std::shared_ptr<TextureAsset> normalTexture = nullptr;
            std::shared_ptr<TextureAsset> metallicTex = nullptr;
            std::shared_ptr<TextureAsset> roughnessTex = nullptr;
            std::shared_ptr<TextureAsset> aoMap = nullptr;
            std::shared_ptr<TextureAsset> emissiveTexture = nullptr;
            std::shared_ptr<TextureAsset> heightMap = nullptr;

            if (materialTextures.find(aiTextureType_DIFFUSE) != materialTextures.end()) {
                baseColorTexture = materialTextures[aiTextureType_DIFFUSE];
                if (!baseColorTexture->Meta().alphaIsAllOpaque) {
                    materialFlags |= MaterialFlags::MATERIAL_DOUBLE_SIDED;
                    blendMode = BlendState::BLEND_STATE_MASK;
                }
                materialFlags |= MaterialFlags::MATERIAL_BASE_COLOR_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
            }
            if (materialTextures.find(aiTextureType_BASE_COLOR) != materialTextures.end()) {
                if (baseColorTexture != nullptr) {
                    spdlog::warn("Material {} has both BASE_COLOR and DIFFUSE textures. Using BASE_COLOR", mIndex);
                }
                baseColorTexture = materialTextures[aiTextureType_BASE_COLOR];
                materialFlags |= MaterialFlags::MATERIAL_BASE_COLOR_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
                if (!baseColorTexture->Meta().alphaIsAllOpaque) {
                    materialFlags |= MaterialFlags::MATERIAL_DOUBLE_SIDED;
                    blendMode = BlendState::BLEND_STATE_MASK;
                }
            }
			bool negateNormals = false;
            if (materialTextures.find(aiTextureType_NORMALS) != materialTextures.end()) {
                normalTexture = materialTextures[aiTextureType_NORMALS];
                materialFlags |= MaterialFlags::MATERIAL_NORMAL_MAP | MaterialFlags::MATERIAL_TEXTURED;
                if (normalTexture->Meta().fileType == ImageFiletype::DDS ||
                    (normalTexture->Meta().isProcessingCacheArtifact && normalTexture->Meta().processing.semantic == TextureSemantic::Normal)) {
                    negateNormals = true;
                }
            }
            if (materialTextures.find(aiTextureType_METALNESS) != materialTextures.end()) {
                metallicTex = materialTextures[aiTextureType_METALNESS];
                materialFlags |= MaterialFlags::MATERIAL_PBR | MaterialFlags::MATERIAL_METALLIC_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
            }
            if (materialTextures.find(aiTextureType_DIFFUSE_ROUGHNESS) != materialTextures.end()) {
                roughnessTex = materialTextures[aiTextureType_DIFFUSE_ROUGHNESS];
                materialFlags |= MaterialFlags::MATERIAL_PBR | MaterialFlags::MATERIAL_ROUGHNESS_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
            }
            if (materialTextures.find(aiTextureType_AMBIENT_OCCLUSION) != materialTextures.end()) {
                aoMap = materialTextures[aiTextureType_AMBIENT_OCCLUSION];
                materialFlags |= MaterialFlags::MATERIAL_AO_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
            }
            if (materialTextures.find(aiTextureType_LIGHTMAP) != materialTextures.end()) {
                if (aoMap != nullptr) {
                    spdlog::warn("Material {} has both AMBIENT_OCCLUSION and LIGHTMAP textures. Using LIGHTMAP", mIndex);
                }
                aoMap = materialTextures[aiTextureType_LIGHTMAP];
                materialFlags |= MaterialFlags::MATERIAL_AO_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
            }

            if (materialTextures.find(aiTextureType_EMISSIVE) != materialTextures.end()) {
                emissiveTexture = materialTextures[aiTextureType_EMISSIVE];
                materialFlags |= MaterialFlags::MATERIAL_EMISSIVE_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
            }
            emissive = aiColor3D(emissive[0] * 10.0f, emissive[1] * 10.0f, emissive[2] * 10.0f);

            if (materialTextures.find(aiTextureType_EMISSION_COLOR) != materialTextures.end()) {
                if (emissiveTexture != nullptr) {
                    spdlog::warn("Material {} has both EMISSION_COLOR and EMISSIVE textures. Using EMISSION_COLOR", mIndex);
                }
                emissiveTexture = materialTextures[aiTextureType_EMISSION_COLOR];
                materialFlags |= MaterialFlags::MATERIAL_EMISSIVE_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
            }

            if (materialTextures.find(aiTextureType_HEIGHT) != materialTextures.end()) {
                heightMap = materialTextures[aiTextureType_HEIGHT];
                materialFlags |= MaterialFlags::MATERIAL_PARALLAX | MaterialFlags::MATERIAL_TEXTURED;
            }
            if (materialTextures.find(aiTextureType_DISPLACEMENT) != materialTextures.end()) {
                if (heightMap != nullptr) {
                    spdlog::warn("Material {} has both HEIGHT and DISPLACEMENT textures. Using DISPLACEMENT", mIndex);
                }
                heightMap = materialTextures[aiTextureType_DISPLACEMENT];
                materialFlags |= MaterialFlags::MATERIAL_PARALLAX | MaterialFlags::MATERIAL_TEXTURED;
            }

            // If the material is two-sided
            bool twoSided = false;
            if (mat->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS && twoSided) {
                materialFlags |= MaterialFlags::MATERIAL_DOUBLE_SIDED;
                blendMode = BlendState::BLEND_STATE_MASK;
            }

            float opacity = 1.0f;
            if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 1.0f) {
                if (opacity == 0.0f) {
                    spdlog::warn("Material {} is fully transparent. Was this intentional?");
                }
                else {
                    blendMode = BlendState::BLEND_STATE_BLEND;
                    diffuse.a *= opacity;
                }
            }

            float transparencyFactor = 0.0f;
            if (mat->Get(AI_MATKEY_TRANSPARENCYFACTOR, transparencyFactor) == AI_SUCCESS && transparencyFactor > 0.0f) {
                blendMode = BlendState::BLEND_STATE_BLEND;
            }

            aiString matName;
            mat->Get(AI_MATKEY_NAME, matName);
            std::string mName = matName.C_Str();
            if (mName.empty()) mName = "Material_" + std::to_string(mIndex);

            DirectX::XMFLOAT4 baseColorFactor(diffuse.r, diffuse.g, diffuse.b, diffuse.a);
            DirectX::XMFLOAT4 emissiveFactor(emissive.r, emissive.g, emissive.b, 1.f);

			MaterialDescription desc;
			desc.name = mName;
			desc.alphaCutoff = alphaCutoff;
			desc.diffuseColor = baseColorFactor;
			desc.emissiveColor = emissiveFactor;
			desc.blendState = blendMode;
			desc.negateNormals = negateNormals; // TODO: How to handle this properly?
			desc.invertNormalGreen = false;
            desc.enableGeometricDisplacement = materialTextures.find(aiTextureType_DISPLACEMENT) != materialTextures.end();
            if (desc.enableGeometricDisplacement) {
                desc.geometricDisplacementMin = 0.0f;
                desc.geometricDisplacementMax = desc.heightMapScale;
            }
            desc.aoMap = { aoMap, { 1 }, { 0 } };
			desc.baseColor = { baseColorTexture, { 1 }, { 0, 1, 2, 3 } };
			desc.normal = { normalTexture, { 1 }, { 0, 1, 2 } };
			desc.heightMap = { heightMap, { 1 }, { 0 } };
			desc.metallic = { metallicTex, { metallicFactor }, { 2 } };
			desc.roughness = { roughnessTex, { roughnessFactor }, { 1 } };
			desc.emissive = { emissiveTexture, { 1 }, { 0, 1, 2 } };

            auto setUvSetIndex = [&](TextureAndConstant& target, aiTextureType textureType) {
                if (target.texture == nullptr) {
                    return;
                }

                unsigned int uvIndex = 0;
                if (mat->GetTextureCount(textureType) > 0) {
                    aiTextureMapping mapping = {};
                    ai_real blend = {};
                    aiTextureOp op = {};
                    aiTextureMapMode mapmode[2] = {};
                    aiString aiTexPath = {};
                    aiGetMaterialTexture(mat, textureType, 0, &aiTexPath, &mapping, &uvIndex, &blend, &op, mapmode, nullptr);
                }
                target.uvSetIndex = uvIndex;
            };

            setUvSetIndex(desc.baseColor, baseColorTexture == textureFor(aiTextureType_BASE_COLOR) ? aiTextureType_BASE_COLOR : aiTextureType_DIFFUSE);
            setUvSetIndex(desc.normal, aiTextureType_NORMALS);
            setUvSetIndex(desc.metallic, aiTextureType_METALNESS);
            setUvSetIndex(desc.roughness, aiTextureType_DIFFUSE_ROUGHNESS);
            setUvSetIndex(desc.emissive, emissiveTexture == textureFor(aiTextureType_EMISSION_COLOR) ? aiTextureType_EMISSION_COLOR : aiTextureType_EMISSIVE);
            setUvSetIndex(desc.heightMap, heightMap == textureFor(aiTextureType_DISPLACEMENT) ? aiTextureType_DISPLACEMENT : aiTextureType_HEIGHT);
            if (aoMap == textureFor(aiTextureType_LIGHTMAP)) {
                setUvSetIndex(desc.aoMap, aiTextureType_LIGHTMAP);
            }
            else {
                setUvSetIndex(desc.aoMap, aiTextureType_AMBIENT_OCCLUSION);
            }

            auto newMaterial = Material::CreateShared(desc);
            
            materials.push_back(newMaterial);
        }

        return materials;
    }

    static std::pair<std::vector<std::shared_ptr<Mesh>>, std::vector<int>> parseAiMeshes(
        const aiScene* pScene,
        const std::vector<std::shared_ptr<Material>>& materials,
        const std::string& sourceFilePath
    )
    {
        // Geometry extraction + CLod cache building
        auto extraction = AssimpGeometryExtractor::ExtractAll(pScene, sourceFilePath);

        std::vector<std::shared_ptr<Mesh>> meshes;
        meshes.reserve(extraction.meshes.size());
        std::vector<int> meshSkinIndices;
        meshSkinIndices.reserve(extraction.meshes.size());

        int currentSkinIndex = -1;
        for (auto& em : extraction.meshes) {
            if (em.hasBones) {
                currentSkinIndex++;
                meshSkinIndices.push_back(currentSkinIndex);
            }
            else {
                meshSkinIndices.push_back(-1);
            }

            std::shared_ptr<Material> material = nullptr;
            if (em.materialIndex < materials.size()) {
                material = materials[em.materialIndex];
            }

            // Phase 2: GPU mesh creation
            auto mesh = em.result.ingest.Build(
                material,
                std::move(em.result.prebuiltData),
                MeshCpuDataPolicy::ReleaseAfterUpload);

            meshes.push_back(mesh);
        }

        return { meshes, meshSkinIndices };
    }

    static void buildAiNodeHierarchy(
        std::shared_ptr<Scene> scene,
        aiNode* ainode,
        const aiScene* pScene,
        const std::vector<std::shared_ptr<Mesh>>& meshes,
        std::vector<flecs::entity>& outNodes,
        std::unordered_map<std::string, flecs::entity>& nodeMap,
        flecs::entity parent = flecs::entity()) {

        std::string nodeName(ainode->mName.C_Str());

        // For local transform: aiNode->mTransformation is a 4x4 matrix
        // Convert it to an XMMATRIX
        aiMatrix4x4 m = ainode->mTransformation;

        DirectX::XMMATRIX transform = XMMatrixSet( // Transpose
            m.a1, m.b1, m.c1, m.d1,
            m.a2, m.b2, m.c2, m.d2,
            m.a3, m.b3, m.c3, m.d3,
            m.a4, m.b4, m.c4, m.d4
        );

        flecs::entity entity;
        // handle multiple meshes per node
        if (ainode->mNumMeshes > 0) {
            std::vector<std::shared_ptr<Mesh>> objectMeshes;
            objectMeshes.reserve(ainode->mNumMeshes);
            for (unsigned int i = 0; i < ainode->mNumMeshes; i++) {
                int meshIndex = ainode->mMeshes[i];
                objectMeshes.push_back(meshes[meshIndex]);
            }
            entity = scene->CreateRenderableEntityECS(objectMeshes, s2ws(nodeName));
        }
        else {
            entity = scene->CreateNodeECS(s2ws(nodeName));
        }

        // Decompose the transform into T/R/S
        XMVECTOR s, r, t;
        XMMatrixDecompose(&s, &r, &t, transform);

        // Set the local transform
        entity.set<Components::Rotation>({ r });
        entity.set<Components::Position>({ t });
        entity.set<Components::Scale>({ s });

        // Attach to parent
        if (parent) {
            entity.child_of(parent);
        }
        else {
            spdlog::warn("Node {} has no parent", nodeName);
        }
        outNodes.push_back(entity);

        nodeMap[nodeName] = entity;
        // Recursively process children
        for (unsigned int c = 0; c < ainode->mNumChildren; ++c) {
            buildAiNodeHierarchy(scene, ainode->mChildren[c], pScene, meshes, outNodes, nodeMap, entity);
        }
    }

    static std::vector<std::shared_ptr<Animation>> parseAiAnimations(
        const aiScene* pScene,
        const std::vector<flecs::entity>& nodes,
        const std::unordered_map<std::string, flecs::entity>& nodeMap
    )
    {
        std::vector<std::shared_ptr<Animation>> animations;
        animations.reserve(pScene->mNumAnimations);

        for (unsigned int i = 0; i < pScene->mNumAnimations; i++) {
            aiAnimation* aiAnim = pScene->mAnimations[i];
            std::string animName = aiAnim->mName.length > 0 ? aiAnim->mName.C_Str() : ("Anim_" + std::to_string(i));

            auto animation = std::make_shared<Animation>(animName);

            // Each aiAnimation has channels -> each channel is for one node
            for (unsigned int c = 0; c < aiAnim->mNumChannels; c++) {
                aiNodeAnim* channel = aiAnim->mChannels[c];
                std::string nodeName = channel->mNodeName.C_Str();

                // Find the node that matches nodeName
                flecs::entity node;
                bool found = false;
                if (nodeMap.find(nodeName) != nodeMap.end()) {
                    node = nodeMap.at(nodeName);
                    found = true;
                }

                if (!found) {
                    // This channel references a node we didn�t find
                    spdlog::warn("Animation {} references unknown node: {}", animName, nodeName);
                    continue;
                }

                // Ensure we have an AnimationClip for that node in this animation
                if (animation->nodesMap.find(nodeName) == animation->nodesMap.end()) {
                    animation->nodesMap[nodeName] = std::make_shared<AnimationClip>();
                }
                auto& clip = animation->nodesMap[nodeName];

                // For position keys
                for (unsigned int k = 0; k < channel->mNumPositionKeys; k++) {
                    float time = static_cast<float>(channel->mPositionKeys[k].mTime / aiAnim->mTicksPerSecond);
                    const aiVector3D& v = channel->mPositionKeys[k].mValue;
                    clip->addPositionKeyframe(time, DirectX::XMFLOAT3(v.x, v.y, v.z));
                }

                // For rotation keys
                for (unsigned int k = 0; k < channel->mNumRotationKeys; k++) {
                    float time = static_cast<float>(channel->mRotationKeys[k].mTime / aiAnim->mTicksPerSecond);
                    const aiQuaternion& q = channel->mRotationKeys[k].mValue;
                    // Convert to XMVECTOR
                    XMVECTOR quat = XMVectorSet(q.x, q.y, q.z, q.w);
                    clip->addRotationKeyframe(time, quat);
                }

                // For scale keys
                for (unsigned int k = 0; k < channel->mNumScalingKeys; k++) {
                    float time = static_cast<float>(channel->mScalingKeys[k].mTime / aiAnim->mTicksPerSecond);
                    const aiVector3D& s = channel->mScalingKeys[k].mValue;
                    clip->addScaleKeyframe(time, DirectX::XMFLOAT3(s.x, s.y, s.z));
                }
            }

            animations.push_back(animation);
        }

        return animations;
    }

    static std::shared_ptr<Skeleton> parseSkeletonForMesh(
        aiMesh* aMesh,
        const std::unordered_map<std::string, flecs::entity>& nodeMap,
        const std::vector<std::shared_ptr<Animation>>& animations) {
        if (!aMesh->HasBones()) {
            return nullptr;
        }
        // Collect inverse bind matrices for each bone
        std::vector<XMMATRIX> inverseBindMatrices;
        inverseBindMatrices.reserve(aMesh->mNumBones);
        std::vector<flecs::entity> jointNodes;
        jointNodes.reserve(aMesh->mNumBones);

        for (unsigned int b = 0; b < aMesh->mNumBones; b++) {
            aiBone* bone = aMesh->mBones[b];

            // Convert offset matrix
            aiMatrix4x4 o = bone->mOffsetMatrix;
            DirectX::XMMATRIX offset = XMMatrixSet( // Transpose
                o.a1, o.b1, o.c1, o.d1,
                o.a2, o.b2, o.c2, o.d2,
                o.a3, o.b3, o.c3, o.d3,
                o.a4, o.b4, o.c4, o.d4
            );

            inverseBindMatrices.push_back(offset);

            // Find the node that corresponds to this bone
            std::string boneName = bone->mName.C_Str();
            flecs::entity boneNode;
            bool found = false;
            if (nodeMap.find(boneName) != nodeMap.end()) {
                boneNode = nodeMap.at(boneName);
                found = true;
            }
            if (!found) {
                // This bone doesn't match any node
                spdlog::error("Bone {} doesn't match any node", boneName);
                throw std::runtime_error("Bone doesn't match any node");
            }
            if (!boneNode.has<AnimationController>()) {
                // Create a new AnimationController for this bone
                boneNode.add<AnimationController>();
                boneNode.set<Components::AnimationName>({ boneName });
            }
            jointNodes.push_back(boneNode);
        }

        auto skeleton = std::make_shared<Skeleton>(jointNodes, inverseBindMatrices);

        // Associate animations that reference these bones
        for (unsigned int b = 0; b < aMesh->mNumBones; b++) {
            auto jointNode = jointNodes[b];
            if (!jointNode) continue;
            for (auto& anim : animations) {
                auto jointAnimationName = jointNode.get<Components::AnimationName>();
                if (anim->nodesMap.contains(jointAnimationName.name.c_str())) {
                    skeleton->AddAnimation(anim);
                }
            }
        }

        return skeleton;
    }

    std::vector<std::shared_ptr<Skeleton>> BuildSkeletons(const aiScene* pScene,
        const std::unordered_map<std::string, flecs::entity>& nodeMap,
        const std::vector<std::shared_ptr<Animation>>& animations) {
        std::vector<std::shared_ptr<Skeleton>> skeletons;
        // For each mesh, parse the skeleton
        for (unsigned int i = 0; i < pScene->mNumMeshes; i++) {
            aiMesh* aMesh = pScene->mMeshes[i];
            auto skeleton = parseSkeletonForMesh(aMesh, nodeMap, animations);
            if (skeleton) {
                skeletons.push_back(skeleton);
            }
        }
        return skeletons;
    }

    std::shared_ptr<Scene> LoadModel(std::string filePath) {
        Assimp::Importer importer;
        constexpr unsigned int kAssimpProcessFlags =
            aiProcess_Triangulate |
            aiProcess_FlipUVs |
            aiProcess_OptimizeGraph |
            aiProcess_OptimizeMeshes;
        const aiScene* pScene = importer.ReadFile(filePath, kAssimpProcessFlags);

        if (!pScene || pScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !pScene->mRootNode) {
            spdlog::error("Model loading failed for {}. Error: {}", filePath, importer.GetErrorString());
            return nullptr;
        }

        auto scene = std::make_shared<Scene>();

        // Directory of the model file, allowing both / and \\ as separators
        auto materials = LoadMaterialsFromAssimpScene(pScene, filePath);
        auto [meshes, meshSkinIndices] = parseAiMeshes(pScene, materials, filePath);
        std::vector<flecs::entity> nodes;
        std::unordered_map<std::string, flecs::entity> nodeMap;
        buildAiNodeHierarchy(scene, pScene->mRootNode, pScene, meshes, nodes, nodeMap);

        auto animations = parseAiAnimations(pScene, nodes, nodeMap);
        auto skeletons = BuildSkeletons(pScene, nodeMap, animations);

        for (unsigned int i = 0; i < meshSkinIndices.size(); i++) {
            int skinIndex = meshSkinIndices[i];
            if (skinIndex != -1) {
                meshes[i]->SetBaseSkin(skeletons[skinIndex]);
            }
        }

        scene->ProcessEntitySkins();

        return scene;
    }

}
