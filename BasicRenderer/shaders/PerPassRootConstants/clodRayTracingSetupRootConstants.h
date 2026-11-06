#ifndef CLOD_RAY_TRACING_SETUP_ROOT_CONSTANTS_H
#define CLOD_RAY_TRACING_SETUP_ROOT_CONSTANTS_H

// ---- Scene build (rayTracingSceneBuild.hlsl)
#define CLOD_RT_CLEAR_UAV UintRootConstant0
#define CLOD_RT_CLEAR_WORD_COUNT UintRootConstant1

// CLAS build inputs
#define CLOD_RT_BUILD_SOURCES_SRV UintRootConstant0
#define CLOD_RT_BUILD_SOURCE_COUNT UintRootConstant1
#define CLOD_RT_BUILD_ARGS_UAV UintRootConstant2
#define CLOD_RT_BUILD_DESTINATIONS_UAV UintRootConstant3
#define CLOD_RT_BUILD_CLAS_INFO_UAV UintRootConstant4
#define CLOD_RT_BUILD_CLAS_ADDRESSES_UAV UintRootConstant5
#define CLOD_RT_BUILD_FIRST_ARG UintRootConstant6 // Where this batch's arguments start.
#define CLOD_RT_BUILD_PUBLISH UintRootConstant7 // Nonzero when this batch really builds, rather than measuring sizes.
#define CLOD_RT_BUILD_SOURCE_FIRST UintRootConstant8 // Where this frame's slice starts in the upload buffer.

// Grouping selected clusters by instance
#define CLOD_RT_SCENE_CLUSTERS_SRV UintRootConstant0
#define CLOD_RT_SCENE_CLUSTER_COUNTER_SRV UintRootConstant1
#define CLOD_RT_SCENE_CLUSTER_CAPACITY UintRootConstant2
#define CLOD_RT_SCENE_DRAW_TABLE_UAV UintRootConstant3
#define CLOD_RT_SCENE_DRAW_TABLE_ENTRIES UintRootConstant4
#define CLOD_RT_SCENE_INSTANCE_LIST_UAV UintRootConstant5
#define CLOD_RT_SCENE_COUNTERS_UAV UintRootConstant6
#define CLOD_RT_SCENE_INSTANCE_CAPACITY UintRootConstant7
#define CLOD_RT_SCENE_BLAS_ARGS_UAV UintRootConstant8
#define CLOD_RT_SCENE_ADDRESS_ARRAY_UAV UintRootConstant9
#define CLOD_RT_SCENE_ADDRESS_ARRAY_LOW UintRootConstant10
#define CLOD_RT_SCENE_ADDRESS_ARRAY_HIGH UintRootConstant11
#define CLOD_RT_SCENE_MAX_CLUSTERS_PER_INSTANCE UintRootConstant12
#define CLOD_RT_SCENE_GROUP_TABLE_SRV UintRootConstant13
#define CLOD_RT_SCENE_SEGMENT_TABLE_SRV UintRootConstant14
#define CLOD_RT_SCENE_CLAS_ADDRESSES_UAV UintRootConstant15
#define CLOD_RT_SCENE_BLAS_ADDRESSES_SRV UintRootConstant18
#define CLOD_RT_SCENE_TLAS_INSTANCES_UAV UintRootConstant19
#define CLOD_RT_SCENE_TLAS_INSTANCE_COUNT UintRootConstant20
#define CLOD_RT_SCENE_INSTANCE_BLAS_UAV UintRootConstant21
#define CLOD_RT_SCENE_SHARE_TABLE_UAV UintRootConstant22
#define CLOD_RT_SCENE_SHARE_TABLE_ENTRIES UintRootConstant23 // A power of two.
#define CLOD_RT_SCENE_REFERENCE_CAPACITY UintRootConstant24
#define CLOD_RT_SCENE_BLAS_CAPACITY UintRootConstant25
#define CLOD_RT_SCENE_COARSE_CUT_UAV UintRootConstant26
#define CLOD_RT_SCENE_SLOT_OMM_KEYS_UAV UintRootConstant27 // Per CLAS slot: the material identity its micromaps were baked for, or zero.
#define CLOD_RT_SCENE_BLAS_OMM_KEYS_UAV UintRootConstant28 // Per BLAS: the same, or CLOD_RT_OMM_KEY_CONFLICT.
#define CLOD_RT_OMM_KEY_CONFLICT 0x80000000u
#define CLOD_RT_SCENE_OMM_ACTIVE UintRootConstant29 // Zero disables every instance's micromaps.

// CLAS build inputs: opacity micromaps
#define CLOD_RT_BUILD_SLOT_OMM_KEYS_UAV UintRootConstant9
#define CLOD_RT_BUILD_OMM_INDEX_POOL_LOW UintRootConstant10
#define CLOD_RT_BUILD_OMM_INDEX_POOL_HIGH UintRootConstant11
#define CLOD_RT_OMM_INDEX_UNIT_BYTES 512u // One cluster's indices: MS_MESHLET_SIZE triangles at 4 bytes.

// The coarse cut buffer culling fills, see CLOD_WG_RT_COARSE_CUT_DESCRIPTOR_INDEX.
#define CLOD_RT_COARSE_CONSUMER_CAPACITY (1u << 18)
#define CLOD_RT_COARSE_MESH_ENTRIES (1u << 18)
#define CLOD_RT_COARSE_MESH_TABLE_BYTES (16u + CLOD_RT_COARSE_CONSUMER_CAPACITY * 8u)
#define CLOD_RT_COARSE_BUFFER_BYTES (CLOD_RT_COARSE_MESH_TABLE_BYTES + CLOD_RT_COARSE_MESH_ENTRIES * 4u)

// Word offsets into the counters buffer.
#define CLOD_RT_COUNTER_INSTANCES 0u
#define CLOD_RT_COUNTER_CLUSTERS 1u // Clusters placed in BLASes this frame.
#define CLOD_RT_COUNTER_BLAS_ARGS 2u // What ExecuteIndirect reads as the argument count: distinct BLASes.
#define CLOD_RT_COUNTER_TLAS_INSTANCES 5u
#define CLOD_RT_COUNTER_LIST_CLUSTERS 6u // The size of the list culling produced, before any sharing.
#define CLOD_RT_COUNTER_DROPPED_BLASES 7u
#define CLOD_RT_COUNTER_DROPPED_INSTANCES 3u
#define CLOD_RT_COUNTER_DROPPED_CLUSTERS 4u
#define CLOD_RT_COUNTER_COARSE_CONSUMERS 8u // Instances that took a BLAS without being traversed.
#define CLOD_RT_COUNTER_ALPHA_INSTANCES 9u // Alpha-tested instances in the TLAS.
#define CLOD_RT_COUNTER_OMM_INSTANCES 10u // Of those, how many trace with micromaps.
#define CLOD_RT_COUNTER_OMM_DISABLED_INSTANCES 11u // Instances whose BLAS has micromaps they can't use.
#define CLOD_RT_COUNTER_WORDS 12u

// In the instance's BLAS entry: not an index but where to find one.
#define CLOD_RT_INSTANCE_BLAS_SHARED 0x80000000u // The rest is a share table slot.
#define CLOD_RT_INSTANCE_BLAS_VIA_ROW 0x40000000u // The rest is another instance row.
#define CLOD_RT_INSTANCE_BLAS_NONE 0xFFFFFFFFu

// ---- Primary visibility (rayTracedVisibility.hlsl)
#define CLOD_RT_VIS_TLAS_SRV UintRootConstant0
#define CLOD_RT_VIS_CLAS_INFO_SRV UintRootConstant1
#define CLOD_RT_VIS_VISIBILITY_UAV UintRootConstant2
#define CLOD_RT_VIS_VISIBLE_CLUSTERS_UAV UintRootConstant3
#define CLOD_RT_VIS_TRANSFORM_INDICES_UAV UintRootConstant4
#define CLOD_RT_VIS_HASH_UAV UintRootConstant5
#define CLOD_RT_VIS_HASH_ENTRIES UintRootConstant6
#define CLOD_RT_VIS_RECORD_BASE UintRootConstant7
#define CLOD_RT_VIS_MODE UintRootConstant8
#define CLOD_RT_VIS_STATS_UAV UintRootConstant9
#define CLOD_RT_VIS_VIEW_ID UintRootConstant10
#define CLOD_RT_VIS_CAPACITY UintRootConstant11

#define CLOD_RT_VIS_MODE_WRITE 0u   // Merge hits into the visibility buffer.
#define CLOD_RT_VIS_MODE_COMPARE 1u // Leave the frame alone and count how raster's keys agree with the rays.

// Word offsets into the comparison statistics.
#define CLOD_RT_STAT_BOTH_HIT_MATCH 0u
#define CLOD_RT_STAT_DIFFERENT_TRIANGLE 1u
#define CLOD_RT_STAT_DIFFERENT_CLUSTER 2u
#define CLOD_RT_STAT_DIFFERENT_INSTANCE 3u
#define CLOD_RT_STAT_RAY_MISSED_RASTER_HIT 4u
#define CLOD_RT_STAT_RAY_HIT_RASTER_MISSED 5u
#define CLOD_RT_STAT_BOTH_MISSED 6u
#define CLOD_RT_STAT_DEPTH_WITHIN_TOLERANCE 7u
#define CLOD_RT_STAT_RAY_HIT_FARTHER 8u
#define CLOD_RT_STAT_RAY_HIT_NEARER 9u
#define CLOD_RT_STAT_RASTER_NOT_COVERED 10u // Raster pixel from a draw the BVH leaves out.
#define CLOD_RT_STAT_DIFFERENT_INSTANCE_RASTER_ALPHA 11u
#define CLOD_RT_STAT_DIFFERENT_INSTANCE_RASTER_DOUBLE_SIDED 12u
#define CLOD_RT_STAT_WORDS 16u

// ---- Secondary rays (rayTracedReflections.hlsl, rayTracedShadows.hlsl)
#define CLOD_RT_SEC_TLAS_SRV UintRootConstant0
#define CLOD_RT_SEC_CLAS_INFO_SRV UintRootConstant1
#define CLOD_RT_SEC_OUTPUT_UAV UintRootConstant2
#define CLOD_RT_SEC_HITS_UAV UintRootConstant3
#define CLOD_RT_SEC_FRAME UintRootConstant4
#define CLOD_RT_SEC_MAX_DISTANCE UintRootConstant5
#define CLOD_RT_SEC_ROUGHNESS_CUTOFF UintRootConstant6
#define CLOD_RT_SEC_SUN_CONE_TANGENT UintRootConstant7
#define CLOD_RT_SEC_SUN_DIRECTION_X UintRootConstant8
#define CLOD_RT_SEC_SUN_DIRECTION_Y UintRootConstant9
#define CLOD_RT_SEC_SUN_DIRECTION_Z UintRootConstant10
#define CLOD_RT_SEC_HISTORY_SRV UintRootConstant11
#define CLOD_RT_SEC_INPUT_SRV UintRootConstant12

// Path tracing shares the secondary rays' TLAS, CLAS info, output and frame slots. Above 21 are the lighting toggles in cbuffers.hlsli.
#define CLOD_RT_PT_NO_DESCRIPTOR 0xFFFFFFFFu
#define CLOD_RT_PT_MAX_BOUNCES UintRootConstant3
#define CLOD_RT_PT_FLAGS UintRootConstant5
#define CLOD_RT_PT_ACCUMULATION_UAV UintRootConstant6
#define CLOD_RT_PT_BASE_COLOR_UAV UintRootConstant7
#define CLOD_RT_PT_NORMAL_ROUGHNESS_UAV UintRootConstant8
#define CLOD_RT_PT_MOTION_UAV UintRootConstant9
#define CLOD_RT_PT_PROJECTED_DEPTH_UAV UintRootConstant10
#define CLOD_RT_PT_SPECULAR_ALBEDO_UAV UintRootConstant11
#define CLOD_RT_PT_SPECULAR_HIT_UAV UintRootConstant12
#define CLOD_RT_PT_FLAG_ACCUMULATE 1u
#define CLOD_RT_PT_FLAG_REORDER_PRIMARY 2u
#define CLOD_RT_PT_FLAG_REORDER_SECONDARY 4u
#define CLOD_RT_PT_FLAG_SUN_ONLY 8u // No sky, emission or local lights, for comparing against raster's direct sun.
#define CLOD_RT_PT_FLAG_FIREFLY_CLAMP 16u
#define CLOD_RT_PT_FLAG_ROULETTE 32u
#define CLOD_RT_PT_FLAG_RESTART_ACCUMULATION 64u // What rays see changed this frame.
#define CLOD_RT_PT_FLAG_OMM_2_STATE_SECONDARY 128u // Bounce and shadow rays take unknown micro-triangles' baked guess instead of running any-hit.
#define CLOD_RT_PT_FLAG_OMM_2_STATE_PRIMARY 256u

#endif // CLOD_RAY_TRACING_SETUP_ROOT_CONSTANTS_H
