#include "include/vertex.hlsli"
#include "include/utilities.hlsli"
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/materialflags.hlsli"
#include "include/lighting.hlsli"
#include "include/gammaCorrection.hlsli"
#include "include/outputTypes.hlsli"
#include "include/materialFlags.hlsli"
#include "include/debugPayload.hlsli"
#include "fullscreenVS.hlsli"

PSInput VSMain(uint vertexID : SV_VertexID) {
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    ByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PostSkinningVertices)];
    
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    PerMeshBuffer meshBuffer = perMeshBuffer[GetRootPerMeshBufferIndex()];
    StructuredBuffer<PerMeshInstanceBuffer> perMeshInstanceBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshInstanceBuffer)];
    PerMeshInstanceBuffer meshInstanceBuffer = perMeshInstanceBuffer[GetRootPerMeshInstanceBufferIndex()];
    
    StructuredBuffer<PerObjectBuffer> perObjectBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    PerObjectBuffer objectBuffer = perObjectBuffer[GetRootPerObjectBufferIndex()];
            
    uint vertexFlags = meshBuffer.vertexFlags;
    
    uint postSkinningBufferOffset = meshInstanceBuffer.postSkinningVertexBufferOffset;
    
    uint prevPostSkinningBufferOffset = postSkinningBufferOffset;
    if (meshBuffer.vertexFlags & VERTEX_SKINNED)
    {
        postSkinningBufferOffset += meshBuffer.vertexByteSize * meshBuffer.numVertices * (perFrameBuffer.frameIndex % 2);
        prevPostSkinningBufferOffset += meshBuffer.vertexByteSize * meshBuffer.numVertices * ((perFrameBuffer.frameIndex + 1) % 2);
    }
    
    uint byteOffset = postSkinningBufferOffset + vertexID * meshBuffer.vertexByteSize;
    Vertex input = LoadVertex(byteOffset, vertexBuffer, meshBuffer.vertexFlags);
    
    float4 pos = float4(input.position.xyz, 1.0f);
    
    float4 prevPos;
    if (vertexFlags & VERTEX_SKINNED)
    {
        uint prevByteOffset = prevPostSkinningBufferOffset + vertexID * meshBuffer.vertexByteSize;
        prevPos = float4(LoadFloat3(prevByteOffset, vertexBuffer), 1.0);
    }
    else
    {
        prevPos = float4(input.position.xyz, 1.0f);
    }
    
    PSInput output;
    float4 worldPosition = mul(pos, objectBuffer.model);

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[meshBuffer.materialDataIndex];
    uint materialFlags = materialInfo.materialFlags;
    
    if (materialFlags & MATERIAL_TEXTURED) {
        output.texcoord = input.texcoord;
    }
    
#if defined(PSO_SHADOW)
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    LightInfo light = lights[GetRootCurrentLightID()];
    matrix lightMatrix;
    matrix viewMatrix;
    switch(light.type) {
        case 0: { // Point light
            StructuredBuffer<unsigned int> pointLightCubemapIndicesBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::PointLightCubemapBuffer)];
            uint lightCameraIndex = pointLightCubemapIndicesBuffer[GetRootLightViewIndex()];
            Camera lightCamera = cameras[lightCameraIndex];
            lightMatrix = lightCamera.viewProjection;
            viewMatrix = lightCamera.view;
            break;
        }
        case 1: { // Spot light
            StructuredBuffer<unsigned int> spotLightMatrixIndexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::SpotLightMatrixBuffer)];
            uint lightCameraIndex = spotLightMatrixIndexBuffer[GetRootLightViewIndex()];
            Camera lightCamera = cameras[lightCameraIndex];
            lightMatrix = lightCamera.viewProjection;
            viewMatrix = lightCamera.view;
            break;
        }
        case 2: { // Directional light
            StructuredBuffer<unsigned int> directionalLightCascadeIndicesBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::DirectionalLightCascadeBuffer)];
            uint lightCameraIndex = directionalLightCascadeIndicesBuffer[GetRootLightViewIndex()];
            Camera lightCamera = cameras[lightCameraIndex];
            lightMatrix = lightCamera.viewProjection;
            viewMatrix = lightCamera.view;
            break;
        }
    }
    output.position = mul(worldPosition, lightMatrix);
    output.positionViewSpace = mul(worldPosition, viewMatrix);
    return output;
#endif // SHADOW
    
    Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];
    
    output.positionWorldSpace = worldPosition;
    float4 viewPosition = mul(worldPosition, mainCamera.view);
    output.positionViewSpace = viewPosition;
    output.position = mul(viewPosition, mainCamera.projection);
    output.clipPosition = mul(viewPosition, mainCamera.unjitteredProjection);
        
    float4 prevPosition = mul(prevPos, objectBuffer.prevModel);
    prevPosition = mul(prevPosition, mainCamera.prevView);
    output.prevClipPosition = mul(prevPosition, mainCamera.prevUnjitteredProjection);
    
    StructuredBuffer<SingleMatrix> normalMatrixBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::NormalMatrixBuffer)];
    float3x3 normalMatrix = (float3x3) normalMatrixBuffer[objectBuffer.normalMatrixBufferIndex].value;
    output.normalWorldSpace = normalize(mul(input.normal, normalMatrix));
    output.tangentWorldSpace = (vertexFlags & VERTEX_TANGENTS)
        ? float4(normalize(mul(input.tangent.xyz, normalMatrix)), input.tangent.w)
        : float4(1.0f, 0.0f, 0.0f, 0.0f);

    output.color = input.color;

    output.meshletIndex = 0; // Unused for vertex shader
    
    output.normalModelSpace = input.normal;
    
    return output;
}

struct PrePassPSOutput
{
    float4 normal;
    float2 motionVector;
    float linearDepth;
    float4 albedo;
    float4 coat;
    float4 metallicRoughness;
    float4 emissive;
    float4 fuzz;
};

PrePassPSOutput PrepassPSMain(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    
    MaterialInputs fragmentInfo;
    GetMaterialInfoForFragment(input, fragmentInfo);
    
#if defined(PSO_DOUBLE_SIDED)
    if (!isFrontFace) {
        fragmentInfo.normalWS = -fragmentInfo.normalWS;
    }
#endif    

    //float3 outNorm = SignedOctEncode(fragmentInfo.normalWS);
    
    PrePassPSOutput output;
    output.normal = float4(fragmentInfo.normalWS, (float)fragmentInfo.openPBRMaterialDataIndex);
    output.linearDepth = -input.positionViewSpace.z;
    
    // Motion vector
    float3 NDCPos = (input.clipPosition / input.clipPosition.w).xyz;
    float3 PrevNDCPos = (input.prevClipPosition / input.prevClipPosition.w).xyz;
    output.motionVector = (NDCPos - PrevNDCPos).xy;
    
#if defined(PSO_DEFERRED)
    output.albedo = float4(fragmentInfo.albedo.xyz, fragmentInfo.ambientOcclusion);
    output.coat = float4(fragmentInfo.coatColor, fragmentInfo.coatWeight);
    output.metallicRoughness = float4(fragmentInfo.metallic, fragmentInfo.roughness, fragmentInfo.coatRoughness, fragmentInfo.fuzzWeight);
    output.emissive = float4(fragmentInfo.emissive.xyz, 0.0);
    output.fuzz = float4(fragmentInfo.fuzzColor, fragmentInfo.fuzzRoughness);
#endif
    return output;
}

#if defined(PSO_SHADOW)
float
#else
[earlydepthstencil]
float4 
#endif
PSMain(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{

    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    uint meshBufferIndex = GetRootPerMeshBufferIndex();
    PerMeshBuffer meshBuffer = perMeshBuffer[meshBufferIndex];
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[meshBuffer.materialDataIndex];
    uint materialFlags = materialInfo.materialFlags;
#if defined(PSO_SHADOW)
#if !defined(PSO_ALPHA_TEST) && !defined(PSO_BLEND)
        return -input.positionViewSpace.z;
#endif // DOUBLE_SIDED
    if (materialFlags & MATERIAL_BASE_COLOR_TEXTURE && !(materialFlags & MATERIAL_OPACITY_TEXTURE)) { // Opacity texture overrides base color alpha for shadow
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[materialInfo.baseColorTextureIndex];
        SamplerState baseColorSamplerState = SamplerDescriptorHeap[materialInfo.baseColorSamplerIndex];
        float2 uv = input.texcoord;
        float4 baseColor = baseColorTexture.Sample(baseColorSamplerState, uv);
        if (baseColor.a*materialInfo.baseColorFactor.a < 0.5){
            discard;
        }
    }
    if (materialFlags & MATERIAL_OPACITY_TEXTURE)
    { 
        Texture2D<float4> opacityTexture = ResourceDescriptorHeap[materialInfo.opacityTextureIndex];
        SamplerState opacitySamplerState = SamplerDescriptorHeap[materialInfo.opacitySamplerIndex];
        float2 uv = input.texcoord;
        float4 opacitySample = opacityTexture.Sample(opacitySamplerState, uv);
        float opacity = opacitySample.a;
        if (opacity < materialInfo.alphaCutoff) {
            discard;
        }
    }
    if (materialInfo.baseColorFactor.a < 0.5){
        discard;
    }
    return -input.positionViewSpace.z; // Shadow outputs linear depth
#endif // PSO_SHADOW
#if !defined(PSO_SHADOW)

    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];
    float3 viewDir = normalize(mainCamera.positionWorldSpace.xyz - input.positionWorldSpace.xyz);
    
    FragmentInfo fragmentInfo;
    GetFragmentInfoDirect(input, viewDir, GetRootEnableGTAO(), false, isFrontFace, fragmentInfo);

    LightingOutput lightingOutput = lightFragment(fragmentInfo, mainCamera, perFrameBuffer.activeEnvironmentIndex, ResourceDescriptorIndex(Builtin::Environment::InfoBuffer), isFrontFace);
    
    float3 lighting = lightingOutput.lighting;
    
    // Write debug payload when not in COLOR mode
    if (perFrameBuffer.outputType != OUTPUT_COLOR) {
        RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
        uint2 pixel = uint2(input.position.xy);
        uint2 payload = uint2(DEBUG_SENTINEL, DEBUG_SENTINEL);
        switch (perFrameBuffer.outputType) {
            case OUTPUT_NORMAL:
                payload = PackDebugFloat3(fragmentInfo.normalWS * 0.5 + 0.5);
                break;
            case OUTPUT_ALBEDO:
            case OUTPUT_TERRAIN_GRASS_OVERLAY:
                payload = PackDebugFloat3(fragmentInfo.albedo.rgb);
                break;
            case OUTPUT_METALLIC:
                payload = PackDebugFloat3(fragmentInfo.metallic.xxx);
                break;
            case OUTPUT_ROUGHNESS:
                payload = PackDebugFloat3(fragmentInfo.roughness.xxx);
                break;
            case OUTPUT_EMISSIVE:
                payload = PackDebugFloat3(fragmentInfo.emissive.rgb);
                break;
            case OUTPUT_AO:
                payload = PackDebugFloat3(fragmentInfo.diffuseAmbientOcclusion.xxx);
                break;
            case OUTPUT_TERRAIN_GEOMETRIC_HEIGHT:
                payload = PackDebugFloat3(fragmentInfo.geometricHeightDebug.xxx);
                break;
            case OUTPUT_DEPTH: {
                float depth = abs(input.positionViewSpace.z) * 0.1;
                payload = PackDebugFloat3(depth.xxx);
                break;
            }
#if defined(PSO_IMAGE_BASED_LIGHTING)
            case OUTPUT_DIFFUSE_IBL:
                payload = PackDebugFloat3(lightingOutput.diffuseIBL.rgb);
                break;
            case OUTPUT_SPECULAR_IBL:
                payload = PackDebugFloat3(lightingOutput.specularIBL.rgb);
                break;
#endif
#if defined(PSO_CLUSTERED_LIGHTING)
            case OUTPUT_LIGHT_CLUSTER_ID:
                payload = PackDebugUint(lightingOutput.clusterIndex);
                break;
            case OUTPUT_LIGHT_CLUSTER_LIGHT_COUNT:
                payload = PackDebugUint(lightingOutput.clusterLightCount);
                break;
#endif
            case OUTPUT_MESHLETS:
                payload = PackDebugUint(input.meshletIndex);
                break;
            case OUTPUT_MODEL_NORMALS:
                payload = PackDebugFloat3(input.normalModelSpace * 0.5 + 0.5);
                break;
            case OUTPUT_MOTION_VECTORS: {
                float3 ndc = (input.clipPosition / input.clipPosition.w).xyz;
                float3 prevNdc = (input.prevClipPosition / input.prevClipPosition.w).xyz;
                float2 mv = (ndc - prevNdc).xy;
                payload = PackDebugFloat3(float3(mv * 0.5 + 0.5, 0.5));
                break;
            }
            case OUTPUT_MATERIAL_SELECTED_MIP:
                if (fragmentInfo.selectedMaterialMipLevel != MATERIAL_DEBUG_INVALID_MIP_LEVEL) {
                    payload = PackDebugUint2(fragmentInfo.selectedMaterialMipLevel, fragmentInfo.selectedMaterialMipMaxLevel);
                }
                break;
            case OUTPUT_PARALLAX_PIXELS:
                payload = PackDebugFloat3(fragmentInfo.parallaxApplied != 0u ? 1.0f.xxx : 0.0f.xxx);
                break;
        }
        if (payload.x != DEBUG_SENTINEL) {
            WriteDebugPixel(debugVisTex, pixel, payload);
        }
    }

    return float4(lighting, 1.0);
#endif // PSO_SHADOW
}
