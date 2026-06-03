#pragma once

#include <string_view>
// GENERATED CODE - DO NOT EDIT

struct Builtin {
  inline static constexpr std::string_view ActiveDrawSetIndices = "Builtin::ActiveDrawSetIndices";
  inline static constexpr std::string_view BRDFLUT = "Builtin::BRDFLUT";
  inline static constexpr std::string_view Backbuffer = "Builtin::Backbuffer";
  struct CLod {
    inline static constexpr std::string_view GroupChunks = "Builtin::CLod::GroupChunks";
    inline static constexpr std::string_view GroupPageMap = "Builtin::CLod::GroupPageMap";
    inline static constexpr std::string_view Groups = "Builtin::CLod::Groups";
    inline static constexpr std::string_view MeshMetadata = "Builtin::CLod::MeshMetadata";
    inline static constexpr std::string_view Nodes = "Builtin::CLod::Nodes";
    inline static constexpr std::string_view Offsets = "Builtin::CLod::Offsets";
    inline static constexpr std::string_view PagePoolSlabBase = "Builtin::CLod::PagePoolSlabBase";
    inline static constexpr std::string_view PageTable = "Builtin::CLod::PageTable";
    inline static constexpr std::string_view Segments = "Builtin::CLod::Segments";
    inline static constexpr std::string_view StreamingActiveGroupsBits = "Builtin::CLod::StreamingActiveGroupsBits";
    inline static constexpr std::string_view StreamingEvictionExemptBits = "Builtin::CLod::StreamingEvictionExemptBits";
    inline static constexpr std::string_view StreamingLoadCounter = "Builtin::CLod::StreamingLoadCounter";
    inline static constexpr std::string_view StreamingLoadRequestBits = "Builtin::CLod::StreamingLoadRequestBits";
    inline static constexpr std::string_view StreamingLoadRequestKeys = "Builtin::CLod::StreamingLoadRequestKeys";
    inline static constexpr std::string_view StreamingLoadRequests = "Builtin::CLod::StreamingLoadRequests";
    inline static constexpr std::string_view StreamingNonResidentBits = "Builtin::CLod::StreamingNonResidentBits";
    inline static constexpr std::string_view StreamingRuntimeState = "Builtin::CLod::StreamingRuntimeState";
    inline static constexpr std::string_view StreamingTouchedGroups = "Builtin::CLod::StreamingTouchedGroups";
    inline static constexpr std::string_view StreamingTouchedGroupsBits = "Builtin::CLod::StreamingTouchedGroupsBits";
    inline static constexpr std::string_view StreamingTouchedGroupsCounter = "Builtin::CLod::StreamingTouchedGroupsCounter";
    inline static constexpr std::string_view VoxelAttributeSamples = "Builtin::CLod::VoxelAttributeSamples";
    inline static constexpr std::string_view VoxelCubeRecords = "Builtin::CLod::VoxelCubeRecords";
    inline static constexpr std::string_view VoxelDescriptorIndices = "Builtin::CLod::VoxelDescriptorIndices";
    inline static constexpr std::string_view VoxelGroupDescriptors = "Builtin::CLod::VoxelGroupDescriptors";
  };
  inline static constexpr std::string_view CameraBuffer = "Builtin::CameraBuffer";
  struct Color {
    inline static constexpr std::string_view HDRColorTarget = "Builtin::Color::HDRColorTarget";
    inline static constexpr std::string_view SRGBColorTarget = "Builtin::Color::SRGBColorTarget";
  };
  inline static constexpr std::string_view CullingCameraBuffer = "Builtin::CullingCameraBuffer";
  inline static constexpr std::string_view DebugTexture = "Builtin::DebugTexture";
  inline static constexpr std::string_view DebugVisualization = "Builtin::DebugVisualization";
  struct Environment {
    inline static constexpr std::string_view CurrentCubemap = "Builtin::Environment::CurrentCubemap";
    inline static constexpr std::string_view CurrentPrefilteredCubemap = "Builtin::Environment::CurrentPrefilteredCubemap";
    inline static constexpr std::string_view InfoBuffer = "Builtin::Environment::InfoBuffer";
    inline static constexpr std::string_view PrefilteredCubemapsGroup = "Builtin::Environment::PrefilteredCubemapsGroup";
    inline static constexpr std::string_view WorkingCubemapGroup = "Builtin::Environment::WorkingCubemapGroup";
    inline static constexpr std::string_view WorkingHDRIGroup = "Builtin::Environment::WorkingHDRIGroup";
  };
  struct GBuffer {
    inline static constexpr std::string_view Albedo = "Builtin::GBuffer::Albedo";
    inline static constexpr std::string_view Coat = "Builtin::GBuffer::Coat";
    inline static constexpr std::string_view Emissive = "Builtin::GBuffer::Emissive";
    inline static constexpr std::string_view Fuzz = "Builtin::GBuffer::Fuzz";
    inline static constexpr std::string_view MetallicRoughness = "Builtin::GBuffer::MetallicRoughness";
    inline static constexpr std::string_view MotionVectors = "Builtin::GBuffer::MotionVectors";
    inline static constexpr std::string_view Normals = "Builtin::GBuffer::Normals";
  };
  struct GTAO {
    inline static constexpr std::string_view OutputAOTerm = "Builtin::GTAO::OutputAOTerm";
    inline static constexpr std::string_view WorkingAOTerm1 = "Builtin::GTAO::WorkingAOTerm1";
    inline static constexpr std::string_view WorkingAOTerm2 = "Builtin::GTAO::WorkingAOTerm2";
    inline static constexpr std::string_view WorkingDepths = "Builtin::GTAO::WorkingDepths";
    inline static constexpr std::string_view WorkingEdges = "Builtin::GTAO::WorkingEdges";
  };
  struct IndirectCommandBuffers {
    inline static constexpr std::string_view Master = "Builtin::IndirectCommandBuffers::Master";
    inline static constexpr std::string_view Primary = "Builtin::IndirectCommandBuffers::Primary";
  };
  inline static constexpr std::string_view InstanceDrawRecordBuffer = "Builtin::InstanceDrawRecordBuffer";
  inline static constexpr std::string_view LastFrameLinearDepthMaps = "Builtin::LastFrameLinearDepthMaps";
  struct Light {
    inline static constexpr std::string_view ActiveLightIndices = "Builtin::Light::ActiveLightIndices";
    inline static constexpr std::string_view BufferGroup = "Builtin::Light::BufferGroup";
    inline static constexpr std::string_view ClusterBuffer = "Builtin::Light::ClusterBuffer";
    inline static constexpr std::string_view DirectionalLightCascadeBuffer = "Builtin::Light::DirectionalLightCascadeBuffer";
    inline static constexpr std::string_view InfoBuffer = "Builtin::Light::InfoBuffer";
    inline static constexpr std::string_view PagesBuffer = "Builtin::Light::PagesBuffer";
    inline static constexpr std::string_view PagesCounter = "Builtin::Light::PagesCounter";
    inline static constexpr std::string_view PointLightCubemapBuffer = "Builtin::Light::PointLightCubemapBuffer";
    inline static constexpr std::string_view SpotLightMatrixBuffer = "Builtin::Light::SpotLightMatrixBuffer";
    inline static constexpr std::string_view ViewResourceGroup = "Builtin::Light::ViewResourceGroup";
  };
  struct Material {
    inline static constexpr std::string_view TextureGroup = "Builtin::Material::TextureGroup";
    inline static constexpr std::string_view TextureStreamingFeedbackBuffer = "Builtin::Material::TextureStreamingFeedbackBuffer";
    inline static constexpr std::string_view TextureStreamingMetadataBuffer = "Builtin::Material::TextureStreamingMetadataBuffer";
  };
  struct Terrain {
    inline static constexpr std::string_view LayerRefs = "Builtin::Terrain::LayerRefs";
    inline static constexpr std::string_view Layers = "Builtin::Terrain::Layers";
    inline static constexpr std::string_view Regions = "Builtin::Terrain::Regions";
    inline static constexpr std::string_view Sets = "Builtin::Terrain::Sets";
    inline static constexpr std::string_view TextureGroup = "Builtin::Terrain::TextureGroup";
    inline static constexpr std::string_view WeightBlocks = "Builtin::Terrain::WeightBlocks";
  };
  struct Noise {
    inline static constexpr std::string_view BlueNoise2D = "Builtin::Noise::BlueNoise2D";
  };
  inline static constexpr std::string_view NormalMatrixBuffer = "Builtin::NormalMatrixBuffer";
  struct OpenPBR {
    inline static constexpr std::string_view FuzzLTC = "Builtin::OpenPBR::FuzzLTC";
    inline static constexpr std::string_view IdealDielectricAverageEnergyComplement = "Builtin::OpenPBR::IdealDielectricAverageEnergyComplement";
    inline static constexpr std::string_view IdealDielectricEnergyComplement = "Builtin::OpenPBR::IdealDielectricEnergyComplement";
    inline static constexpr std::string_view IdealDielectricReflectionRatio = "Builtin::OpenPBR::IdealDielectricReflectionRatio";
    inline static constexpr std::string_view IdealMetalAverageEnergyComplement = "Builtin::OpenPBR::IdealMetalAverageEnergyComplement";
    inline static constexpr std::string_view IdealMetalEnergyComplement = "Builtin::OpenPBR::IdealMetalEnergyComplement";
    inline static constexpr std::string_view OpaqueDielectricAverageEnergyComplement = "Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement";
    inline static constexpr std::string_view OpaqueDielectricEnergyComplement = "Builtin::OpenPBR::OpaqueDielectricEnergyComplement";
  };
  struct PPLL {
    inline static constexpr std::string_view Counter = "Builtin::PPLL::Counter";
    inline static constexpr std::string_view DataBuffer = "Builtin::PPLL::DataBuffer";
    inline static constexpr std::string_view HeadPointerTexture = "Builtin::PPLL::HeadPointerTexture";
  };
  inline static constexpr std::string_view PerFrameBuffer = "Builtin::PerFrameBuffer";
  inline static constexpr std::string_view PerInstanceTransformBuffer = "Builtin::PerInstanceTransformBuffer";
  inline static constexpr std::string_view PerMaterialDataBuffer = "Builtin::PerMaterialDataBuffer";
  inline static constexpr std::string_view PerMaterialOpenPBRDataBuffer = "Builtin::PerMaterialOpenPBRDataBuffer";
  inline static constexpr std::string_view PerMeshBuffer = "Builtin::PerMeshBuffer";
  inline static constexpr std::string_view PerMeshInstanceBuffer = "Builtin::PerMeshInstanceBuffer";
  inline static constexpr std::string_view PerObjectBuffer = "Builtin::PerObjectBuffer";
  struct PostProcessing {
    inline static constexpr std::string_view AdaptedLuminance = "Builtin::PostProcessing::AdaptedLuminance";
    inline static constexpr std::string_view LuminanceHistogram = "Builtin::PostProcessing::LuminanceHistogram";
    inline static constexpr std::string_view ScreenSpaceReflections = "Builtin::PostProcessing::ScreenSpaceReflections";
    inline static constexpr std::string_view UpscaledHDR = "Builtin::PostProcessing::UpscaledHDR";
  };
  struct PrimaryCamera {
    inline static constexpr std::string_view DepthTexture = "Builtin::PrimaryCamera::DepthTexture";
    struct IndirectCommandBuffers {
      inline static constexpr std::string_view Primary = "Builtin::PrimaryCamera::IndirectCommandBuffers::Primary";
    };
    inline static constexpr std::string_view LinearDepthMap = "Builtin::PrimaryCamera::LinearDepthMap";
    inline static constexpr std::string_view ProjectedDepthTexture = "Builtin::PrimaryCamera::ProjectedDepthTexture";
    inline static constexpr std::string_view VisibilityTexture = "Builtin::PrimaryCamera::VisibilityTexture";
  };
  struct Shadows {
    inline static constexpr std::string_view CLodClipmapInfo = "Builtin::Shadows::CLodClipmapInfo";
    inline static constexpr std::string_view CLodCompactMainCamera = "Builtin::Shadows::CLodCompactMainCamera";
    inline static constexpr std::string_view CLodCompactShadowCameras = "Builtin::Shadows::CLodCompactShadowCameras";
    inline static constexpr std::string_view CLodDirectionalPageViewInfo = "Builtin::Shadows::CLodDirectionalPageViewInfo";
    inline static constexpr std::string_view CLodPageTable = "Builtin::Shadows::CLodPageTable";
    inline static constexpr std::string_view CLodPhysicalPages = "Builtin::Shadows::CLodPhysicalPages";
  };
  struct SkeletonResources {
    inline static constexpr std::string_view BoneTransforms = "Builtin::SkeletonResources::BoneTransforms";
    inline static constexpr std::string_view InverseBindMatrices = "Builtin::SkeletonResources::InverseBindMatrices";
    inline static constexpr std::string_view InverseSkinMatrices = "Builtin::SkeletonResources::InverseSkinMatrices";
    inline static constexpr std::string_view SkinningInstanceInfo = "Builtin::SkeletonResources::SkinningInstanceInfo";
  };
};

