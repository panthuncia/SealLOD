#ifndef CLOD_RAY_TRACING_SETUP_ROOT_CONSTANTS_H
#define CLOD_RAY_TRACING_SETUP_ROOT_CONSTANTS_H

// ---- Scene build (rayTracingSceneBuild.hlsl)
#define CLOD_RT_CLEAR_UAV UintRootConstant0
#define CLOD_RT_CLEAR_WORD_COUNT UintRootConstant1

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
#define CLOD_RT_SCENE_GROUP_STATE_SRV UintRootConstant13 // Raw: the cache's per-group state (clodRayTracingCache.hlsli).
#define CLOD_RT_SCENE_GROUP_STATE_UAV UintRootConstant34 // The same, for Count's frame marks.
#define CLOD_RT_SCENE_DEMAND_UAV UintRootConstant35 // Raw: the demand bitset Count fills (CLOD_RT_DEMAND_*).
#define CLOD_RT_SCENE_CACHE_ENTRIES_SRV UintRootConstant14 // Raw: the cache's entries.
#define CLOD_RT_SCENE_CACHE_TRANSIENT_SRV UintRootConstant32 // uint2 per group: this frame's transient CLAS IDs.
#define CLOD_RT_SCENE_CACHE_CTRL_UAV UintRootConstant31 // Raw: the cache's control words: the frame, and what placing counts.
#define CLOD_RT_SCENE_CLAS_ADDRESSES_UAV UintRootConstant15
// Part portals (docs/merge_hierarchies.md): a cluster reached through a portal belongs to that occurrence's instance, not
// its draw's. The side band names the occurrence's assembly transform; its instance key is DRAW_KEYS + transform.
#define CLOD_RT_SCENE_CLUSTER_SIDE_BAND_SRV UintRootConstant16
#define CLOD_RT_SCENE_INSTANCE_ROWS_UAV UintRootConstant17 // Per instance row: uint4(draw, assembly transform, a cluster's group, 0).
#define CLOD_RT_SCENE_DRAW_KEYS UintRootConstant30 // Instance keys below this are draws; above, DRAW_KEYS + assembly transform.
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

// The CLAS cache passes (CLodRtCache*CS). See clodRayTracingCache.hlsli.
#define CLOD_RT_CACHE_STATE_UAV UintRootConstant0
#define CLOD_RT_CACHE_CTRL_UAV UintRootConstant1
#define CLOD_RT_CACHE_ENTRIES_UAV UintRootConstant2 // Raw, CLOD_RT_CACHE_ENTRY_BYTES each.
#define CLOD_RT_CACHE_PAGE_NEXT_UAV UintRootConstant3 // Per page: the next page of its group.
#define CLOD_RT_CACHE_QUEUES_UAV UintRootConstant4 // Raw: the free page, chunk and entry queues, in that order.
#define CLOD_RT_CACHE_REQUESTS_UAV UintRootConstant5 // uint2 [group, material identity]: groups the list needs, ascending.
#define CLOD_RT_CACHE_DEMAND_UAV UintRootConstant6 // Raw: the demand bitset (CLOD_RT_DEMAND_*).
#define CLOD_RT_CACHE_REQUEST_CAPACITY UintRootConstant7
#define CLOD_RT_CACHE_RANGES_UAV UintRootConstant8
#define CLOD_RT_CACHE_PENDING_UAV UintRootConstant9 // Per cluster built this frame: its range.
#define CLOD_RT_CACHE_ARGS_UAV UintRootConstant10
#define CLOD_RT_CACHE_CLAS_INFO_UAV UintRootConstant11
#define CLOD_RT_CACHE_CLAS_ADDRESSES_UAV UintRootConstant12
#define CLOD_RT_CACHE_SLOT_OMM_KEYS_UAV UintRootConstant13
#define CLOD_RT_CACHE_ARENA_ADDRESSES_SRV UintRootConstant14 // Per cluster: where the implicit build put it.
#define CLOD_RT_CACHE_ARENA_SIZES_SRV UintRootConstant15 // Per cluster, 8 bytes apart: its size.
#define CLOD_RT_CACHE_TRANSIENT_UAV UintRootConstant16 // Per group: uint2 [frame, first CLAS ID | TRANSIENT_DIRECT, or its entry].
#define CLOD_RT_CACHE_PACK_SCRATCH_UAV UintRootConstant44 // Per cluster built: two words, its page's fill and its place.
#define CLOD_RT_CACHE_PAGE_REFS_UAV UintRootConstant45 // Per page: the groups with clusters in it.
// This frame's transient arena, a slice of the pool's allocation past its pages: its first 128-byte unit from the pool's
// address, and its size in units.
#define CLOD_RT_CACHE_TRANSIENT_ARENA_UNIT UintRootConstant46
#define CLOD_RT_CACHE_TRANSIENT_ARENA_UNITS UintRootConstant47
#define CLOD_RT_CACHE_MOVE_ARGS_UAV UintRootConstant17
#define CLOD_RT_CACHE_MOVE_DESTINATIONS_UAV UintRootConstant18
#define CLOD_RT_CACHE_SLAB_ADDRESSES_SRV UintRootConstant19 // uint2 per slab descriptor index: its device address.
#define CLOD_RT_CACHE_SLAB_ADDRESS_COUNT UintRootConstant20
#define CLOD_RT_CACHE_POOL_LOW UintRootConstant21
#define CLOD_RT_CACHE_POOL_HIGH UintRootConstant22
#define CLOD_RT_CACHE_BUDGET UintRootConstant23 // Clusters built per frame at most.
#define CLOD_RT_CACHE_PROTECTED_FRAMES UintRootConstant24 // Frames that may still be tracing what a frame frees; below HISTORY.
#define CLOD_RT_CACHE_PAGE_COUNT UintRootConstant25
#define CLOD_RT_CACHE_CHUNK_COUNT UintRootConstant26
#define CLOD_RT_CACHE_ENTRY_COUNT UintRootConstant27
#define CLOD_RT_CACHE_OMM_ACTIVE UintRootConstant28
#define CLOD_RT_CACHE_COUNTERS_UAV UintRootConstant29 // For CLOD_RT_COUNTER_REJECTED_CLAS_ARGS.
// Opacity micromaps: a table by group of what the CPU has loaded, the groups admitted without them, and the cached groups
// to build again now that theirs are in.
#define CLOD_RT_CACHE_OMM_TABLE_UAV UintRootConstant30
#define CLOD_RT_CACHE_OMM_WANTED_UAV UintRootConstant31 // uint2 [group, material identity], counted in CTRL_OMM_WANTED.
#define CLOD_RT_CACHE_OMM_REBUILD_SRV UintRootConstant32 // Raw: [count, groups...].
#define CLOD_RT_CACHE_OMM_REBUILD_CAPACITY UintRootConstant33
#define CLOD_RT_CACHE_OMM_INDEX_POOL_LOW UintRootConstant34
#define CLOD_RT_CACHE_OMM_INDEX_POOL_HIGH UintRootConstant35
#define CLOD_RT_CACHE_OMM_ARRAY_POOL_LOW UintRootConstant36
#define CLOD_RT_CACHE_OMM_ARRAY_POOL_HIGH UintRootConstant37
#define CLOD_RT_CACHE_OMM_REBUILD_UAV UintRootConstant38 // CacheEnd empties the list: the CPU writes it only when it has more.
#define CLOD_RT_CACHE_OMM_RELEASE_UAV UintRootConstant39 // uint4 per entry retired this frame: [slot, group + 1, index unit, array unit].
#define CLOD_RT_CACHE_OMM_RETIRE_WINDOW UintRootConstant40 // Table slots looked at per frame: zero without micromap pool pressure.
#define CLOD_RT_CACHE_TRANSIENT_ID_BASE UintRootConstant41 // This frame's transient CLAS IDs: the build's cluster i takes BASE + i.
#define CLOD_RT_CACHE_MAX_KEEP_AGE UintRootConstant42 // The longest a group the RT view stopped using is kept, in frames.
#define CLOD_RT_CACHE_WATERMARK_PAGES UintRootConstant43 // Below this many free pages, idle groups are evicted.
// Micromap table entries: [group + 1, material identity, first index unit or NONE, array offset in 128-byte units or
// NONE]. Open addressing from CLodRtOmmHash. Under micromap pool pressure the GPU retires entries of idle groups, which
// leaves a tombstone the CPU reuses once it has read the release back.
#define CLOD_RT_OMM_TABLE_ENTRIES (1u << 17)
#define CLOD_RT_OMM_TABLE_PROBES 32u
#define CLOD_RT_OMM_NONE 0xFFFFFFFFu
#define CLOD_RT_OMM_TOMBSTONE 0xFFFFFFFFu // In an entry's first word.
#define CLOD_RT_OMM_ARRAY_ALIGNMENT 128u
#define CLOD_RT_OMM_RETIRE_WINDOW_MAX (CLOD_RT_OMM_TABLE_ENTRIES / 16u)

// Shared with CLodRayTracingSystem.cpp.
#define CLOD_RT_CACHE_MAX_GROUP_CLUSTERS 512u
#define CLOD_RT_CACHE_STATE_BYTES 16u
#define CLOD_RT_CACHE_UNCACHEABLE 0xFFFFFFFFu
#define CLOD_RT_CACHE_REQUESTED 0x80000000u // In a frame mark: the list needed the group and it had no current CLAS.
#define CLOD_RT_CACHE_MARK_FLAGS CLOD_RT_CACHE_REQUESTED
#define CLOD_RT_CACHE_INVALID 0xFFFFFFFFu
#define CLOD_RT_CACHE_UNIT_BYTES 128u
#define CLOD_RT_CACHE_PAGE_UNITS 64u // 8 KiB pages. A larger CLAS is transient.
#define CLOD_RT_CACHE_TAIL_CLASSES 7u // Shared-page block sizes for groups' last pages: 1, 2, 4 ... PAGE_UNITS units.
#define CLOD_RT_CACHE_PACK_WINDOW 8u // Pack tries a group's last this many pages for each cluster, first fit.
#define CLOD_RT_CACHE_CHUNK_IDS 32u
#define CLOD_RT_CACHE_MAX_CHUNKS (CLOD_RT_CACHE_MAX_GROUP_CLUSTERS / CLOD_RT_CACHE_CHUNK_IDS)
#define CLOD_RT_CACHE_HISTORY 8u // Queue tails kept, one per frame: PROTECTED_FRAMES is below this.
#define CLOD_RT_CACHE_GROUP_THREADS 1024u // The collect, admit, assign and pack passes are one group of this many.
#define CLOD_RT_TRANSIENT_DIRECT 0x80000000u // In a transient word: the rest is the group's first CLAS ID.

// Entries, 96 bytes: [group, page count, first page, micromap table slot + 1, chunk count, chunks (16), packed units, 0, 0].
#define CLOD_RT_CACHE_ENTRY_BYTES 96u
#define CLOD_RT_CACHE_ENTRY_CHUNKS_OFFSET 20u
#define CLOD_RT_CACHE_ENTRY_UNITS_OFFSET 84u

// The demand bitset, raw words: a summary bit per bitset word, then a bit per group, then a word per group for the material
// identity micromaps would be baked for (the largest any drawing material asked for, so the result doesn't depend on order).
#define CLOD_RT_DEMAND_GROUPS (1u << 21) // Groups the bitset covers: the cache's maxGroups.
#define CLOD_RT_DEMAND_BIT_WORDS (CLOD_RT_DEMAND_GROUPS / 32u)
#define CLOD_RT_DEMAND_SUMMARY_WORDS (CLOD_RT_DEMAND_BIT_WORDS / 32u)
#define CLOD_RT_DEMAND_BITS_OFFSET (CLOD_RT_DEMAND_SUMMARY_WORDS * 4u)
#define CLOD_RT_DEMAND_IDENTITY_OFFSET (CLOD_RT_DEMAND_BITS_OFFSET + CLOD_RT_DEMAND_BIT_WORDS * 4u)
#define CLOD_RT_DEMAND_BYTES (CLOD_RT_DEMAND_IDENTITY_OFFSET + CLOD_RT_DEMAND_GROUPS * 4u)

// Control words. Queue positions only ever grow; a queue's slot is the position modulo its item count.
#define CLOD_RT_CTRL_PAGE_HEAD 0u
#define CLOD_RT_CTRL_PAGE_TAIL 1u
#define CLOD_RT_CTRL_CHUNK_HEAD 2u
#define CLOD_RT_CTRL_CHUNK_TAIL 3u
#define CLOD_RT_CTRL_ENTRY_HEAD 4u
#define CLOD_RT_CTRL_ENTRY_TAIL 5u
#define CLOD_RT_CTRL_KEEP_AGE 6u // Idle groups older than this are evicted when pages run short.
#define CLOD_RT_CTRL_FRAME 7u // The frame number the cache's marks and transient words use. End advances it.
#define CLOD_RT_CTRL_TAIL_HISTORY 8u // Per frame (frame % HISTORY): the page, chunk and entry tails after it, 3 words.
#define CLOD_RT_CTRL_FRAME_FIRST 40u // Words cleared at the end of every frame, through FRAME_END.
#define CLOD_RT_CTRL_BUILD_COUNT 40u
#define CLOD_RT_CTRL_MOVE_COUNT 41u
#define CLOD_RT_CTRL_RANGE_COUNT 42u
#define CLOD_RT_CTRL_PERSISTENT 43u // Groups built and cached this frame.
#define CLOD_RT_CTRL_TRANSIENT 44u // Groups built for this frame only.
#define CLOD_RT_CTRL_TRANSIENT_CLUSTERS 45u
#define CLOD_RT_CTRL_REFUSED 46u // Groups that went transient for lack of entries, chunks or pages.
#define CLOD_RT_CTRL_EVICTED 47u
#define CLOD_RT_CTRL_REQUESTS 48u // Groups the list needed without a current CLAS (Collect).
#define CLOD_RT_CTRL_OMM_WANTED 49u // Micromap requests this frame.
#define CLOD_RT_CTRL_REBUILT 50u // Cached groups built again with their micromaps this frame.
#define CLOD_RT_CTRL_OMM_RELEASED 51u
#define CLOD_RT_CTRL_STALE 52u // Entries freed because their group's pages moved.
#define CLOD_RT_CTRL_ADMITTED 53u // Requests built this frame.
#define CLOD_RT_CTRL_NOT_BUILT 54u // Requests over the build budget: no CLAS this frame.
#define CLOD_RT_CTRL_BUILD_ARG_COUNT 55u // The build's count: BUILD_COUNT clamped to the budget.
#define CLOD_RT_CTRL_DISCARDED 56u // Groups built with no room in the cache or the transient arena: no CLAS this frame.
#define CLOD_RT_CTRL_DISCARDED_CLUSTERS 57u
#define CLOD_RT_CTRL_FRAME_END 60u
#define CLOD_RT_CTRL_BUILT_TOTAL 64u // Cumulative clusters built.
#define CLOD_RT_CTRL_FREED_TOTAL 65u // Cumulative clusters evicted or replaced.
#define CLOD_RT_CTRL_CACHED_GROUPS 66u
#define CLOD_RT_CTRL_CACHED_PAGES 67u
#define CLOD_RT_CTRL_MISSING_TOTAL 68u // Cumulative: clusters of the list placed with no CLAS (null references).
#define CLOD_RT_CTRL_OMM_RETIRE_CURSOR 69u
#define CLOD_RT_CTRL_EVICTED_TOTAL 70u
#define CLOD_RT_CTRL_TRANSIENT_TOTAL 71u // Cumulative transient clusters.
#define CLOD_RT_CTRL_NOT_BUILT_TOTAL 72u
#define CLOD_RT_CTRL_CACHED_CLUSTERS 73u
#define CLOD_RT_CTRL_CACHED_UNITS 74u // Units used in cached groups' pages.
#define CLOD_RT_CTRL_DISCARDED_TOTAL 75u
#define CLOD_RT_CTRL_OVERSIZE 76u // Cumulative clusters larger than a page.
#define CLOD_RT_CTRL_WORDS 80u

// Range records, one per group built this frame: [group, kind, first cluster built, cluster count, entry, slab, page signature,
// micromap table slot + 1, the entry a rebuild replaces + 1, kept (usable last frame: assigned, packed and placed first), 0, 0].
#define CLOD_RT_CACHE_RANGE_BYTES 48u
#define CLOD_RT_RANGE_REQUEST 0u // Needed by the list, before Assign.
#define CLOD_RT_RANGE_REBUILD 1u // A cached group to build again with its micromaps, before Assign.
#define CLOD_RT_RANGE_PERSISTENT 2u // Has an entry and chunks: packed into pages, else FALLBACK.
#define CLOD_RT_RANGE_PERSISTENT_REBUILD 3u // The same, replacing the group's entry; else DISCARD.
#define CLOD_RT_RANGE_TRANSIENT 4u // In this frame's transient arena, with the frame's own CLAS IDs.
#define CLOD_RT_RANGE_DISCARD 5u // Built for nothing: a rebuild that found no room, or a request with no room anywhere.
#define CLOD_RT_RANGE_FALLBACK 6u // In this frame's transient arena, with its entry's CLAS IDs: no pages.

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
#define CLOD_RT_COUNTER_FRAME_WORDS 12u // The words above are per frame; the scene build clears them.
// Cumulative, never cleared: clusters whose build arguments broke the batch's limits, built empty instead, since that is
// undefined behaviour.
#define CLOD_RT_COUNTER_REJECTED_CLAS_ARGS 12u
#define CLOD_RT_COUNTER_WORDS 14u // Even: the readback slice puts 8-byte-aligned query data after the counters.

// In the instance's BLAS entry: not an index but where to find one.
#define CLOD_RT_INSTANCE_BLAS_SHARED 0x80000000u // The rest is a share table slot.
#define CLOD_RT_INSTANCE_BLAS_VIA_ROW 0x40000000u // The rest is another instance row.
#define CLOD_RT_INSTANCE_BLAS_NONE 0xFFFFFFFFu

// InstanceID(): the draw record, with this bit on a part portal occurrence's row, whose assembly transform is then in
// the instance rows at InstanceIndex().
#define CLOD_RT_INSTANCE_ID_PORTAL 0x800000u
#define CLOD_RT_INSTANCE_ID_DRAW_MASK 0x7FFFFFu

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
#define CLOD_RT_VIS_INSTANCE_ROWS_SRV UintRootConstant12

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
#define CLOD_RT_SEC_INSTANCE_ROWS_SRV UintRootConstant13 // Path tracing too. See CLodRtInstanceTransform.

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
#define CLOD_RT_PT_FLAG_LENS 512u // Primary rays through the physical lens (Builtin::RayTracing::LensData, lensCamera.hlsli).

// Pixel watch (SARP_RT_PIXEL_WATCH): path traced pixels and hits that look wrong, with what made them, for the CPU to log.
// A header of counters, then CLOD_RT_WATCH_RECORDS records of each kind. A kind's counter is its record cursor too: only the
// first records of a frame are kept, and the count goes on.
#define CLOD_RT_PT_WATCH_UAV UintRootConstant14
#define CLOD_RT_PT_WATCH_BRIGHT UintRootConstant15 // asfloat: final luminance above which a pixel that hit something is recorded.
#define CLOD_RT_WATCH_BRIGHT_PIXEL 1u // Final radiance above the threshold. The record names the bounce that added most.
#define CLOD_RT_WATCH_BAD_MATERIAL 2u // Material inputs or BSDF results out of range or not finite, at any bounce.
#define CLOD_RT_WATCH_DARK_TERRAIN_PIXEL 3u // A primary terrain hit whose pixel came out black.
#define CLOD_RT_WATCH_RESOLVE_FAILED 4u // A hit whose CLAS info didn't resolve to a triangle of the right kind: the path ends black.
#define CLOD_RT_WATCH_DARK_TERRAIN_ALBEDO 5u // A primary terrain hit whose material came out black, with its RVT state.
#define CLOD_RT_WATCH_PRIMARY_ENDED 6u // A primary hit with no light to trace and no BSDF sample: the pixel gets only emission.
#define CLOD_RT_WATCH_KINDS 6u
#define CLOD_RT_WATCH_RECORDS 16u
#define CLOD_RT_WATCH_RECORD_WORDS 16u
#define CLOD_RT_WATCH_HEADER_WORDS 16u
// Header words: [kind] the count of each kind (1..6); then these.
#define CLOD_RT_WATCH_MAX_LUMINANCE 8u // asuint of the brightest final luminance of a pixel that hit something.
#define CLOD_RT_WATCH_NON_FINITE 9u // Pixels whose radiance wasn't finite, written black.
#define CLOD_RT_WATCH_WORDS (CLOD_RT_WATCH_HEADER_WORDS + CLOD_RT_WATCH_KINDS * CLOD_RT_WATCH_RECORDS * CLOD_RT_WATCH_RECORD_WORDS)
// Bad material flags (record word 0, bits 16 up).
#define CLOD_RT_WATCH_ALBEDO_RANGE 0x1u
#define CLOD_RT_WATCH_ALBEDO_NON_FINITE 0x2u
#define CLOD_RT_WATCH_EMISSIVE_NON_FINITE 0x4u
#define CLOD_RT_WATCH_EMISSIVE_HUGE 0x8u
#define CLOD_RT_WATCH_SCALAR_NON_FINITE 0x10u // Roughness, metallic, coat or fuzz.
#define CLOD_RT_WATCH_NORMAL_BAD 0x20u
#define CLOD_RT_WATCH_WEIGHT_HUGE 0x40u
#define CLOD_RT_WATCH_WEIGHT_NON_FINITE 0x80u
#define CLOD_RT_WATCH_DIRECT_NON_FINITE 0x100u
#define CLOD_RT_WATCH_DIRECT_HUGE 0x200u
#define CLOD_RT_WATCH_TERRAIN 0x8000u
#define CLOD_RT_WATCH_EMISSIVE_LIMIT 64.0f
#define CLOD_RT_WATCH_WEIGHT_LIMIT 16.0f

#endif // CLOD_RAY_TRACING_SETUP_ROOT_CONSTANTS_H
