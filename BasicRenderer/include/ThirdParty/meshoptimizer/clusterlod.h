/**
 * clusterlod - a small "library"/example built on top of meshoptimizer to generate cluster LOD hierarchies
 * This is intended to either be used as is, or as a reference for implementing similar functionality in your engine.
 *
 * To use this code, you need to have one source file which includes meshoptimizer.h and defines CLUSTERLOD_IMPLEMENTATION
 * before including this file. Other source files in your project can just include this file and use the provided functions.
 *
 * Copyright (C) 2016-2026, by Arseny Kapoulkine (arseny.kapoulkine@gmail.com)
 * This code is distributed under the MIT License. See notice at the end of this file.
 */
#pragma once

#include <stddef.h>

struct clodMesh;
struct clodValidateCandidate;
struct clodValidationStats;

// optional check of a simplification result before the builder accepts it (clodConfig::simplify_validate)
// called from iteration tasks, possibly concurrently, so it must be thread-safe; it must not modify anything the builder reads
// returns 0 to accept the candidate, or a nonzero mask of clodValidateReject reasons to reject it
typedef unsigned int (*clodValidateSimplify)(void* context, const clodMesh* mesh, const clodValidateCandidate* candidate);

// optional: called serially before each level's candidates are validated, with that level's whole surface: indices (into the
// original vertex buffer) and, per triangle, the group (clodValidateCandidate::group) whose input it is
typedef void (*clodValidateLevel)(void* context, const clodMesh* mesh, int depth, const unsigned int* indices, const unsigned int* triangle_groups, size_t index_count);

struct clodReplacementCandidate;
struct clodReplacementBatch;
struct clodReplacementStats;

// BR (replacement vertices): fits the shading of an update candidate's private vertices and decides which become replacement
// vertices (clodReplacementCandidate). Called from iteration tasks, possibly concurrently: it must be thread-safe and write only
// the candidate's outputs.
typedef void (*clodFitReplacements)(void* context, const clodMesh* mesh, clodReplacementCandidate* candidate);

// BR (replacement vertices): appends one group's accepted replacement vertices to every stream the mesh points at (positions,
// attributes, vertex_lock) and updates the mesh's pointers and vertex_count. Called serially, in group order, while no iteration
// task runs; the new vertices get ids batch->first_id onwards.
typedef void (*clodCommitReplacements)(void* context, clodMesh* mesh, const clodReplacementBatch* batch);

// BR (replacement vertices): optional independent measurement of a candidate's own deviation (mesh units, not merged with its
// clusters' errors). Called from iteration tasks like clodValidateSimplify; uses simplify_update_context.
typedef float (*clodMeasureSimplify)(void* context, const clodMesh* mesh, const clodValidateCandidate* candidate);

struct clodOptimalRequest;

// BR (stage 4, optimal placement): simplifies an update candidate's private buffers with replacement positions and attributes
// solved while collapses are ranked and validated, instead of meshopt_simplifyWithUpdate solving them after the collapse loop.
// Called from iteration tasks, possibly concurrently: it must be thread-safe and write only the request's buffers.
// Returns the result's index count (the result replaces request->indices).
typedef size_t (*clodSimplifyOptimal)(void* context, const clodMesh* mesh, clodOptimalRequest* request);

struct clodTopologyStats;

struct clodConfig
{
	// configuration of each cluster; maps to meshopt_buildMeshlets* parameters
	size_t max_vertices;
	size_t min_triangles;
	size_t max_triangles;

	// partitioning setup; maps to meshopt_partitionClusters parameters (plus optional partition sorting)
	// note: partition size is the target size, not maximum; actual partitions may be up to 1/3 larger (e.g. target 24 results in maximum 32)
	bool partition_spatial;
	bool partition_sort;
	size_t partition_size;

	// optional cap on number of distinct clodCluster::refined ids per output partition
	// 0 disables the cap; useful for downstream pipelines that require bounded child fanout
	size_t partition_max_refined_groups;

	// optional instrumentation counter: incremented for each partition split caused by partition_max_refined_groups
	// when null, instrumentation is disabled
	size_t* partition_refined_split_count;

	// clusterization setup; maps to meshopt_buildMeshletsSpatial / meshopt_buildMeshletsFlex
	bool cluster_spatial;
	float cluster_fill_weight;
	float cluster_split_factor;

	// every level aims to reduce the number of triangles by ratio, and considers clusters that don't reach the threshold stuck
	float simplify_ratio;
	float simplify_threshold;

	// to compute the error of simplified clusters, we use the formula that combines previous accumulated error as follows:
	// max(previous_error * simplify_error_merge_previous, current_error) + current_error * simplify_error_merge_additive
	float simplify_error_merge_previous;
	float simplify_error_merge_additive;

	// amplify the error of clusters that go through sloppy simplification to account for appearance degradation
	float simplify_error_factor_sloppy;

	// limit error by edge length, aiming to remove subpixel triangles even if the attribute error is high
	float simplify_error_edge_limit;

	// use permissive simplification instead of regular simplification (make sure to use attribute_protect_mask if this is set!)
	bool simplify_permissive;

	// use permissive or sloppy simplification but only if regular simplification gets stuck
	bool simplify_fallback_permissive;
	bool simplify_fallback_sloppy;

	// BR: sloppy as a last resort, with simplify_fallback_sloppy off: a group every other candidate leaves stuck above
	// simplify_threshold (it would be terminal, parentless with error FLT_MAX) is simplified by the sloppy fallback instead,
	// with its real (scaled) error. Not used with simplify_validate, whose candidates already decide about sloppy.
	bool simplify_last_resort_sloppy;
	// BR: with simplify_last_resort_sloppy, also take groups a candidate got under simplify_threshold but whose every candidate
	// validation rejected (stage 5 topology candidates); without it only groups no candidate got there
	bool simplify_last_resort_rejected;
	// BR: with simplify_last_resort_sloppy and simplify_update_fit, a last-resort result's vertices get their normals fitted to the
	// original as stage 2 refits unmoved vertices (sloppy moves none); replaced ones become replacement vertices
	bool simplify_last_resort_fit;
	// BR: the last resort's error factor in place of simplify_error_factor_sloppy; 0 uses that one
	float simplify_error_factor_last_resort;
	// BR: simplify_update_fit is set only for simplify_last_resort_fit; groups run no update candidates
	bool simplify_update_fit_last_resort_only;

	// use regularization during simplification to make triangle density more uniform, at some cost to overall triangle count; recommended for deformable objects
	bool simplify_regularize;

	// should clodCluster::bounds be computed based on the geometry of each cluster
	bool optimize_bounds;

	// should clodCluster::indices be optimized for locality; helps with rasterization performance and ray tracing performance in fast-build modes
	bool optimize_clusters;
	int optimize_clusters_level;

	// BR: optional validation of every simplification result (regular, permissive and sloppy) before it is accepted; null disables it
	// and leaves the build exactly as without it. A rejected candidate is replaced by the next one: the attribute-aware result instead
	// of sloppy, then the same passes at less aggressive targets. When every candidate is rejected the group stays terminal (keeps
	// its finer geometry). Sloppy output is only ever accepted when it validates. Validation chooses among results; it never edits one.
	clodValidateSimplify simplify_validate;
	void* simplify_validate_context;
	clodValidateLevel simplify_validate_level; // optional, see clodValidateLevel; uses simplify_validate_context

	// number of less aggressive targets tried after the configured one, evenly spaced between it and simplify_threshold
	int simplify_validate_retries;

	// optional counters, accumulated serially by clodBuildEx (not reset); null disables them
	clodValidationStats* simplify_validate_stats;

	// BR: replacement vertices. When simplify_update_fit is set, each group first tries an update candidate: it simplifies a
	// private copy of the group's vertices (meshopt_simplifyWithUpdate when simplify_update_positions, otherwise
	// meshopt_simplifyWithAttributes without moving anything), the fit callback fits its shading and picks the vertices that
	// become new ones, and an accepted candidate's new vertices are committed serially with simplify_update_commit, getting ids
	// past the current vertex count. Later levels simplify them like any other vertex. Locked vertices (group boundaries) are
	// never replaced. Null leaves the build exactly as without it.
	clodFitReplacements simplify_update_fit;
	clodCommitReplacements simplify_update_commit;
	void* simplify_update_context;
	size_t simplify_update_record_size; // builder bytes carried per replacement vertex from fit to commit
	bool simplify_update_positions;

	// optional independent error: every candidate's reported error becomes max(its own, the measurement); uses simplify_update_context
	clodMeasureSimplify simplify_measure;

	// optional counters for replacement vertices, accumulated serially (not reset)
	clodReplacementStats* simplify_update_stats;

	// BR (stage 4): optimal placement. When set (with simplify_update_fit), each target first offers an update candidate simplified
	// by this callback, then the meshopt_simplifyWithUpdate one and the rest as before; its vertices are fitted and committed like
	// any update candidate's. Null leaves the build exactly as without it.
	clodSimplifyOptimal simplify_optimal;
	void* simplify_optimal_context;

	// BR (stage 5): optional counters, accumulated serially (not reset); null disables them. With topology_diagnostics, every
	// group that ends terminal is also classified by which restriction stops it (see clodTopologyStats), by re-running the
	// simplifier with restrictions relaxed. Neither changes anything the build produces.
	clodTopologyStats* topology_stats;
	bool topology_diagnostics;

	// BR (stage 5, controlled topology changes): when set (with simplify_update_fit), a group that would otherwise be terminal
	// (stuck above simplify_threshold, or every candidate rejected) tries topology candidates before it is: update candidates
	// whose relaxable seams (below) lose their protect bits, so meshoptimizer may collapse across them, with shading refitted
	// from the original by simplify_update_fit. Each is validated: with simplify_validate when set, otherwise with this callback
	// (simplify_validate_context). When every one is rejected the group stays terminal. Null leaves the build exactly as without it.
	clodValidateSimplify simplify_topology;
	int simplify_topology_retries; // less aggressive targets after the configured one, up to simplify_threshold

	// BR (stage 5): which protected seams topology candidates (and the diagnostics' class 1) may relax. A seam vertex is a UV
	// seam when its update attributes (the UV streams, mesh.update_attribute_*) differ from its position's canonical vertex; when
	// the mesh carries none, when its protected attributes differ outside simplify_topology_relax_mask (bit K: attribute K).
	// Every other protected seam (normal hard edges, and the tangent differences they cause) is relaxable.
	unsigned int simplify_topology_relax_mask;
	bool simplify_topology_relax_uv; // relax UV seams too
	// BR (follow-up): with simplify_optimal, a topology candidate the optimal loop leaves rejected or stuck is followed by the plain
	// update candidate (meshoptimizer's loop, same relaxed seams) at the same target, as validated simplification offers both.
	// False: optimal-placement topology candidates only.
	bool simplify_topology_update_fallback;
};

struct clodMesh
{
	// input triangle indices
	const unsigned int* indices;
	size_t index_count;

	// total vertex count
	size_t vertex_count;

	// input vertex positions; must be 3 floats per vertex
	const float* vertex_positions;
	size_t vertex_positions_stride;

	// input vertex attributes; used for attribute-aware simplification and permissive simplification
	const float* vertex_attributes;
	size_t vertex_attributes_stride;

	// input vertex locks; allows to preserve additional seams (when not using attribute_protect_mask) or lock vertices via meshopt_SimplifyVertex_* flags
	const unsigned char* vertex_lock;

	// attribute weights for attribute-aware simplification; maps to meshopt_simplifyWithAttributes parameters
	const float* attribute_weights;
	size_t attribute_count;

	// attribute mask to flag attribute discontinuities for permissive simplification; mask (1<<K) corresponds to attribute K
	unsigned int attribute_protect_mask;

	// BR (replacement vertices): further attributes stored after attribute_count in the same stream (the stride must hold them)
	// and weighted by update_attribute_weights; only update candidates pass them to meshoptimizer, to have them solved
	const float* update_attribute_weights;
	size_t update_attribute_count;
};

// To compute approximate (perspective) projection error of a cluster in screen space (0..1; multiply by screen height to get pixels):
// - camera_proj is projection[1][1], or cot(fovy/2); camera_znear is *positive* near plane distance
// - for simplicity, we ignore perspective distortion and use rotationally invariant projection size estimation
// - return: bounds.error / max(distance(bounds.center, camera_position) - bounds.radius, camera_znear) * (camera_proj * 0.5f)
struct clodBounds
{
	// sphere bounds, in mesh coordinate space
	float center[3];
	float radius;

	// combined simplification error, in mesh coordinate space
	float error;
};

struct clodCluster
{
	// index of more refined group (with more triangles) that produced this cluster during simplification, or -1 for original geometry
	int refined;

	// cluster bounds; should only be used for culling, as bounds.error is not monotonic across DAG
	clodBounds bounds;

	// cluster indices; refer to the original mesh vertex buffer (BR: or, with replacement vertices, to the replacement vertices
	// committed after it: ids from the original vertex count onwards)
	const unsigned int* indices;
	size_t index_count;

	// cluster vertex count; indices[] has vertex_count unique entries
	size_t vertex_count;
};

struct clodGroup
{
	// DAG level the group was generated at
	int depth;

	// simplified group bounds (reflects error for clusters with clodCluster::refined == group id; error is FLT_MAX for terminal groups)
	// cluster should be rendered if:
	// 1. clodGroup::simplified for the group it's in is over error threshold
	// 2. cluster.refined is -1 *or* clodGroup::simplified for groups[cluster.refined].simplified is at or under error threshold
	clodBounds simplified;

	// which simplifier produced the clusters that refine into this group (clodSimplifyMethod); diagnostics only
	int simplify_method;

	// what validation decided for this group's simplification (clodSimplifyValidation); diagnostics only
	int simplify_validation;

	// BR: nonzero when the clusters that refine into this group came from an update candidate (replacement vertices); diagnostics only
	int simplify_replaced;
};

// clodGroup::simplify_method: the last meshoptimizer path the group's simplification took
enum clodSimplifyMethod
{
	clodSimplifyMethod_None = 0,       // terminal group: not simplified
	clodSimplifyMethod_Regular = 1,    // meshopt_simplifyWithAttributes with the configured options (permissive if simplify_permissive)
	clodSimplifyMethod_Permissive = 2, // the permissive fallback after regular simplification got stuck
	clodSimplifyMethod_Sloppy = 3,     // the sloppy fallback after the attribute-aware passes got stuck
	// BR (stage 5): topology candidates record Permissive. BasicRenderer runs permissive regular simplification without the
	// permissive fallback, so it marks them uniquely there.
	clodSimplifyMethod_Topology = clodSimplifyMethod_Permissive,
};

// clodGroup::simplify_validation
enum clodSimplifyValidation
{
	clodSimplifyValidation_None = 0,                   // not validated: validation off, or no candidate reached simplify_threshold
	clodSimplifyValidation_Accepted = 1,               // the first candidate validated
	clodSimplifyValidation_AcceptedAfterRejection = 2, // a later candidate validated after at least one was rejected
	clodSimplifyValidation_Rejected = 3,               // every candidate was rejected: the group is terminal
};

// reasons a clodValidateSimplify callback rejects a candidate (bit mask)
enum clodValidateReject
{
	clodValidateReject_Fold = 1 << 0,     // new folds: output faces opposite the matched original surface
	clodValidateReject_Shading = 1 << 1,  // new severe shading failures against the original's interpolated normals
	clodValidateReject_Coverage = 1 << 2, // input surface left without nearby output facing its way
	clodValidateReject_Spurious = 1 << 3, // output surface far from any original surface
	clodValidateReject_SourceCoverage = 1 << 4, // original surface under the group left without nearby output facing its way
	clodValidateReject_Texture = 1 << 5, // BR (stage 5): topology candidates only: texture slides farther than the radius on too much area
};

// a simplification result offered to clodValidateSimplify; all indices refer to the original mesh vertex buffer
struct clodValidateCandidate
{
	// the group's input: the merged indices of its clusters
	const unsigned int* input_indices;
	size_t input_index_count;

	// the candidate result
	const unsigned int* output_indices;
	size_t output_index_count;

	// the error the group would report if the candidate is accepted (merged with its clusters' errors), in mesh units
	float error;

	int method; // clodSimplifyMethod
	int depth;
	int attempt; // 0 for the first candidate of the group
	int group;   // the group's index within its level, as clodValidateLevel numbers it

	// BR: an update candidate (replacement vertices): output_indices index these private arrays of output_vertex_count vertices,
	// 3 floats each (positions, and the unit shading normals the candidate would emit), instead of the mesh vertex buffer.
	// Null for every other candidate.
	const float* output_positions;
	const float* output_normals;
	size_t output_vertex_count;

	// BR (stage 5): a topology candidate's emitted texcoords, 2 floats per private vertex (clodReplacementCandidate::uvs), or null
	const float* output_uvs;
	// BR (follow-up): an update candidate's private vertices' ids in the mesh vertex buffer (output_vertex_count), or null
	const unsigned int* output_vertex_ids;
};

// BR: an update candidate handed to clodFitReplacements. The private vertices are the group's input vertices, in increasing id.
struct clodReplacementCandidate
{
	size_t vertex_count;
	const unsigned int* vertex_ids;   // each private vertex's id in the mesh vertex buffer
	const float* positions;           // 3 floats per private vertex, after the update
	const float* attributes;          // mesh->vertex_attributes_stride bytes per private vertex, as the update left them (solved
	                                  // values; only meaningful where it moved the vertex), or null
	const unsigned char* locks;       // meshopt_SimplifyVertex_* flags; vertices with meshopt_SimplifyVertex_Lock never move or get replaced
	const unsigned char* moved;       // nonzero where the update moved the position
	const unsigned int* indices;      // the candidate's triangles, private indices
	size_t index_count;
	int depth;

	// outputs
	unsigned char* replace;           // nonzero: a vertex the candidate uses that becomes a replacement vertex (every moved one must)
	float* normals;                   // 3 floats per private vertex the candidate uses: the unit shading normal it would emit
	unsigned char* records;           // simplify_update_record_size bytes per private vertex, written for replaced vertices: what
	                                  // the commit needs to append them
	float* uvs;                       // stage 5, optional (null: not wanted): 2 floats per private vertex the candidate uses, the
	                                  // texcoord it would emit (left as is when the mesh has none)
};

// BR (stage 4): one optimal-placement simplification of an update candidate's private buffers (clodSimplifyOptimal)
struct clodOptimalRequest
{
	const unsigned int* vertex_ids;   // each private vertex's id in the mesh vertex buffer
	size_t vertex_count;
	float* positions;                 // 3 floats per private vertex; moved vertices are updated in place
	float* attributes;                // attributes_stride bytes per private vertex, or null; moved vertices' solved values are written
	size_t attributes_stride;
	const float* attribute_weights;   // attribute_count weights (the ranked attributes, then the solved-only ones)
	size_t attribute_count;
	const unsigned char* locks;       // meshopt_SimplifyVertex_* flags
	unsigned int* indices;            // the group's input triangles (private indices); replaced by the result
	size_t index_count;
	size_t target_index_count;
	unsigned int options;             // meshopt_Simplify* options
	int depth;

	// per input triangle, 3 floats the callback may fill once per group with what it needs across targets (the source-sheet
	// orientation under the triangle); triangle_reference_ready is nonzero once it has
	float* triangle_reference;
	int triangle_reference_ready;

	float error;                      // out: the result's error (absolute)
};

// BR: the replacement vertices of one accepted update candidate, as clodCommitReplacements receives them
struct clodReplacementBatch
{
	size_t count;
	size_t first_id;               // the id the first one gets (the mesh's vertex count before the commit)
	const unsigned int* parents;   // the vertex each one replaces
	const unsigned char* records;  // simplify_update_record_size bytes each, as the fit wrote them
};

// BR: clodConfig::simplify_update_stats
struct clodReplacementStats
{
	size_t candidates;        // update candidates built (one per target tried)
	size_t offered;           // update candidates offered for acceptance (validation, or the plain path)
	size_t accepted;          // update candidates accepted: groups simplified with replacement vertices
	size_t vertices;          // replacement vertices committed
	size_t vertices_moved;    // of which the update moved
	size_t locked_moved;      // locked vertices the update moved anyway (restored; should stay 0)
	size_t measured;          // candidates whose error was measured
	size_t measured_raised;   // of which the measurement exceeded the candidate's own error
	size_t optimal_candidates; // stage 4: optimal-placement candidates built (included in candidates)
	size_t optimal_offered;    // of which offered (included in offered)
	size_t optimal_accepted;   // of which accepted (included in accepted)
};

// BR (stage 5): clodConfig::topology_stats. Terminal groups and their triangles (what the coarsest cut draws), and, for groups
// stuck above simplify_threshold, the first relaxation (in this order) that lets the ordinary simplifier reach it:
// 0 group boundary locks dropped; 1 protect bits dropped on relaxable seams (all but UV seams: normal hard edges and the
// tangent differences they cause; clodConfig::simplify_topology_relax_mask); 2 every protect bit dropped; 3 vertices welded by
// position, positions only (every seam gone); 4 welded and boundary locks dropped; 5 disconnected components (all relaxed and
// still stuck, but meshopt_SimplifyPrune reaches it); 6 none of these (other limits).
struct clodTopologyStats
{
	size_t roots;                   // final single-cluster groups (the hierarchy's top; not stuck)
	size_t root_triangles;
	size_t rejected;                // groups made terminal by validation rejecting every candidate
	size_t rejected_triangles;
	size_t stuck;                   // groups stuck above simplify_threshold
	size_t stuck_triangles;
	size_t stuck_class[7];          // stuck groups per class (above)
	size_t stuck_class_triangles[7];
	size_t stuck_components;        // connected components (by position) over the stuck groups' inputs
	size_t stuck_small_triangles;   // triangles of stuck groups in components of at most 16 triangles
	size_t stuck_boundary_vertices; // distinct positions of the stuck groups locked as group boundary
	size_t stuck_positions;         // distinct positions of the stuck groups
	size_t stuck_uv_seam_vertices;  // vertices of the stuck groups on UV seams (update attributes differ from their position's)
	size_t stuck_uv_seam_tiny;      // of which every UV differs by less than 1/1024
	size_t stuck_uv_seam_integer;   // of which every UV differs by a whole number (within 1/1024), some by at least one: wrap seams
	size_t topology_groups;         // stage 5: groups that tried topology candidates (they had relaxable seams)
	size_t topology_offered;        // stage 5: topology candidates offered for validation
	size_t topology_rejected;       // of which rejected
	size_t topology_rejected_reason[6]; // per clodValidateReject bit (fold, shading, coverage, spurious, source coverage, texture)
	size_t topology_stuck;          // topology candidates still stuck above simplify_threshold (not offered)
	size_t topology_accepted;       // groups simplified by a topology candidate
	size_t topology_accepted_triangles; // their input triangles
	size_t topology_relaxed_vertices; // seam vertices whose protect bit accepted topology candidates dropped
	size_t last_resort;             // groups that would have been stuck, simplified by the last-resort sloppy fallback
	size_t last_resort_triangles;   // their input triangles
	size_t last_resort_rejected;    // of which every topology candidate had been rejected by validation
	size_t last_resort_rejected_triangles;
	// diagnostics (topology_diagnostics): the stuck class of groups validation rejected and of last-resort groups
	size_t rejected_class[7];
	size_t rejected_class_triangles[7];
	size_t last_resort_class[7];
	size_t last_resort_class_triangles[7];
};

// clodConfig::simplify_validate_stats
struct clodValidationStats
{
	size_t candidates;           // candidates passed to the callback
	size_t accepted;             // candidates accepted (one per group that validated)
	size_t rejected;             // candidates rejected
	size_t rejected_reason[5];   // rejected candidates per clodValidateReject bit (fold, shading, coverage, spurious, source coverage); one candidate may count in several
	size_t groups_first;         // groups whose first candidate validated
	size_t groups_retried;       // groups accepted after at least one rejection
	size_t groups_terminal;      // groups made terminal because every candidate was rejected
	size_t sloppy_accepted;      // sloppy candidates accepted
	size_t sloppy_rejected;      // sloppy candidates rejected
};

// gets called for each group in sequence
// returned value gets saved for clusters emitted from this group (clodCluster::refined)
typedef int (*clodOutput)(void* output_context, clodGroup group, const clodCluster* clusters, size_t cluster_count);

// extended output callback variant used by clodBuildEx; task/thread indices are for diagnostics only
typedef int (*clodOutputEx)(void* output_context, clodGroup group, const clodCluster* clusters, size_t cluster_count, size_t task_index, unsigned int thread_index);

// called once per LOD iteration (except the final terminal output pass)
// callback is expected to schedule tasks [0, task_count) and call clodBuild_iterationTask for each of them
typedef void (*clodIteration)(void* iteration_context, void* output_context, int depth, size_t task_count);

// BR (build cost): runs body(body_context, i) for every i in [0, count), in any order and concurrently; returns when all are done
typedef void (*clodParallelFor)(void* context, size_t count, void (*body)(void* body_context, size_t index), void* body_context);

struct clodBuildParallelConfig
{
	clodIteration iteration_callback;
	clodParallelFor parallel_for; // BR: optional; without it, the work runs serially
	void* parallel_for_context;
};

// when using BVH to determine cluster visibility, traverse the tree into nodes while bounds is over error threshold; for leaves,
// group clusters should be rendered if cluster.refined is -1 *or* clodGroup::simplified for groups[cluster.refined].simplified is at or under error threshold
struct clodNode
{
	// BVH node bounds, reflecting worst case error for all groups in the subtree
	clodBounds bounds;

	// leaf node: index of the group this node represents (or -1 for internal nodes)
	int group;

	// internal node: children are at nodes[child_offset..child_offset+child_count)
	unsigned int child_offset;
	unsigned int child_count;
};

#ifdef __cplusplus
extern "C"
{
#endif

// default configuration optimized for rasterization / raytracing
clodConfig clodDefaultConfig(size_t max_triangles);
clodConfig clodDefaultConfigRT(size_t max_triangles);

// build cluster LOD hierarchy, calling output callbacks as new clusters and groups are generated
// returns the total number of clusters produced
size_t clodBuild(clodConfig config, clodMesh mesh, void* output_context, clodOutput output_callback);

// extended build entry point supporting deterministic inner-threaded per-iteration execution
size_t clodBuildEx(clodConfig config, clodMesh mesh, void* output_context, clodOutputEx output_callback, const clodBuildParallelConfig* parallel_config);

// helper used by clodIteration callback to execute one task from the current iteration
void clodBuild_iterationTask(void* iteration_context, size_t task_index, unsigned int thread_index);

// extract meshlet-local indices from cluster indices produced by clodBuild
// fills triangles[] and vertices[] such that vertices[triangles[i]] == indices[i]
// returns number of unique vertices (which will be equal to clodCluster::vertex_count)
size_t clodLocalIndices(unsigned int* vertices, unsigned char* triangles, const unsigned int* indices, size_t index_count);

// upper bound on the number of nodes clodBuildHierarchy can produce
// level_count is the number of levels in the group DAG (max(clodGroup::depth) + 1)
size_t clodBuildHierarchyBound(size_t group_count, size_t node_width, size_t level_count);

// build a spatial hierarchy over the groups produced by clodBuild for accelerating cluster visibility
// level_count is the number of levels in the group DAG (max(clodGroup::depth) + 1)
// the result is a forest with one tree per DAG level:
// - nodes 0..level_count-1 are the roots of per-level trees, each covering groups with clodGroup::depth == level
// - remaining nodes are the tree nodes; each internal node has up to node_width children, and each leaf node refers to a single group
size_t clodBuildHierarchy(clodNode* nodes, const clodGroup* groups, size_t group_count, size_t node_width, size_t level_count);

#ifdef __cplusplus
} // extern "C"

template <typename Output>
size_t clodBuild(clodConfig config, clodMesh mesh, Output output)
{
	struct Call
	{
		static int output(void* output_context, clodGroup group, const clodCluster* clusters, size_t cluster_count)
		{
			return (*static_cast<Output*>(output_context))(group, clusters, cluster_count);
		}
	};

	return clodBuild(config, mesh, &output, &Call::output);
}

template <typename OutputEx, typename Iteration>
size_t clodBuildEx(clodConfig config, clodMesh mesh, OutputEx output, Iteration iteration)
{
	struct CallContext
	{
		OutputEx* output;
		Iteration* iteration;
	};

	struct Call
	{
		static int output(void* output_context, clodGroup group, const clodCluster* clusters, size_t cluster_count, size_t task_index, unsigned int thread_index)
		{
			CallContext* context = static_cast<CallContext*>(output_context);
			return (*context->output)(group, clusters, cluster_count, task_index, thread_index);
		}

		static void iterate(void* iteration_context, void* iteration_output_context, int depth, size_t task_count)
		{
			CallContext* context = static_cast<CallContext*>(iteration_output_context);
			(*context->iteration)(iteration_context, depth, task_count);
		}
	};

	CallContext callContext{ &output, &iteration };
	clodBuildParallelConfig parallelConfig{};
	parallelConfig.iteration_callback = &Call::iterate;
	return clodBuildEx(config, mesh, &callContext, &Call::output, &parallelConfig);
}

template <typename OutputEx>
size_t clodBuildEx(clodConfig config, clodMesh mesh, OutputEx output)
{
	struct Call
	{
		static int output(void* output_context, clodGroup group, const clodCluster* clusters, size_t cluster_count, size_t task_index, unsigned int thread_index)
		{
			return (*static_cast<OutputEx*>(output_context))(group, clusters, cluster_count, task_index, thread_index);
		}
	};

	return clodBuildEx(config, mesh, &output, &Call::output, nullptr);
}
#endif

#ifdef CLUSTERLOD_IMPLEMENTATION
// For reference, see the original Nanite paper:
// Brian Karis. Nanite: A Deep Dive. 2021
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <tracy/Tracy.hpp>

namespace clod
{

struct Cluster
{
	size_t vertices;
	std::vector<unsigned int> indices;

	int group;
	int refined;

	clodBounds bounds;
};

static clodBounds boundsCompute(const clodMesh& mesh, const std::vector<unsigned int>& indices, float error)
{
	ZoneScopedN("clusterlod::boundsCompute");
	meshopt_Bounds bounds = meshopt_computeClusterBounds(&indices[0], indices.size(), mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride);

	clodBounds result;
	result.center[0] = bounds.center[0];
	result.center[1] = bounds.center[1];
	result.center[2] = bounds.center[2];
	result.radius = bounds.radius;
	result.error = error;
	return result;
}

static clodBounds boundsMerge(const clodBounds* bounds, size_t count, size_t stride)
{
	meshopt_Bounds merged = meshopt_computeSphereBounds(&bounds[0].center[0], count, stride, &bounds[0].radius, stride);

	clodBounds result = {};
	result.center[0] = merged.center[0];
	result.center[1] = merged.center[1];
	result.center[2] = merged.center[2];
	result.radius = merged.radius;

	// merged bounds error must be conservative wrt cluster errors
	result.error = 0.f;
	for (size_t j = 0; j < count; ++j)
		result.error = std::max(result.error, reinterpret_cast<const clodBounds*>(reinterpret_cast<const char*>(bounds) + j * stride)->error);

	return result;
}

static clodBounds mergeGroups(const std::vector<Cluster>& clusters, const std::vector<int>& group)
{
	std::vector<clodBounds> bounds(group.size());
	for (size_t j = 0; j < group.size(); ++j)
		bounds[j] = clusters[group[j]].bounds;

	return boundsMerge(bounds.data(), bounds.size(), sizeof(clodBounds));
}

static std::vector<Cluster> clusterize(const clodConfig& config, const clodMesh& mesh, const unsigned int* indices, size_t index_count)
{
	ZoneScopedN("clusterlod::clusterize");
	TracyPlot("CLOD.clodBuild.clusterize.Indices", static_cast<int64_t>(index_count));
	size_t max_meshlets = meshopt_buildMeshletsBound(index_count, config.max_vertices, config.min_triangles);

	// Scratch the builder overwrites: allocated without zero-filling, which cost as much as a small builder stage.
	std::unique_ptr<meshopt_Meshlet[]> meshlets(new meshopt_Meshlet[max_meshlets]);
	std::unique_ptr<unsigned int[]> meshlet_vertices(new unsigned int[index_count]);

#if MESHOPTIMIZER_VERSION < 1000
	std::unique_ptr<unsigned char[]> meshlet_triangles(new unsigned char[index_count + max_meshlets * 3]); // account for 4b alignment
#else
	std::unique_ptr<unsigned char[]> meshlet_triangles(new unsigned char[index_count]);
#endif
	size_t meshlet_count = 0;

	if (config.cluster_spatial)
	{
		ZoneScopedN("clusterlod::clusterize::meshopt_buildMeshletsSpatial");
		meshlet_count = (meshopt_buildMeshletsSpatial(meshlets.get(), meshlet_vertices.get(), meshlet_triangles.get(), indices, index_count,
		    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    config.max_vertices, config.min_triangles, config.max_triangles, config.cluster_fill_weight));
	}
	else
	{
		ZoneScopedN("clusterlod::clusterize::meshopt_buildMeshletsFlex");
		meshlet_count = (meshopt_buildMeshletsFlex(meshlets.get(), meshlet_vertices.get(), meshlet_triangles.get(), indices, index_count,
		    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    config.max_vertices, config.min_triangles, config.max_triangles, 0.f, config.cluster_split_factor));
	}
	TracyPlot("CLOD.clodBuild.clusterize.Meshlets", static_cast<int64_t>(meshlet_count));

	std::vector<Cluster> clusters(meshlet_count);

	{
		ZoneScopedN("clusterlod::clusterize::BuildClusters");
		for (size_t i = 0; i < meshlet_count; ++i)
		{
			const meshopt_Meshlet& meshlet = meshlets[i];

			if (config.optimize_clusters)
				meshopt_optimizeMeshletLevel(&meshlet_vertices[meshlet.vertex_offset], meshlet.vertex_count, &meshlet_triangles[meshlet.triangle_offset], meshlet.triangle_count, config.optimize_clusters_level);

			clusters[i].vertices = meshlet.vertex_count;

			// note: we discard meshlet-local indices; they can be recovered by the caller using clodLocalIndices
			clusters[i].indices.resize(meshlet.triangle_count * 3);
			for (size_t j = 0; j < meshlet.triangle_count * 3; ++j)
				clusters[i].indices[j] = meshlet_vertices[meshlet.vertex_offset + meshlet_triangles[meshlet.triangle_offset + j]];

			clusters[i].group = -1;
			clusters[i].refined = -1;
		}
	}

	return clusters;
}

static std::vector<std::vector<int> > partition(const clodConfig& config, const clodMesh& mesh, const std::vector<Cluster>& clusters, const std::vector<int>& pending, const std::vector<unsigned int>& remap)
{
	ZoneScopedN("clusterlod::partition");
	TracyPlot("CLOD.clodBuild.partition.Pending", static_cast<int64_t>(pending.size()));
	if (pending.size() <= config.partition_size)
	{
		if (config.partition_max_refined_groups == 0)
			return {pending};

		std::vector<int> refinedSet;
		refinedSet.reserve(pending.size());
		for (size_t i = 0; i < pending.size(); ++i)
		{
			const int refined = clusters[pending[i]].refined;
			bool seen = false;
			for (size_t j = 0; j < refinedSet.size(); ++j)
			{
				if (refinedSet[j] == refined)
				{
					seen = true;
					break;
				}
			}

			if (!seen)
			{
				refinedSet.push_back(refined);
				if (refinedSet.size() > config.partition_max_refined_groups)
					break;
			}
		}

		if (refinedSet.size() <= config.partition_max_refined_groups)
			return {pending};
	}

	std::vector<unsigned int> cluster_indices;
	std::vector<unsigned int> cluster_counts(pending.size());

	// copy cluster index data into a flat array for partitioning
	size_t total_index_count = 0;
	for (size_t i = 0; i < pending.size(); ++i)
		total_index_count += clusters[pending[i]].indices.size();

	cluster_indices.reserve(total_index_count);

	for (size_t i = 0; i < pending.size(); ++i)
	{
		const Cluster& cluster = clusters[pending[i]];

		cluster_counts[i] = unsigned(cluster.indices.size());

		for (size_t j = 0; j < cluster.indices.size(); ++j)
			cluster_indices.push_back(remap[cluster.indices[j]]);
	}

	// partition clusters into groups; the output is a partition id per cluster
	std::vector<unsigned int> cluster_part(pending.size());
	size_t partition_count = 0;
	{
		ZoneScopedN("clusterlod::partition::meshopt_partitionClusters");
		partition_count = meshopt_partitionClusters(&cluster_part[0], &cluster_indices[0], cluster_indices.size(), &cluster_counts[0], cluster_counts.size(),
		    config.partition_spatial ? mesh.vertex_positions : NULL, remap.size(), mesh.vertex_positions_stride, config.partition_size);
	}
	TracyPlot("CLOD.clodBuild.partition.Partitions", static_cast<int64_t>(partition_count));

	// preallocate partitions for worst case
	std::vector<std::vector<int> > partitions(partition_count);
	for (size_t i = 0; i < partition_count; ++i)
		partitions[i].reserve(config.partition_size + config.partition_size / 3);

	std::vector<unsigned int> partition_remap;

	if (config.partition_sort)
	{
		ZoneScopedN("clusterlod::partition::SortPartitions");
		// compute partition points for sorting; any representative point will do, we use last cluster center for simplicity
		std::vector<float> partition_point(partition_count * 3);
		for (size_t i = 0; i < pending.size(); ++i)
			memcpy(&partition_point[cluster_part[i] * 3], clusters[pending[i]].bounds.center, sizeof(float) * 3);

		// sort partitions spatially; the output is a remap table from old index (partition id) to new index
		partition_remap.resize(partition_count);
		meshopt_spatialSortRemap(partition_remap.data(), partition_point.data(), partition_count, sizeof(float) * 3);
	}

	// distribute clusters into partitions, applying spatial order if requested
	for (size_t i = 0; i < pending.size(); ++i)
		partitions[partition_remap.empty() ? cluster_part[i] : partition_remap[cluster_part[i]]].push_back(pending[i]);

	if (config.partition_max_refined_groups > 0)
	{
		std::vector<std::vector<int> > limited;
		limited.reserve(partitions.size());

		for (size_t i = 0; i < partitions.size(); ++i)
		{
			const std::vector<int>& part = partitions[i];
			if (part.empty())
				continue;

			std::vector<int> refinedKeys;
			refinedKeys.reserve(part.size());
			std::vector<std::vector<int> > refinedBuckets;
			refinedBuckets.reserve(part.size());

			for (size_t j = 0; j < part.size(); ++j)
			{
				const int clusterIndex = part[j];
				const int refined = clusters[clusterIndex].refined;

				size_t keyIndex = refinedKeys.size();
				for (size_t k = 0; k < refinedKeys.size(); ++k)
				{
					if (refinedKeys[k] == refined)
					{
						keyIndex = k;
						break;
					}
				}

				if (keyIndex == refinedKeys.size())
				{
					refinedKeys.push_back(refined);
					refinedBuckets.push_back(std::vector<int>());
					refinedBuckets.back().reserve(8);
				}

				refinedBuckets[keyIndex].push_back(clusterIndex);
			}

			if (refinedBuckets.size() <= config.partition_max_refined_groups)
			{
				limited.push_back(part);
				continue;
			}

			if (config.partition_refined_split_count)
				(*config.partition_refined_split_count)++;

			std::vector<int> current;
			current.reserve(part.size());
			size_t currentRefinedCount = 0;

			for (size_t bucketIndex = 0; bucketIndex < refinedBuckets.size(); ++bucketIndex)
			{
				const std::vector<int>& bucket = refinedBuckets[bucketIndex];

				if (!current.empty() && currentRefinedCount >= config.partition_max_refined_groups)
				{
					limited.push_back(std::move(current));
					current = std::vector<int>();
					current.reserve(part.size());
					currentRefinedCount = 0;
				}

				current.insert(current.end(), bucket.begin(), bucket.end());
				currentRefinedCount++;
			}

			if (!current.empty())
				limited.push_back(std::move(current));
		}

		partitions = std::move(limited);
	}

	return partitions;
}

static void lockBoundary(std::vector<unsigned char>& locks, const std::vector<std::vector<int> >& groups, const std::vector<Cluster>& clusters, const std::vector<unsigned int>& remap, const unsigned char* vertex_lock)
{
	ZoneScopedN("clusterlod::lockBoundary");
	// for each remapped vertex, use bit 7 as temporary storage to indicate that the vertex has been used by a different group previously
	for (size_t i = 0; i < locks.size(); ++i)
		locks[i] &= ~((1 << 0) | (1 << 7));

	for (size_t i = 0; i < groups.size(); ++i)
	{
		// mark all remapped vertices as locked if seen by a prior group
		for (size_t j = 0; j < groups[i].size(); ++j)
		{
			const Cluster& cluster = clusters[groups[i][j]];

			for (size_t k = 0; k < cluster.indices.size(); ++k)
			{
				unsigned int v = cluster.indices[k];
				unsigned int r = remap[v];

				locks[r] |= locks[r] >> 7;
			}
		}

		// mark all remapped vertices as seen
		for (size_t j = 0; j < groups[i].size(); ++j)
		{
			const Cluster& cluster = clusters[groups[i][j]];

			for (size_t k = 0; k < cluster.indices.size(); ++k)
			{
				unsigned int v = cluster.indices[k];
				unsigned int r = remap[v];

				locks[r] |= 1 << 7;
			}
		}
	}

	for (size_t i = 0; i < locks.size(); ++i)
	{
		unsigned int r = remap[i];

		// consistently lock all vertices with the same position; keep protect bit if set
		locks[i] = (locks[r] & 1) | (locks[i] & meshopt_SimplifyVertex_Protect);

		if (vertex_lock)
			locks[i] |= vertex_lock[i];
	}
}

struct SloppyVertex
{
	float x, y, z;
	unsigned int id;
};

static void simplifyFallback(std::vector<unsigned int>& lod, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, size_t target_count, float* error)
{
	ZoneScopedN("clusterlod::simplifyFallback");
	std::vector<SloppyVertex> subset(indices.size());
	std::vector<unsigned char> subset_locks(indices.size());

	lod.resize(indices.size());

	size_t positions_stride = mesh.vertex_positions_stride / sizeof(float);

	// deindex the mesh subset to avoid calling simplifySloppy on the entire vertex buffer (which is prohibitively expensive without sparsity)
	for (size_t i = 0; i < indices.size(); ++i)
	{
		unsigned int v = indices[i];
		assert(v < mesh.vertex_count);

		subset[i].x = mesh.vertex_positions[v * positions_stride + 0];
		subset[i].y = mesh.vertex_positions[v * positions_stride + 1];
		subset[i].z = mesh.vertex_positions[v * positions_stride + 2];
		subset[i].id = v;

		// Sloppy simplification has no priority concept; promote priority vertices to locks so
		// coverage preservation isn't discarded when the regular simplifier gets stuck.
		subset_locks[i] = (locks[v] & meshopt_SimplifyVertex_Priority)
			? static_cast<unsigned char>(locks[v] | meshopt_SimplifyVertex_Lock)
			: locks[v];
		lod[i] = unsigned(i);
	}

	lod.resize(meshopt_simplifySloppy(&lod[0], &lod[0], lod.size(), &subset[0].x, subset.size(), sizeof(SloppyVertex), subset_locks.data(), target_count, FLT_MAX, error));

	// convert error to absolute
	*error *= meshopt_simplifyScale(&subset[0].x, subset.size(), sizeof(SloppyVertex));

	// restore original vertex indices
	for (size_t i = 0; i < lod.size(); ++i)
		lod[i] = subset[lod[i]].id;
}

static std::vector<unsigned int> simplify(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, size_t target_count, float* error, int* method)
{
	*method = clodSimplifyMethod_None;
	ZoneScopedN("clusterlod::simplify");
	TracyPlot("CLOD.clodBuild.simplify.InputIndices", static_cast<int64_t>(indices.size()));
	if (target_count > indices.size())
		return indices;

	std::vector<unsigned int> lod(indices.size());
	*method = clodSimplifyMethod_Regular;

	unsigned int options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | (config.simplify_permissive ? meshopt_SimplifyPermissive : 0) | (config.simplify_regularize ? meshopt_SimplifyRegularize : 0);

	{
		ZoneScopedN("clusterlod::simplify::meshopt_simplifyWithAttributes");
		lod.resize(meshopt_simplifyWithAttributes(&lod[0], &indices[0], indices.size(),
		    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    mesh.vertex_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count,
		    &locks[0], target_count, FLT_MAX, options, error));
	}

	if (lod.size() > target_count && config.simplify_fallback_permissive && !config.simplify_permissive)
	{
		ZoneScopedN("clusterlod::simplify::PermissiveFallback");
		lod.resize(meshopt_simplifyWithAttributes(&lod[0], &indices[0], indices.size(),
		    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    mesh.vertex_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count,
		    &locks[0], target_count, FLT_MAX, options | meshopt_SimplifyPermissive, error));
		*method = clodSimplifyMethod_Permissive;
	}

	// while it's possible to call simplifySloppy directly, it doesn't support sparsity or absolute error, so we need to do some extra work
	if (lod.size() > target_count && config.simplify_fallback_sloppy)
	{
		simplifyFallback(lod, mesh, indices, locks, target_count, error);
		*error *= config.simplify_error_factor_sloppy; // scale error up to account for appearance degradation
		*method = clodSimplifyMethod_Sloppy;
	}

	// optionally limit error by edge length, aiming to remove subpixel triangles even if the attribute error is high
	if (config.simplify_error_edge_limit > 0)
	{
		float max_edge_sq = 0;

		for (size_t i = 0; i < indices.size(); i += 3)
		{
			unsigned int a = indices[i + 0], b = indices[i + 1], c = indices[i + 2];
			assert(a < mesh.vertex_count && b < mesh.vertex_count && c < mesh.vertex_count);

			const float* va = &mesh.vertex_positions[a * (mesh.vertex_positions_stride / sizeof(float))];
			const float* vb = &mesh.vertex_positions[b * (mesh.vertex_positions_stride / sizeof(float))];
			const float* vc = &mesh.vertex_positions[c * (mesh.vertex_positions_stride / sizeof(float))];

			// compute squared edge lengths
			float eab = (va[0] - vb[0]) * (va[0] - vb[0]) + (va[1] - vb[1]) * (va[1] - vb[1]) + (va[2] - vb[2]) * (va[2] - vb[2]);
			float eac = (va[0] - vc[0]) * (va[0] - vc[0]) + (va[1] - vc[1]) * (va[1] - vc[1]) + (va[2] - vc[2]) * (va[2] - vc[2]);
			float ebc = (vb[0] - vc[0]) * (vb[0] - vc[0]) + (vb[1] - vc[1]) * (vb[1] - vc[1]) + (vb[2] - vc[2]) * (vb[2] - vc[2]);

			float emax = std::max(std::max(eab, eac), ebc);
			float emin = std::min(std::min(eab, eac), ebc);

			// we prefer using min edge length to reduce the number of triangles <1px thick, but need some stopgap for thin and long triangles like wires
			max_edge_sq = std::max(max_edge_sq, std::max(emin, emax / 4));
		}

		// adjust the error to limit it for dense clusters based on edge lengths
		*error = std::min(*error, sqrtf(max_edge_sq) * config.simplify_error_edge_limit);
	}

	return lod;
}

// BR: the pieces of simplify() for the validated path, which must try them in a different order. With validation off, simplify()
// runs instead and the build is unchanged.
static std::vector<unsigned int> simplifyAttributeAware(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, size_t target_count, float* error, int* method)
{
	ZoneScopedN("clusterlod::simplifyAttributeAware");
	std::vector<unsigned int> lod(indices.size());
	*method = clodSimplifyMethod_Regular;

	unsigned int options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | (config.simplify_permissive ? meshopt_SimplifyPermissive : 0) | (config.simplify_regularize ? meshopt_SimplifyRegularize : 0);

	lod.resize(meshopt_simplifyWithAttributes(&lod[0], &indices[0], indices.size(),
	    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
	    mesh.vertex_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count,
	    &locks[0], target_count, FLT_MAX, options, error));

	if (lod.size() > target_count && config.simplify_fallback_permissive && !config.simplify_permissive)
	{
		lod.resize(meshopt_simplifyWithAttributes(&lod[0], &indices[0], indices.size(),
		    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    mesh.vertex_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count,
		    &locks[0], target_count, FLT_MAX, options | meshopt_SimplifyPermissive, error));
		*method = clodSimplifyMethod_Permissive;
	}

	return lod;
}

static void limitErrorByEdgeLength(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, float* error)
{
	if (config.simplify_error_edge_limit <= 0)
		return;

	float max_edge_sq = 0;
	const size_t stride = mesh.vertex_positions_stride / sizeof(float);

	for (size_t i = 0; i < indices.size(); i += 3)
	{
		const float* va = &mesh.vertex_positions[indices[i + 0] * stride];
		const float* vb = &mesh.vertex_positions[indices[i + 1] * stride];
		const float* vc = &mesh.vertex_positions[indices[i + 2] * stride];

		float eab = (va[0] - vb[0]) * (va[0] - vb[0]) + (va[1] - vb[1]) * (va[1] - vb[1]) + (va[2] - vb[2]) * (va[2] - vb[2]);
		float eac = (va[0] - vc[0]) * (va[0] - vc[0]) + (va[1] - vc[1]) * (va[1] - vc[1]) + (va[2] - vc[2]) * (va[2] - vc[2]);
		float ebc = (vb[0] - vc[0]) * (vb[0] - vc[0]) + (vb[1] - vc[1]) * (vb[1] - vc[1]) + (vb[2] - vc[2]) * (vb[2] - vc[2]);

		float emax = std::max(std::max(eab, eac), ebc);
		float emin = std::min(std::min(eab, eac), ebc);
		max_edge_sq = std::max(max_edge_sq, std::max(emin, emax / 4));
	}

	*error = std::min(*error, sqrtf(max_edge_sq) * config.simplify_error_edge_limit);
}

// BR (stage 5): per-task topology diagnostics and counters; summed into clodTopologyStats in task order
struct TopologyTaskStats
{
	int stuck_class = -1; // clodTopologyStats class of a stuck group, -1 otherwise
	size_t components = 0;
	size_t small_triangles = 0;
	size_t boundary_vertices = 0;
	size_t positions = 0;
	size_t uv_seam = 0;
	size_t uv_seam_tiny = 0;
	size_t uv_seam_integer = 0;
	size_t groups = 0;
	size_t offered = 0;
	size_t rejected = 0;
	size_t rejected_reason[6] = {};
	size_t stuck = 0;
	size_t relaxed = 0; // of the accepted candidate
};

// The protected attributes (attribute_protect_mask bits) in which vertex v differs from its position's canonical vertex, as
// clodBuildEx compares them to set protect bits.
static unsigned int seamDifference(const clodMesh& mesh, const std::vector<unsigned int>& remap, unsigned int v)
{
	const unsigned int r = remap[v];
	if (r == v || !mesh.attribute_protect_mask || !mesh.vertex_attributes)
		return 0;
	const size_t max_attributes = mesh.vertex_attributes_stride / sizeof(float);
	unsigned int mask = 0;
	for (size_t j = 0; j < max_attributes && j < 32; ++j)
		if ((mesh.attribute_protect_mask & (1u << j)) && mesh.vertex_attributes[v * max_attributes + j] != mesh.vertex_attributes[r * max_attributes + j])
			mask |= 1u << j;
	return mask;
}

// Whether a stage 5 topology candidate may drop vertex v's protect bit (clodConfig::simplify_topology_relax_mask). UVs within
// 1/1024 of each other (relative above 1) are the same: the separately solved wedges of one moved position differ by rounding.
static bool relaxableSeam(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& remap, unsigned int v)
{
	const unsigned int r = remap[v];
	if (r == v || config.simplify_topology_relax_uv)
		return true;
	if (mesh.update_attribute_count && mesh.vertex_attributes)
	{
		const size_t max_attributes = mesh.vertex_attributes_stride / sizeof(float);
		const size_t first = mesh.attribute_count, last = std::min(max_attributes, mesh.attribute_count + mesh.update_attribute_count);
		for (size_t j = first; j < last; ++j)
		{
			const float a = mesh.vertex_attributes[v * max_attributes + j], b = mesh.vertex_attributes[r * max_attributes + j];
			if (!(fabsf(a - b) <= (1.f / 1024) * std::max(1.f, std::max(fabsf(a), fabsf(b)))))
				return false;
		}
		return true;
	}
	return (seamDifference(mesh, remap, v) & ~config.simplify_topology_relax_mask) == 0;
}

// Connected components of a triangle list by position (remap), over a compact numbering of the positions it uses. Fills
// component[] per triangle and returns the component count; component_triangles gets each component's triangle count.
static size_t topologyComponents(const std::vector<unsigned int>& indices, const std::vector<unsigned int>& remap, std::vector<unsigned int>& positions, std::vector<unsigned int>& triangle_component, std::vector<unsigned int>& component_triangles)
{
	positions.clear();
	for (size_t i = 0; i < indices.size(); ++i)
		positions.push_back(remap[indices[i]]);
	std::sort(positions.begin(), positions.end());
	positions.erase(std::unique(positions.begin(), positions.end()), positions.end());

	std::vector<unsigned int> parent(positions.size());
	for (size_t i = 0; i < parent.size(); ++i)
		parent[i] = unsigned(i);
	const auto find = [&](unsigned int x)
	{
		while (parent[x] != x)
		{
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	};
	const auto local = [&](unsigned int v) { return unsigned(std::lower_bound(positions.begin(), positions.end(), remap[v]) - positions.begin()); };

	for (size_t i = 0; i < indices.size(); i += 3)
	{
		const unsigned int a = find(local(indices[i + 0])), b = find(local(indices[i + 1])), c = find(local(indices[i + 2]));
		const unsigned int m = std::min(a, std::min(b, c));
		parent[a] = m;
		parent[b] = m;
		parent[c] = m;
	}

	std::vector<unsigned int> number(positions.size(), ~0u);
	size_t count = 0;
	triangle_component.resize(indices.size() / 3);
	component_triangles.clear();
	for (size_t i = 0; i < indices.size(); i += 3)
	{
		const unsigned int root = find(local(indices[i]));
		if (number[root] == ~0u)
		{
			number[root] = unsigned(count++);
			component_triangles.push_back(0);
		}
		triangle_component[i / 3] = number[root];
		component_triangles[number[root]]++;
	}
	return count;
}

// Classifies why a group got stuck above simplify_threshold by re-running the regular simplifier with restrictions relaxed (see
// clodTopologyStats). Diagnostics only: reads the shared streams, writes only the task's counters.
static void diagnoseStuckGroup(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, const std::vector<unsigned int>& remap, size_t target_count, TopologyTaskStats& stats)
{
	ZoneScopedN("clusterlod::diagnoseStuckGroup");
	const size_t threshold = size_t(double(indices.size()) * config.simplify_threshold);
	const unsigned int options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | (config.simplify_permissive ? meshopt_SimplifyPermissive : 0) | (config.simplify_regularize ? meshopt_SimplifyRegularize : 0);

	std::vector<unsigned int> positions, triangle_component, component_triangles;
	stats.components = topologyComponents(indices, remap, positions, triangle_component, component_triangles);
	for (size_t t = 0; t < triangle_component.size(); ++t)
		stats.small_triangles += component_triangles[triangle_component[t]] <= 16 ? 1 : 0;
	stats.positions = positions.size();
	for (size_t k = 0; k < positions.size(); ++k)
	{
		const unsigned int r = positions[k];
		const bool external = mesh.vertex_lock && (mesh.vertex_lock[r] & meshopt_SimplifyVertex_Lock);
		stats.boundary_vertices += ((locks[r] & meshopt_SimplifyVertex_Lock) && !external) ? 1 : 0;
	}

	// UV seams by kind (diagnostics)
	if (mesh.update_attribute_count && mesh.vertex_attributes)
	{
		std::vector<unsigned int> vertices(indices);
		std::sort(vertices.begin(), vertices.end());
		vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
		const size_t max_attributes = mesh.vertex_attributes_stride / sizeof(float);
		const size_t first = mesh.attribute_count, last = std::min(max_attributes, mesh.attribute_count + mesh.update_attribute_count);
		for (unsigned int v : vertices)
		{
			const unsigned int r = remap[v];
			if (r == v)
				continue;
			bool differs = false, tiny = true, integer = true, whole = false;
			for (size_t j = first; j < last; ++j)
			{
				const float d = mesh.vertex_attributes[v * max_attributes + j] - mesh.vertex_attributes[r * max_attributes + j];
				if (d == 0.f)
					continue;
				differs = true;
				tiny = tiny && fabsf(d) < 1.f / 1024;
				integer = integer && fabsf(d - roundf(d)) < 1.f / 1024;
				whole = whole || fabsf(d) >= 0.5f;
			}
			if (!differs)
				continue;
			stats.uv_seam++;
			stats.uv_seam_tiny += tiny ? 1 : 0;
			stats.uv_seam_integer += (!tiny && integer && whole) ? 1 : 0;
		}
	}

	// protect bits dropped on relaxable seams (class 1), then on every seam (class 2)
	std::vector<unsigned char> relaxed(locks), unprotected(locks);
	for (size_t i = 0; i < indices.size(); ++i)
	{
		const unsigned int v = indices[i];
		unprotected[v] &= ~meshopt_SimplifyVertex_Protect;
		if (relaxableSeam(config, mesh, remap, v))
			relaxed[v] &= ~meshopt_SimplifyVertex_Protect;
	}

	// relaxed locks: group boundary locks dropped (locks the mesh asked for are kept)
	std::vector<unsigned char> unlocked(locks);
	for (size_t i = 0; i < indices.size(); ++i)
	{
		const unsigned int v = indices[i];
		const unsigned char external = mesh.vertex_lock ? mesh.vertex_lock[v] : 0;
		unlocked[v] = (unlocked[v] & ~meshopt_SimplifyVertex_Lock) | (external & meshopt_SimplifyVertex_Lock);
	}

	// welded: every corner on its position's canonical vertex, degenerate triangles dropped
	std::vector<unsigned int> welded;
	welded.reserve(indices.size());
	for (size_t i = 0; i < indices.size(); i += 3)
	{
		const unsigned int a = remap[indices[i + 0]], b = remap[indices[i + 1]], c = remap[indices[i + 2]];
		if (a == b || b == c || c == a)
			continue;
		welded.push_back(a);
		welded.push_back(b);
		welded.push_back(c);
	}
	std::vector<unsigned char> welded_locks(locks), welded_unlocked(unlocked);
	for (size_t i = 0; i < welded.size(); ++i)
	{
		welded_locks[welded[i]] &= ~meshopt_SimplifyVertex_Protect;
		welded_unlocked[welded[i]] &= ~meshopt_SimplifyVertex_Protect;
	}

	std::vector<unsigned int> lod(indices.size());
	float error = 0.f;
	const auto attributeAware = [&](const std::vector<unsigned char>& l) -> size_t
	{
		return meshopt_simplifyWithAttributes(&lod[0], &indices[0], indices.size(), mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    mesh.vertex_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count, &l[0], target_count, FLT_MAX, options, &error);
	};
	const auto positionsOnly = [&](const std::vector<unsigned char>& l, unsigned int extra) -> size_t
	{
		if (welded.empty())
			return 0;
		return meshopt_simplifyWithAttributes(&lod[0], &welded[0], welded.size(), mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    NULL, 0, NULL, 0, &l[0], target_count, FLT_MAX, options | extra, &error);
	};

	if (attributeAware(unlocked) <= threshold)
		stats.stuck_class = 0;
	else if (config.simplify_permissive && attributeAware(relaxed) <= threshold)
		stats.stuck_class = 1;
	else if (config.simplify_permissive && attributeAware(unprotected) <= threshold)
		stats.stuck_class = 2;
	else if (positionsOnly(welded_locks, 0) <= threshold)
		stats.stuck_class = 3;
	else if (positionsOnly(welded_unlocked, 0) <= threshold)
		stats.stuck_class = 4;
	else if (positionsOnly(welded_unlocked, meshopt_SimplifyPrune) <= threshold)
		stats.stuck_class = 5;
	else
		stats.stuck_class = 6;
}

// per-task validation counters; summed into clodValidationStats in task order
struct ValidationTaskStats
{
	size_t candidates = 0;
	size_t rejected = 0;
	size_t rejected_reason[5] = {};
	size_t sloppy_accepted = 0;
	size_t sloppy_rejected = 0;
};

// BR: replacement vertices (clodConfig::simplify_update_fit).

// In a task's result, before its commit, a replacement vertex is this bit plus its index in the task's replacement list.
static const unsigned int kReplacementBit = 0x80000000u;

// per-task replacement counters; summed into clodReplacementStats in task order
struct ReplacementTaskStats
{
	size_t candidates = 0;
	size_t offered = 0;
	size_t locked_moved = 0;
	size_t measured = 0;
	size_t measured_raised = 0;
	size_t optimal_candidates = 0;
	size_t optimal_offered = 0;
};

// an update candidate with its private buffers (clodReplacementCandidate)
struct UpdateCandidate
{
	bool optimal = false; // stage 4: simplified by clodConfig::simplify_optimal
	std::vector<unsigned int> ids; // private vertex -> mesh vertex id, increasing
	std::vector<float> positions;
	std::vector<float> attributes;
	std::vector<unsigned char> locks;
	std::vector<unsigned char> moved;
	std::vector<unsigned char> replace;
	std::vector<float> normals;
	std::vector<unsigned char> records;
	std::vector<float> uvs; // stage 5: emitted texcoords (topology candidates only)
	std::vector<unsigned int> indices; // private
	float error = 0.f;
};

// an accepted update candidate's replacement vertices, waiting for the serial commit
struct ReplacementOutput
{
	std::vector<unsigned int> parents;
	std::vector<unsigned char> records;
	size_t moved = 0;
};

// Per-group state of the optimal-placement candidates (stage 4), shared across the group's targets.
struct OptimalGroupState
{
	std::vector<float> reference; // clodOptimalRequest::triangle_reference
	int reference_ready = 0;
};

// BR (build cost): which results simplifyUpdate fits. A result its caller cannot use (not a triangle list within simplify_threshold,
// over max_size, or of skip_size, the size of a result already rejected) is left unfitted, since the fit is most of a candidate's
// cost and its output would be discarded; the result is otherwise the same.
struct UpdateFitGate
{
	size_t max_size;
	size_t skip_size;
};

// Simplifies a private copy of the group's vertices towards target_count and has the builder fit the result. The shared mesh
// streams are only read. With optimal (stage 4), clodConfig::simplify_optimal simplifies it instead of meshoptimizer.
// With relax_remap (stage 5, a topology candidate), relaxable seam vertices (relaxableSeam) lose their protect bit in the private
// copy, and *relaxed counts them.
// With fit_only, indices are already the result: nothing is simplified, only fitted (clodConfig::simplify_last_resort_fit).
static void simplifyUpdate(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, size_t target_count, int depth, UpdateCandidate& c, ReplacementTaskStats& stats, OptimalGroupState* optimal = NULL, const std::vector<unsigned int>* relax_remap = NULL, size_t* relaxed = NULL, bool fit_only = false, const UpdateFitGate* gate = NULL)
{
	ZoneScopedN("clusterlod::simplifyUpdate");
	stats.candidates++;
	c.optimal = optimal != NULL;
	if (optimal)
		stats.optimal_candidates++;

	c.ids.assign(indices.begin(), indices.end());
	std::sort(c.ids.begin(), c.ids.end());
	c.ids.erase(std::unique(c.ids.begin(), c.ids.end()), c.ids.end());
	const size_t vertex_count = c.ids.size();

	c.indices.resize(indices.size());
	for (size_t i = 0; i < indices.size(); ++i)
		c.indices[i] = unsigned(std::lower_bound(c.ids.begin(), c.ids.end(), indices[i]) - c.ids.begin());

	const size_t positions_stride = mesh.vertex_positions_stride / sizeof(float);
	const size_t attributes_stride = mesh.vertex_attributes ? mesh.vertex_attributes_stride / sizeof(float) : 0;
	c.positions.resize(vertex_count * 3);
	c.attributes.resize(vertex_count * attributes_stride);
	c.locks.resize(vertex_count);
	for (size_t k = 0; k < vertex_count; ++k)
	{
		const unsigned int v = c.ids[k];
		memcpy(&c.positions[k * 3], &mesh.vertex_positions[v * positions_stride], sizeof(float) * 3);
		if (attributes_stride)
			memcpy(&c.attributes[k * attributes_stride], &mesh.vertex_attributes[v * attributes_stride], sizeof(float) * attributes_stride);
		c.locks[k] = locks[v];
		if (relax_remap && (c.locks[k] & meshopt_SimplifyVertex_Protect) && relaxableSeam(config, mesh, *relax_remap, v))
		{
			c.locks[k] &= ~meshopt_SimplifyVertex_Protect;
			if (relaxed)
				(*relaxed)++;
		}
	}
	const std::vector<float> original_positions = c.positions;

	// the ranked attributes, then (when updating) the solved-only ones after them in the same stream
	float weights[32] = {};
	size_t attribute_count = attributes_stride ? mesh.attribute_count : 0;
	for (size_t k = 0; k < attribute_count && k < 32; ++k)
		weights[k] = mesh.attribute_weights[k];
	if (config.simplify_update_positions && attributes_stride && mesh.update_attribute_weights && attribute_count + mesh.update_attribute_count <= 32 &&
	    attribute_count + mesh.update_attribute_count <= attributes_stride)
	{
		for (size_t k = 0; k < mesh.update_attribute_count; ++k)
			weights[attribute_count + k] = mesh.update_attribute_weights[k];
		attribute_count += mesh.update_attribute_count;
	}

	const unsigned int options = meshopt_SimplifyErrorAbsolute | (config.simplify_permissive ? meshopt_SimplifyPermissive : 0) | (config.simplify_regularize ? meshopt_SimplifyRegularize : 0);
	size_t count = 0;
	if (fit_only)
	{
		count = c.indices.size();
	}
	else if (optimal)
	{
		optimal->reference.resize(indices.size());
		clodOptimalRequest request = {};
		request.vertex_ids = c.ids.data();
		request.vertex_count = vertex_count;
		request.positions = c.positions.data();
		request.attributes = attributes_stride ? c.attributes.data() : NULL;
		request.attributes_stride = attributes_stride * sizeof(float);
		request.attribute_weights = weights;
		request.attribute_count = attribute_count;
		request.locks = c.locks.data();
		request.indices = c.indices.data();
		request.index_count = c.indices.size();
		request.target_index_count = target_count;
		request.options = options;
		request.depth = depth;
		request.triangle_reference = optimal->reference.data();
		request.triangle_reference_ready = optimal->reference_ready;
		count = config.simplify_optimal(config.simplify_optimal_context, &mesh, &request);
		optimal->reference_ready = request.triangle_reference_ready;
		c.error = request.error;
	}
	else if (config.simplify_update_positions)
	{
		count = meshopt_simplifyWithUpdate(c.indices.data(), c.indices.size(), c.positions.data(), vertex_count, sizeof(float) * 3,
		    attributes_stride ? c.attributes.data() : NULL, attributes_stride * sizeof(float), weights, attribute_count,
		    c.locks.data(), target_count, FLT_MAX, options, &c.error);
	}
	else
	{
		count = meshopt_simplifyWithAttributes(c.indices.data(), c.indices.data(), c.indices.size(), c.positions.data(), vertex_count, sizeof(float) * 3,
		    attributes_stride ? c.attributes.data() : NULL, attributes_stride * sizeof(float), weights, attribute_count,
		    c.locks.data(), target_count, FLT_MAX, options, &c.error);
	}
	c.indices.resize(count);
	const bool usable = !gate || (count >= 3 && count % 3 == 0 && !(count > indices.size() * config.simplify_threshold) && count <= gate->max_size && count != gate->skip_size);

	c.moved.assign(vertex_count, 0);
	for (size_t k = 0; k < vertex_count; ++k)
	{
		if (memcmp(&c.positions[k * 3], &original_positions[k * 3], sizeof(float) * 3) == 0)
			continue;

		// meshoptimizer leaves locked vertices in place (finalizeVertices skips them); keep it that way whatever it does
		if (c.locks[k] & meshopt_SimplifyVertex_Lock)
		{
			memcpy(&c.positions[k * 3], &original_positions[k * 3], sizeof(float) * 3);
			stats.locked_moved++;
			continue;
		}

		c.moved[k] = 1;
	}

	c.replace.assign(vertex_count, 0);
	c.normals.assign(vertex_count * 3, 0.f);
	c.records.assign(vertex_count * config.simplify_update_record_size, 0);
	c.uvs.assign(relax_remap ? vertex_count * 2 : 0, 0.f);

	clodReplacementCandidate candidate = {};
	candidate.vertex_count = vertex_count;
	candidate.vertex_ids = c.ids.data();
	candidate.positions = c.positions.data();
	candidate.attributes = attributes_stride ? c.attributes.data() : NULL;
	candidate.locks = c.locks.data();
	candidate.moved = c.moved.data();
	candidate.indices = c.indices.data();
	candidate.index_count = c.indices.size();
	candidate.depth = depth;
	candidate.replace = c.replace.data();
	candidate.normals = c.normals.data();
	candidate.records = c.records.empty() ? NULL : c.records.data();
	candidate.uvs = c.uvs.empty() ? NULL : c.uvs.data();
	if (!usable)
		return;
	config.simplify_update_fit(config.simplify_update_context, &mesh, &candidate);

	// only vertices the result uses, never locked ones; every moved one the result uses must be replaced
	std::vector<unsigned char> used(vertex_count, 0);
	for (size_t i = 0; i < c.indices.size(); ++i)
		used[c.indices[i]] = 1;
	for (size_t k = 0; k < vertex_count; ++k)
	{
		if (!used[k] || (c.locks[k] & meshopt_SimplifyVertex_Lock))
			c.replace[k] = 0;
		assert(!(used[k] && c.moved[k]) || c.replace[k]);
	}
}

// Turns an accepted update candidate into the task's result: kept vertices by their id, replaced ones as kReplacementBit | n.
static void acceptUpdate(const clodConfig& config, const UpdateCandidate& c, std::vector<unsigned int>& result, ReplacementOutput& out)
{
	const size_t record_size = config.simplify_update_record_size;
	std::vector<unsigned int> temporary(c.ids.size(), 0);
	out.parents.clear();
	out.records.clear();
	out.moved = 0;
	for (size_t k = 0; k < c.ids.size(); ++k)
	{
		if (!c.replace[k])
			continue;

		temporary[k] = kReplacementBit | unsigned(out.parents.size());
		out.parents.push_back(c.ids[k]);
		out.records.insert(out.records.end(), c.records.begin() + k * record_size, c.records.begin() + (k + 1) * record_size);
		out.moved += c.moved[k] ? 1 : 0;
	}

	result.resize(c.indices.size());
	for (size_t i = 0; i < c.indices.size(); ++i)
	{
		const unsigned int v = c.indices[i];
		result[i] = c.replace[v] ? temporary[v] : c.ids[v];
	}
}

static void fillUpdateView(clodValidateCandidate& candidate, const UpdateCandidate& c)
{
	candidate.output_indices = c.indices.data();
	candidate.output_index_count = c.indices.size();
	candidate.output_positions = c.positions.data();
	candidate.output_normals = c.normals.data();
	candidate.output_vertex_count = c.ids.size();
	candidate.output_uvs = c.uvs.empty() ? NULL : c.uvs.data();
	candidate.output_vertex_ids = c.ids.data();
}

// The error a candidate reports when an independent measurement is configured: the larger of its own and the measured one.
static void measureCandidate(const clodConfig& config, const clodMesh& mesh, const clodValidateCandidate& candidate, float* error, ReplacementTaskStats& stats)
{
	if (!config.simplify_measure)
		return;

	const float measured = config.simplify_measure(config.simplify_update_context, &mesh, &candidate);
	stats.measured++;
	if (measured > *error)
	{
		stats.measured_raised++;
		*error = measured;
	}
}

// Appends one group's replacement vertices (serially, in group order) and gives them ids: extends the position remap and the
// locks, and rewrites the task's result from temporary to final ids.
static void commitReplacements(const clodConfig& config, clodMesh& mesh, std::vector<unsigned char>& locks, std::vector<unsigned int>& remap, const ReplacementOutput& replacements, std::vector<unsigned int>& simplified)
{
	ZoneScopedN("clusterlod::commitReplacements");
	const size_t first = mesh.vertex_count;
	const size_t count = replacements.parents.size();
	assert(first + count < kReplacementBit);

	clodReplacementBatch batch = {};
	batch.count = count;
	batch.first_id = first;
	batch.parents = replacements.parents.data();
	batch.records = replacements.records.empty() ? NULL : replacements.records.data();
	config.simplify_update_commit(config.simplify_update_context, &mesh, &batch);
	assert(mesh.vertex_count == first + count);

	// Position remap: a replacement that kept its parent's position shares the parent's (an unlocked vertex's position is used by
	// this group only); moved ones share among themselves (the wedges of one moved position move together).
	const size_t positions_stride = mesh.vertex_positions_stride / sizeof(float);
	remap.resize(first + count);
	locks.resize(first + count, 0);

	struct Moved
	{
		unsigned int bits[3];
		unsigned int id;
	};
	std::vector<Moved> moved;
	moved.reserve(count);
	for (size_t k = 0; k < count; ++k)
	{
		const size_t id = first + k;
		const float* p = &mesh.vertex_positions[id * positions_stride];
		const float* q = &mesh.vertex_positions[replacements.parents[k] * positions_stride];
		remap[id] = unsigned(id);
		if (memcmp(p, q, sizeof(float) * 3) == 0)
		{
			remap[id] = remap[replacements.parents[k]];
			continue;
		}

		Moved m;
		memcpy(m.bits, p, sizeof(m.bits));
		m.id = unsigned(id);
		moved.push_back(m);
	}

	std::sort(moved.begin(), moved.end(), [](const Moved& a, const Moved& b)
	{
		if (a.bits[0] != b.bits[0]) return a.bits[0] < b.bits[0];
		if (a.bits[1] != b.bits[1]) return a.bits[1] < b.bits[1];
		if (a.bits[2] != b.bits[2]) return a.bits[2] < b.bits[2];
		return a.id < b.id;
	});
	for (size_t i = 1; i < moved.size(); ++i)
		if (memcmp(moved[i].bits, moved[i - 1].bits, sizeof(moved[i].bits)) == 0)
			remap[moved[i].id] = remap[moved[i - 1].id];

	// protect bits on attribute seams for permissive mode, as clodBuildEx sets them for the original vertices
	if (mesh.attribute_protect_mask && mesh.vertex_attributes)
	{
		const size_t max_attributes = mesh.vertex_attributes_stride / sizeof(float);
		for (size_t id = first; id < first + count; ++id)
		{
			const unsigned int r = remap[id];
			for (size_t j = 0; j < max_attributes; ++j)
				if (r != id && (mesh.attribute_protect_mask & (1u << j)) && mesh.vertex_attributes[id * max_attributes + j] != mesh.vertex_attributes[r * max_attributes + j])
					locks[id] |= meshopt_SimplifyVertex_Protect;
		}
	}

	for (size_t i = 0; i < simplified.size(); ++i)
		if (simplified[i] & kReplacementBit)
			simplified[i] = unsigned(first + (simplified[i] & ~kReplacementBit));
}

// BR: simplification with replacement vertices and no validation: the update candidate, unless it gets stuck and sloppy is
// enabled, which then runs as in simplify().
// With optimal placement (stage 4) its candidate comes first: it is used when it reaches the target, or when it is stuck within
// simplify_threshold and sloppy is off; otherwise the rest runs as without it.
static std::vector<unsigned int> simplifyWithReplacements(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, size_t target_count, int depth, float* error, int* method, int* replaced, int* replaced_optimal, ReplacementOutput& replacements, ReplacementTaskStats& stats)
{
	ZoneScopedN("clusterlod::simplifyWithReplacements");
	*method = clodSimplifyMethod_None;
	*replaced = 0;
	*replaced_optimal = 0;
	if (target_count > indices.size())
		return indices;

	clodValidateCandidate candidate = {};
	candidate.input_indices = indices.data();
	candidate.input_index_count = indices.size();
	candidate.depth = depth;

	if (config.simplify_optimal)
	{
		OptimalGroupState state;
		UpdateCandidate optimal;
		simplifyUpdate(config, mesh, indices, locks, target_count, depth, optimal, stats, &state);
		const bool within = !(optimal.indices.size() > indices.size() * config.simplify_threshold);
		if (optimal.indices.size() <= target_count || (within && !config.simplify_fallback_sloppy))
		{
			std::vector<unsigned int> result;
			*error = optimal.error;
			*method = clodSimplifyMethod_Regular;
			limitErrorByEdgeLength(config, mesh, indices, error);
			if (within)
			{
				fillUpdateView(candidate, optimal);
				candidate.method = *method;
				measureCandidate(config, mesh, candidate, error, stats);
				stats.offered++;
				stats.optimal_offered++;
			}
			acceptUpdate(config, optimal, result, replacements);
			*replaced = 1;
			*replaced_optimal = 1;
			return result;
		}
	}

	UpdateCandidate update;
	const UpdateFitGate gate = { config.simplify_fallback_sloppy ? target_count : SIZE_MAX, SIZE_MAX };
	simplifyUpdate(config, mesh, indices, locks, target_count, depth, update, stats, NULL, NULL, NULL, false, &gate);

	std::vector<unsigned int> result;
	if (update.indices.size() > target_count && config.simplify_fallback_sloppy)
	{
		simplifyFallback(result, mesh, indices, locks, target_count, error);
		*error *= config.simplify_error_factor_sloppy;
		*method = clodSimplifyMethod_Sloppy;
		limitErrorByEdgeLength(config, mesh, indices, error);
		candidate.output_indices = result.data();
		candidate.output_index_count = result.size();
		candidate.method = *method;
		if (!(result.size() > indices.size() * config.simplify_threshold))
			measureCandidate(config, mesh, candidate, error, stats);
		return result;
	}

	*error = update.error;
	*method = clodSimplifyMethod_Regular;
	limitErrorByEdgeLength(config, mesh, indices, error);
	if (!(update.indices.size() > indices.size() * config.simplify_threshold))
	{
		fillUpdateView(candidate, update);
		candidate.method = *method;
		measureCandidate(config, mesh, candidate, error, stats);
		stats.offered++;
	}
	acceptUpdate(config, update, result, replacements);
	*replaced = 1;
	return result;
}

// BR: validated simplification. Candidates, in order, for each target from the configured one towards simplify_threshold:
// 1. the attribute-aware result (regular, or the permissive fallback), when it reaches the target;
// 2. otherwise (stuck) the sloppy result at that target, if sloppy is enabled;
// 3. then the stuck attribute-aware result itself, if it is under simplify_threshold.
// The first candidate the callback accepts is returned. Returns false when none was accepted; *outcome tells whether any was
// rejected (Rejected) or none was even offered (None, the ordinary terminal case).
// With replacement vertices (simplify_update_fit), each target first offers the update candidate, then the ones above; with
// optimal placement (simplify_optimal, stage 4) its candidate comes before the update candidate.
static bool simplifyValidated(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, size_t target_count, float child_error, int depth, int group, std::vector<unsigned int>& result, float* result_error, int* result_method, int* outcome, ValidationTaskStats& stats, int* result_replaced, int* result_optimal, ReplacementOutput& replacements, ReplacementTaskStats& replacement_stats)
{
	ZoneScopedN("clusterlod::simplifyValidated");
	*outcome = clodSimplifyValidation_None;
	*result_replaced = 0;
	*result_optimal = 0;
	if (target_count > indices.size())
		return false;

	const auto withinThreshold = [&](size_t size)
	{
		// the same test runIterationTask applies to decide a group is stuck
		return size >= 3 && size % 3 == 0 && !(size > indices.size() * config.simplify_threshold);
	};

	const size_t threshold_count = size_t((indices.size() / 3) * config.simplify_threshold) * 3;
	const int retries = std::max(0, config.simplify_validate_retries);

	int attempt = 0;
	bool any_rejected = false;
	size_t rejected_stuck_size = 0; // a stuck attribute-aware result already rejected; the same size again is the same result
	size_t rejected_update_size = 0; // the same for a stuck update candidate
	size_t rejected_optimal_size = 0; // the same for a stuck optimal-placement candidate
	OptimalGroupState optimal_state;

	// update is the update candidate (lod is then unused), or null
	const auto offer = [&](std::vector<unsigned int>& lod, float error, int method, const UpdateCandidate* update) -> bool
	{
		if (!withinThreshold(update ? update->indices.size() : lod.size()))
			return false;

		limitErrorByEdgeLength(config, mesh, indices, &error);

		clodValidateCandidate candidate = {};
		candidate.input_indices = indices.data();
		candidate.input_index_count = indices.size();
		candidate.output_indices = lod.data();
		candidate.output_index_count = lod.size();
		if (update)
		{
			fillUpdateView(candidate, *update);
			replacement_stats.offered++;
			replacement_stats.optimal_offered += update->optimal ? 1 : 0;
		}
		candidate.method = method;
		candidate.depth = depth;
		candidate.group = group;
		candidate.error = std::max(child_error * config.simplify_error_merge_previous, error) + error * config.simplify_error_merge_additive;
		measureCandidate(config, mesh, candidate, &error, replacement_stats);
		candidate.error = std::max(child_error * config.simplify_error_merge_previous, error) + error * config.simplify_error_merge_additive;
		candidate.attempt = attempt++;

		stats.candidates++;
		const unsigned int reasons = config.simplify_validate(config.simplify_validate_context, &mesh, &candidate);

		if (method == clodSimplifyMethod_Sloppy)
			(reasons ? stats.sloppy_rejected : stats.sloppy_accepted)++;

		if (reasons == 0)
		{
			if (update)
			{
				acceptUpdate(config, *update, result, replacements);
				*result_replaced = 1;
				*result_optimal = update->optimal ? 1 : 0;
			}
			else
			{
				result.swap(lod);
			}
			*result_error = error;
			*result_method = method;
			*outcome = any_rejected ? clodSimplifyValidation_AcceptedAfterRejection : clodSimplifyValidation_Accepted;
			return true;
		}

		any_rejected = true;
		stats.rejected++;
		for (int bit = 0; bit < 5; ++bit)
			if (reasons & (1u << bit))
				stats.rejected_reason[bit]++;
		return false;
	};

	for (int retry = 0; retry <= retries; ++retry)
	{
		size_t target = target_count;
		if (retry > 0)
		{
			if (threshold_count <= target_count)
				break;
			target = target_count + ((threshold_count - target_count) / 3 * size_t(retry) / size_t(retries + 1)) * 3;
		}

		if (config.simplify_update_fit && config.simplify_optimal)
		{
			UpdateCandidate optimal;
			simplifyUpdate(config, mesh, indices, locks, target, depth, optimal, replacement_stats, &optimal_state);
			const size_t size = optimal.indices.size();
			std::vector<unsigned int> unused;
			if (size != rejected_optimal_size)
			{
				if (offer(unused, optimal.error, clodSimplifyMethod_Regular, &optimal))
					return true;
				if (size > target && withinThreshold(size))
					rejected_optimal_size = size;
			}
		}

		if (config.simplify_update_fit)
		{
			UpdateCandidate update;
			const UpdateFitGate gate = { SIZE_MAX, rejected_update_size };
			simplifyUpdate(config, mesh, indices, locks, target, depth, update, replacement_stats, NULL, NULL, NULL, false, &gate);
			const size_t size = update.indices.size();
			std::vector<unsigned int> unused;
			if (size != rejected_update_size)
			{
				if (offer(unused, update.error, clodSimplifyMethod_Regular, &update))
					return true;
				// stuck above the target: later targets get the same result
				if (size > target && withinThreshold(size))
					rejected_update_size = size;
			}
		}

		float error = 0.f;
		int method = clodSimplifyMethod_Regular;
		std::vector<unsigned int> lod = simplifyAttributeAware(config, mesh, indices, locks, target, &error, &method);

		if (lod.size() <= target)
		{
			if (offer(lod, error, method, NULL))
				return true;
			continue;
		}

		// stuck above the threshold with nothing else to try: later targets get the same result
		if (!config.simplify_fallback_sloppy && lod.size() > threshold_count)
			break;

		if (config.simplify_fallback_sloppy)
		{
			std::vector<unsigned int> sloppy;
			float sloppy_error = 0.f;
			simplifyFallback(sloppy, mesh, indices, locks, target, &sloppy_error);
			sloppy_error *= config.simplify_error_factor_sloppy;
			if (offer(sloppy, sloppy_error, clodSimplifyMethod_Sloppy, NULL))
				return true;
		}

		if (lod.size() != rejected_stuck_size)
		{
			const size_t size = lod.size();
			if (offer(lod, error, method, NULL))
				return true;
			if (withinThreshold(size))
				rejected_stuck_size = size;
		}
	}

	if (any_rejected)
		*outcome = clodSimplifyValidation_Rejected;
	return false;
}

// BR (stage 5): topology candidates for a group that would otherwise be terminal (clodConfig::simplify_topology). For each target
// from the configured one towards simplify_threshold: an update candidate (optimal placement when configured, then with
// simplify_topology_update_fallback meshoptimizer's) whose relaxable seams lose their protect bits, so meshoptimizer may collapse
// across them; its shading is refitted from the original like any update candidate's. Each is validated (simplify_validate, or simplify_topology when validation is off); the first accepted is
// returned. False when the group has no relaxable seam, every candidate stays stuck, or every one is rejected (*any_rejected).
static bool simplifyTopology(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, const std::vector<unsigned int>& remap, size_t target_count, float child_error, int depth, int group, std::vector<unsigned int>& result, float* result_error, int* result_optimal, bool* any_rejected, ReplacementOutput& replacements, ReplacementTaskStats& replacement_stats, TopologyTaskStats& stats)
{
	ZoneScopedN("clusterlod::simplifyTopology");
	*any_rejected = false;
	*result_optimal = 0;
	if (target_count > indices.size() || !config.simplify_update_fit)
		return false;

	bool relaxable = false;
	for (size_t i = 0; i < indices.size() && !relaxable; ++i)
		relaxable = (locks[indices[i]] & meshopt_SimplifyVertex_Protect) && relaxableSeam(config, mesh, remap, indices[i]);
	if (!relaxable)
		return false;
	stats.groups++;

	const clodValidateSimplify validate = config.simplify_validate ? config.simplify_validate : config.simplify_topology;
	const size_t threshold_count = size_t((indices.size() / 3) * config.simplify_threshold) * 3;
	const int retries = std::max(0, config.simplify_topology_retries);
	OptimalGroupState optimal_state;
	// per kind of candidate (0 meshoptimizer's loop, 1 the optimal loop): the size of a stuck result already rejected, and stuck
	size_t rejected_size[2] = {0, 0};
	bool kind_stuck[2] = {false, false};
	const bool use_optimal = config.simplify_optimal != NULL;
	const bool use_update = !use_optimal || config.simplify_topology_update_fallback; // BR (follow-up)
	int attempt = 0;

	for (int retry = 0; retry <= retries; ++retry)
	{
		size_t target = target_count;
		if (retry > 0)
		{
			if (threshold_count <= target_count)
				break;
			target = target_count + ((threshold_count - target_count) / 3 * size_t(retry) / size_t(retries + 1)) * 3;
		}

		for (int kind = use_optimal ? 1 : 0; kind >= (use_update ? 0 : 1); --kind)
		{
			if (kind_stuck[kind])
				continue;
			UpdateCandidate c;
			size_t relaxed = 0;
			const UpdateFitGate gate = { SIZE_MAX, rejected_size[kind] };
			simplifyUpdate(config, mesh, indices, locks, target, depth, c, replacement_stats, kind ? &optimal_state : NULL, &remap, &relaxed, false, kind ? NULL : &gate);
			const size_t size = c.indices.size();
			if (size < 3 || size % 3 != 0 || size > indices.size() * config.simplify_threshold)
			{
				// still stuck: less aggressive targets get the same result
				stats.stuck++;
				kind_stuck[kind] = true;
				continue;
			}
			if (size == rejected_size[kind])
				continue;

			float error = c.error;
			limitErrorByEdgeLength(config, mesh, indices, &error);

			clodValidateCandidate candidate = {};
			candidate.input_indices = indices.data();
			candidate.input_index_count = indices.size();
			fillUpdateView(candidate, c);
			candidate.method = clodSimplifyMethod_Topology;
			candidate.depth = depth;
			candidate.group = group;
			candidate.error = std::max(child_error * config.simplify_error_merge_previous, error) + error * config.simplify_error_merge_additive;
			measureCandidate(config, mesh, candidate, &error, replacement_stats);
			candidate.error = std::max(child_error * config.simplify_error_merge_previous, error) + error * config.simplify_error_merge_additive;
			candidate.attempt = attempt++;
			replacement_stats.offered++;
			replacement_stats.optimal_offered += c.optimal ? 1 : 0;
			stats.offered++;

			const unsigned int reasons = validate ? validate(config.simplify_validate_context, &mesh, &candidate) : 0;
			if (reasons == 0)
			{
				acceptUpdate(config, c, result, replacements);
				*result_error = error;
				*result_optimal = c.optimal ? 1 : 0;
				stats.relaxed = relaxed;
				return true;
			}

			*any_rejected = true;
			stats.rejected++;
			for (int bit = 0; bit < 6; ++bit)
				if (reasons & (1u << bit))
					stats.rejected_reason[bit]++;
			if (size > target)
				rejected_size[kind] = size;
		}

		if ((!use_optimal || kind_stuck[1]) && (!use_update || kind_stuck[0]))
			break;
	}

	return false;
}

struct IterationTaskResult
{
	bool computed = false;
	bool terminal = false;
	clodBounds bounds{};
	std::vector<unsigned int> simplified;
	unsigned int threadIndex = 0;
	int simplifyMethod = 0; // clodSimplifyMethod; None for terminal groups
	int simplifyValidation = 0; // clodSimplifyValidation
	ValidationTaskStats validation;
	int replaced = 0; // simplified came from an update candidate; replacements holds its new vertices
	int replacedOptimal = 0; // stage 4: that update candidate was an optimal-placement one
	ReplacementOutput replacements;
	ReplacementTaskStats replacementStats;
	int topology = 0; // stage 5: simplified came from a topology candidate
	TopologyTaskStats topologyStats;
	int lastResort = 0; // simplified came from the last-resort sloppy fallback (simplify_last_resort_sloppy)
};

struct IterationContext
{
	clodConfig config{};
	clodMesh mesh{};
	const std::vector<std::vector<int> >* groups = NULL;
	const std::vector<unsigned char>* locks = NULL;
	const std::vector<unsigned int>* remap = NULL; // BR: the position remap (stage 5)
	std::vector<Cluster>* clusters = NULL;
	std::vector<IterationTaskResult>* taskResults = NULL;
	int depth = 0;
};

static int outputGroupEx(const clodConfig& config, const clodMesh& mesh, const std::vector<Cluster>& clusters, const std::vector<int>& group, const clodBounds& simplified, int depth, int simplify_method, int simplify_validation, int simplify_replaced, void* output_context, clodOutputEx output_callback, size_t task_index, unsigned int thread_index)
{
	ZoneScopedN("clusterlod::outputGroupEx");
	TracyPlot("CLOD.clodBuild.outputGroupEx.Clusters", static_cast<int64_t>(group.size()));
	std::vector<clodCluster> group_clusters(group.size());

	for (size_t i = 0; i < group.size(); ++i)
	{
		const Cluster& cluster = clusters[group[i]];
		clodCluster& result = group_clusters[i];

		result.refined = cluster.refined;
		result.bounds = (config.optimize_bounds && cluster.refined != -1) ? boundsCompute(mesh, cluster.indices, cluster.bounds.error) : cluster.bounds;
		result.indices = cluster.indices.data();
		result.index_count = cluster.indices.size();
		result.vertex_count = cluster.vertices;
	}

	return output_callback ? output_callback(output_context, {depth, simplified, simplify_method, simplify_validation, simplify_replaced}, group_clusters.data(), group_clusters.size(), task_index, thread_index) : -1;
}

static void runIterationTask(IterationContext& context, size_t task_index, unsigned int thread_index)
{
	ZoneScopedN("clusterlod::runIterationTask");
	assert(context.groups != NULL && context.locks != NULL && context.clusters != NULL && context.taskResults != NULL);
	assert(task_index < context.groups->size());

	IterationTaskResult& taskResult = (*context.taskResults)[task_index];
	const std::vector<int>& group = (*context.groups)[task_index];
	const std::vector<Cluster>& clusters = *context.clusters;

	std::vector<unsigned int> merged;
	{
		ZoneScopedN("clusterlod::runIterationTask::MergeIndices");
		merged.reserve(group.size() * context.config.max_triangles * 3);
		for (size_t j = 0; j < group.size(); ++j)
			merged.insert(merged.end(), clusters[group[j]].indices.begin(), clusters[group[j]].indices.end());
	}

	size_t target_size = size_t((merged.size() / 3) * context.config.simplify_ratio) * 3;
	if (!merged.empty())
		target_size = std::max<size_t>(3, target_size);

	{
		ZoneScopedN("clusterlod::runIterationTask::MergeBounds");
		taskResult.bounds = mergeGroups(clusters, group);
	}

	float error = 0.f;
	int simplifyMethod = clodSimplifyMethod_None;
	std::vector<unsigned int> simplified;
	taskResult.simplifyValidation = clodSimplifyValidation_None;
	taskResult.validation = ValidationTaskStats();
	taskResult.replaced = 0;
	taskResult.replacedOptimal = 0;
	taskResult.replacements = ReplacementOutput();
	taskResult.replacementStats = ReplacementTaskStats();
	taskResult.topology = 0;
	taskResult.topologyStats = TopologyTaskStats();
	if (context.config.simplify_validate)
	{
		// a group nothing validated for keeps its finer geometry: the empty result makes it terminal below
		simplifyValidated(context.config, context.mesh, merged, *context.locks, target_size, taskResult.bounds.error, context.depth, int(task_index), simplified, &error, &simplifyMethod, &taskResult.simplifyValidation, taskResult.validation,
		    &taskResult.replaced, &taskResult.replacedOptimal, taskResult.replacements, taskResult.replacementStats);
	}
	else if (context.config.simplify_update_fit && !context.config.simplify_update_fit_last_resort_only)
	{
		simplified = simplifyWithReplacements(context.config, context.mesh, merged, *context.locks, target_size, context.depth, &error, &simplifyMethod, &taskResult.replaced, &taskResult.replacedOptimal, taskResult.replacements, taskResult.replacementStats);
	}
	else
	{
		simplified = simplify(context.config, context.mesh, merged, *context.locks, target_size, &error, &simplifyMethod);
	}
	bool invalidSimplifiedTopology = !simplified.empty() && (simplified.size() % 3) != 0;
	bool emptyOrDegenerateSimplified = !merged.empty() && simplified.size() < 3;
	bool terminal = simplified.size() > merged.size() * context.config.simplify_threshold || invalidSimplifiedTopology || emptyOrDegenerateSimplified;

	// BR (stage 5): a group about to be terminal tries topology candidates
	if (terminal && context.config.simplify_topology && context.remap)
	{
		std::vector<unsigned int> topology;
		float topologyError = 0.f;
		int topologyOptimal = 0;
		bool topologyRejected = false;
		ReplacementOutput topologyReplacements;
		if (simplifyTopology(context.config, context.mesh, merged, *context.locks, *context.remap, target_size, taskResult.bounds.error, context.depth, int(task_index), topology, &topologyError, &topologyOptimal,
		        &topologyRejected, topologyReplacements, taskResult.replacementStats, taskResult.topologyStats))
		{
			const bool rejectedBefore = taskResult.simplifyValidation == clodSimplifyValidation_Rejected || taskResult.validation.rejected > 0;
			simplified.swap(topology);
			error = topologyError;
			simplifyMethod = clodSimplifyMethod_Topology;
			taskResult.simplifyValidation = (rejectedBefore || topologyRejected) ? clodSimplifyValidation_AcceptedAfterRejection : clodSimplifyValidation_Accepted;
			taskResult.replaced = 1;
			taskResult.replacedOptimal = topologyOptimal;
			taskResult.replacements = std::move(topologyReplacements);
			taskResult.topology = 1;
			invalidSimplifiedTopology = false;
			emptyOrDegenerateSimplified = false;
			terminal = false;
		}
		else if (topologyRejected && taskResult.simplifyValidation == clodSimplifyValidation_None)
		{
			taskResult.simplifyValidation = clodSimplifyValidation_Rejected;
		}
	}

	// BR (stage 5, diagnostics): why a group about to be terminal is stuck, classified before the last resort may take it, and
	// for groups validation rejected too (their class is what kept the ordinary simplifier from the threshold)
	if (terminal && context.config.topology_stats && context.config.topology_diagnostics && !invalidSimplifiedTopology && context.remap)
		diagnoseStuckGroup(context.config, context.mesh, merged, *context.locks, *context.remap, target_size, taskResult.topologyStats);

	// BR: last-resort sloppy. A stuck group is simplified by the sloppy fallback rather than left terminal, so every group gets
	// a parent; its error is the sloppy result's own (scaled as the global fallback scales it), never FLT_MAX.
	taskResult.lastResort = 0;
	if (terminal && context.config.simplify_last_resort_sloppy && !context.config.simplify_fallback_sloppy && !context.config.simplify_validate &&
	    !merged.empty() && (context.config.simplify_last_resort_rejected || taskResult.simplifyValidation != clodSimplifyValidation_Rejected))
	{
		std::vector<unsigned int> sloppy;
		float sloppyError = 0.f;
		simplifyFallback(sloppy, context.mesh, merged, *context.locks, target_size, &sloppyError);
		sloppyError *= context.config.simplify_error_factor_last_resort > 0.f ? context.config.simplify_error_factor_last_resort : context.config.simplify_error_factor_sloppy;
		limitErrorByEdgeLength(context.config, context.mesh, merged, &sloppyError);
		if (sloppy.size() >= 3 && sloppy.size() % 3 == 0 && !(sloppy.size() > merged.size() * context.config.simplify_threshold))
		{
			simplified.swap(sloppy);
			error = sloppyError;
			simplifyMethod = clodSimplifyMethod_Sloppy;
			taskResult.replaced = 0;
			taskResult.replacedOptimal = 0;
			taskResult.replacements = ReplacementOutput();
			taskResult.topology = 0;
			taskResult.lastResort = 1;
			if (context.config.simplify_last_resort_fit && context.config.simplify_update_fit)
			{
				UpdateCandidate fitted;
				ReplacementTaskStats fitStats;
				simplifyUpdate(context.config, context.mesh, simplified, *context.locks, simplified.size(), context.depth, fitted, fitStats, NULL, NULL, NULL, true);
				acceptUpdate(context.config, fitted, simplified, taskResult.replacements);
				taskResult.replaced = taskResult.replacements.parents.empty() ? 0 : 1;
			}
			invalidSimplifiedTopology = false;
			emptyOrDegenerateSimplified = false;
			terminal = false;
		}
	}

	if (terminal)
	{
		taskResult.terminal = true;
		taskResult.bounds.error = FLT_MAX;
		taskResult.simplified.clear();
		taskResult.simplifyMethod = clodSimplifyMethod_None;
		taskResult.replaced = 0;
		taskResult.replacedOptimal = 0;
		taskResult.replacements = ReplacementOutput();
		taskResult.threadIndex = thread_index;
		taskResult.computed = true;
		return;
	}

	taskResult.terminal = false;
	taskResult.bounds.error = std::max(taskResult.bounds.error * context.config.simplify_error_merge_previous, error) + error * context.config.simplify_error_merge_additive;
	taskResult.simplified = std::move(simplified);
	taskResult.simplifyMethod = simplifyMethod;
	taskResult.threadIndex = thread_index;
	taskResult.computed = true;
}

static clodNode mergeNodes(const clodNode* nodes, size_t offset, size_t count)
{
	clodNode result = {};
	result.bounds = boundsMerge(&nodes[offset].bounds, count, sizeof(clodNode));
	result.group = -1;
	result.child_offset = unsigned(offset);
	result.child_count = unsigned(count);
	return result;
}

} // namespace clod

clodConfig clodDefaultConfig(size_t max_triangles)
{
	assert(max_triangles >= 4 && max_triangles <= 256);

	clodConfig config = {};
	config.max_vertices = max_triangles;
	config.min_triangles = max_triangles / 3;
	config.max_triangles = max_triangles;

#if MESHOPTIMIZER_VERSION < 1000
	config.min_triangles &= ~3; // account for 4b alignment
#endif

	config.partition_spatial = true;
	config.partition_size = 16;
	config.partition_max_refined_groups = 0;
	config.partition_refined_split_count = NULL;

	config.cluster_spatial = false;
	config.cluster_split_factor = 2.0f;

	config.optimize_clusters = true;
	config.optimize_clusters_level = 1;

	config.simplify_ratio = 0.5f;
	config.simplify_threshold = 0.85f;
	config.simplify_error_merge_previous = 1.0f;
	config.simplify_error_factor_sloppy = 2.0f;
	config.simplify_permissive = true;
	config.simplify_fallback_permissive = false; // note: by default we run in permissive mode, but it's also possible to disable that and use it only as a fallback
	config.simplify_fallback_sloppy = true;
	config.simplify_last_resort_sloppy = false;
	config.simplify_last_resort_rejected = true;
	config.simplify_last_resort_fit = false;
	config.simplify_error_factor_last_resort = 0.f;
	config.simplify_update_fit_last_resort_only = false;

	return config;
}

clodConfig clodDefaultConfigRT(size_t max_triangles)
{
	clodConfig config = clodDefaultConfig(max_triangles);

	// for ray tracing, we may want smaller clusters when that improves BVH quality further; for maximum ray tracing performance this could be reduced even further
	config.min_triangles = max_triangles / 4;

	// by default, we use larger max_vertices for RT; the vertex count is not important for ray tracing performance, and this helps improve cluster utilization
	config.max_vertices = std::min(size_t(256), max_triangles * 2);

	config.cluster_spatial = true;
	config.cluster_fill_weight = 0.5f;

	return config;
}

size_t clodBuildEx(clodConfig config, clodMesh mesh, void* output_context, clodOutputEx output_callback, const clodBuildParallelConfig* parallel_config)
{
	ZoneScopedN("clusterlod::clodBuildEx");
	TracyPlot("CLOD.clodBuild.InputVertices", static_cast<int64_t>(mesh.vertex_count));
	TracyPlot("CLOD.clodBuild.InputIndices", static_cast<int64_t>(mesh.index_count));
	using namespace clod;

	const bool missingIndices = mesh.index_count > 0 && mesh.indices == NULL;
	const bool missingPositions = mesh.vertex_count > 0 && mesh.vertex_positions == NULL;
	const bool missingAttributes = mesh.attribute_count > 0 && mesh.vertex_attributes == NULL;
	const bool emptyGeometry = mesh.index_count == 0 || mesh.vertex_count == 0;
	const bool invalidPositionStride = mesh.vertex_positions_stride < sizeof(float) * 3;
	const bool invalidAttributeStride = mesh.attribute_count > 0 && mesh.vertex_attributes_stride < mesh.attribute_count * sizeof(float);

	if (emptyGeometry || missingIndices || missingPositions || missingAttributes || invalidPositionStride || invalidAttributeStride)
	{
		fprintf(stderr,
		    "clusterlod: skipping mesh with invalid or empty geometry (index_count=%zu, vertex_count=%zu, indices=%p, positions=%p, position_stride=%zu, attribute_count=%zu, attributes=%p, attribute_stride=%zu)\n",
		    mesh.index_count,
		    mesh.vertex_count,
		    static_cast<const void*>(mesh.indices),
		    static_cast<const void*>(mesh.vertex_positions),
		    mesh.vertex_positions_stride,
		    mesh.attribute_count,
		    static_cast<const void*>(mesh.vertex_attributes),
		    mesh.vertex_attributes_stride);
		return 0;
	}

	assert(mesh.vertex_attributes_stride % sizeof(float) == 0);
	assert(mesh.attribute_count * sizeof(float) <= mesh.vertex_attributes_stride);
	assert(mesh.attribute_protect_mask < (1u << (mesh.vertex_attributes_stride / sizeof(float))));

	std::vector<unsigned char> locks(mesh.vertex_count);

	// for cluster connectivity, we need a position-only remap that maps vertices with the same position to the same index
	std::vector<unsigned int> remap(mesh.vertex_count);
	{
		ZoneScopedN("clusterlod::clodBuildEx::GeneratePositionRemap");
		meshopt_generatePositionRemap(&remap[0], mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride);
	}

	// set up protect bits on UV seams for permissive mode
	if (mesh.attribute_protect_mask)
	{
		ZoneScopedN("clusterlod::clodBuildEx::ProtectAttributeSeams");
		size_t max_attributes = mesh.vertex_attributes_stride / sizeof(float);

		for (size_t i = 0; i < mesh.vertex_count; ++i)
		{
			unsigned int r = remap[i]; // canonical vertex with the same position

			for (size_t j = 0; j < max_attributes; ++j)
				if (r != i && (mesh.attribute_protect_mask & (1u << j)) && mesh.vertex_attributes[i * max_attributes + j] != mesh.vertex_attributes[r * max_attributes + j])
					locks[i] |= meshopt_SimplifyVertex_Protect;
		}
	}

	// initial clusterization splits the original mesh
	std::vector<Cluster> clusters;
	{
		ZoneScopedN("clusterlod::clodBuildEx::InitialClusterize");
		clusters = clusterize(config, mesh, mesh.indices, mesh.index_count);
	}
	TracyPlot("CLOD.clodBuild.InitialClusters", static_cast<int64_t>(clusters.size()));

	// compute initial precise bounds; subsequent bounds will be using group-merged bounds
	{
		ZoneScopedN("clusterlod::clodBuildEx::InitialBounds");
		for (Cluster& cluster : clusters)
			cluster.bounds = boundsCompute(mesh, cluster.indices, 0.f);
	}

	std::vector<int> pending(clusters.size());
	for (size_t i = 0; i < clusters.size(); ++i)
		pending[i] = int(i);

	int depth = 0;

	// merge and simplify clusters until we can't merge anymore
	while (pending.size() > 1)
	{
		ZoneScopedN("clusterlod::clodBuildEx::DepthIteration");
		TracyPlot("CLOD.clodBuild.Depth", static_cast<int64_t>(depth));
		TracyPlot("CLOD.clodBuild.PendingClusters", static_cast<int64_t>(pending.size()));
		std::vector<std::vector<int> > groups;
		{
			ZoneScopedN("clusterlod::clodBuildEx::Partition");
			groups = partition(config, mesh, clusters, pending, remap);
		}
		TracyPlot("CLOD.clodBuild.Groups", static_cast<int64_t>(groups.size()));

		pending.clear();

		// mark boundaries between groups with a lock bit to avoid gaps in simplified result
		{
			ZoneScopedN("clusterlod::clodBuildEx::LockBoundary");
			lockBoundary(locks, groups, clusters, remap, mesh.vertex_lock);
		}

		std::vector<IterationTaskResult> taskResults(groups.size());
		IterationContext iterationContext{};
		iterationContext.config = config;
		iterationContext.mesh = mesh;
		iterationContext.groups = &groups;
		iterationContext.locks = &locks;
		iterationContext.remap = &remap;
		iterationContext.clusters = &clusters;
		iterationContext.taskResults = &taskResults;
		iterationContext.depth = depth;

		// BR: the level's whole surface, for validation that needs to know which group owns which surface
		if ((config.simplify_validate || config.simplify_topology) && config.simplify_validate_level)
		{
			ZoneScopedN("clusterlod::clodBuildEx::ValidateLevel");
			std::vector<unsigned int> level_indices;
			std::vector<unsigned int> level_groups;
			for (size_t i = 0; i < groups.size(); ++i)
				for (int c : groups[i])
				{
					level_indices.insert(level_indices.end(), clusters[c].indices.begin(), clusters[c].indices.end());
					level_groups.insert(level_groups.end(), clusters[c].indices.size() / 3, unsigned(i));
				}
			config.simplify_validate_level(config.simplify_validate_context, &mesh, depth, level_indices.data(), level_groups.data(), level_indices.size());
		}

		if (parallel_config && parallel_config->iteration_callback)
		{
			ZoneScopedN("clusterlod::clodBuildEx::IterationCallback");
			parallel_config->iteration_callback(&iterationContext, output_context, depth, groups.size());
		}
		else
		{
			ZoneScopedN("clusterlod::clodBuildEx::RunIterationTasksSerial");
			for (size_t i = 0; i < groups.size(); ++i)
			{
				runIterationTask(iterationContext, i, 0);
			}
		}

		{
			ZoneScopedN("clusterlod::clodBuildEx::RunMissingIterationTasks");
			for (size_t i = 0; i < groups.size(); ++i)
			{
				if (!taskResults[i].computed)
				{
					runIterationTask(iterationContext, i, 0);
				}
			}
		}

		// BR (build cost): every accepted result's replacement vertices are committed first, in group order (a commit only appends,
		// and outputs read only the vertices their groups already had), so the results can be split into clusters side by side;
		// the outputs then run in group order as before.
		std::vector<std::vector<Cluster> > splits(groups.size());
		{
			ZoneScopedN("clusterlod::clodBuildEx::CommitAndSplitGroups");
			for (size_t i = 0; i < groups.size(); ++i)
			{
				IterationTaskResult& taskResult = taskResults[i];
				if (!taskResult.terminal && taskResult.replaced && !taskResult.replacements.parents.empty())
					commitReplacements(config, mesh, locks, remap, taskResult.replacements, taskResult.simplified);
			}
			struct SplitContext
			{
				const clodConfig* config;
				const clodMesh* mesh;
				std::vector<IterationTaskResult>* taskResults;
				std::vector<std::vector<Cluster> >* splits;
				static void run(void* context, size_t i)
				{
					SplitContext& self = *static_cast<SplitContext*>(context);
					IterationTaskResult& taskResult = (*self.taskResults)[i];
					if (taskResult.terminal)
						return;
					ZoneScopedN("clusterlod::clodBuildEx::SplitSimplifiedGroup");
					(*self.splits)[i] = clusterize(*self.config, *self.mesh, taskResult.simplified.data(), taskResult.simplified.size());
				}
			};
			SplitContext splitContext = { &config, &mesh, &taskResults, &splits };
			if (parallel_config && parallel_config->parallel_for)
				parallel_config->parallel_for(parallel_config->parallel_for_context, groups.size(), &SplitContext::run, &splitContext);
			else
				for (size_t i = 0; i < groups.size(); ++i)
					SplitContext::run(&splitContext, i);
		}

		{
			ZoneScopedN("clusterlod::clodBuildEx::OutputAndSplitGroups");
			for (size_t i = 0; i < groups.size(); ++i)
			{
				const std::vector<int>& group = groups[i];
				IterationTaskResult& taskResult = taskResults[i];

				if (clodValidationStats* stats = config.simplify_validate_stats)
				{
					const ValidationTaskStats& task = taskResult.validation;
					stats->candidates += task.candidates;
					stats->rejected += task.rejected;
					for (int bit = 0; bit < 5; ++bit)
						stats->rejected_reason[bit] += task.rejected_reason[bit];
					stats->sloppy_accepted += task.sloppy_accepted;
					stats->sloppy_rejected += task.sloppy_rejected;
					stats->accepted += (taskResult.simplifyValidation == clodSimplifyValidation_Accepted || taskResult.simplifyValidation == clodSimplifyValidation_AcceptedAfterRejection) ? 1 : 0;
					stats->groups_first += taskResult.simplifyValidation == clodSimplifyValidation_Accepted ? 1 : 0;
					stats->groups_retried += taskResult.simplifyValidation == clodSimplifyValidation_AcceptedAfterRejection ? 1 : 0;
					stats->groups_terminal += taskResult.simplifyValidation == clodSimplifyValidation_Rejected ? 1 : 0;
				}

				if (clodReplacementStats* stats = config.simplify_update_stats)
				{
					const ReplacementTaskStats& task = taskResult.replacementStats;
					stats->candidates += task.candidates;
					stats->offered += task.offered;
					stats->locked_moved += task.locked_moved;
					stats->measured += task.measured;
					stats->measured_raised += task.measured_raised;
					stats->optimal_candidates += task.optimal_candidates;
					stats->optimal_offered += task.optimal_offered;
				}

				if (clodTopologyStats* stats = config.topology_stats)
				{
					const TopologyTaskStats& task = taskResult.topologyStats;
					size_t triangles = 0;
					for (int c : group)
						triangles += clusters[c].indices.size() / 3;
					stats->topology_groups += task.groups;
					stats->topology_offered += task.offered;
					stats->topology_rejected += task.rejected;
					for (int bit = 0; bit < 6; ++bit)
						stats->topology_rejected_reason[bit] += task.rejected_reason[bit];
					stats->topology_stuck += task.stuck;
					if (taskResult.terminal && taskResult.simplifyValidation == clodSimplifyValidation_Rejected)
					{
						stats->rejected++;
						stats->rejected_triangles += triangles;
						if (task.stuck_class >= 0)
						{
							stats->rejected_class[task.stuck_class]++;
							stats->rejected_class_triangles[task.stuck_class] += triangles;
						}
					}
					else if (taskResult.terminal)
					{
						stats->stuck++;
						stats->stuck_triangles += triangles;
						if (task.stuck_class >= 0)
						{
							stats->stuck_class[task.stuck_class]++;
							stats->stuck_class_triangles[task.stuck_class] += triangles;
						}
						stats->stuck_components += task.components;
						stats->stuck_small_triangles += task.small_triangles;
						stats->stuck_boundary_vertices += task.boundary_vertices;
						stats->stuck_positions += task.positions;
						stats->stuck_uv_seam_vertices += task.uv_seam;
						stats->stuck_uv_seam_tiny += task.uv_seam_tiny;
						stats->stuck_uv_seam_integer += task.uv_seam_integer;
					}
					else if (taskResult.lastResort)
					{
						stats->last_resort++;
						stats->last_resort_triangles += triangles;
						if (task.stuck_class >= 0)
						{
							stats->last_resort_class[task.stuck_class]++;
							stats->last_resort_class_triangles[task.stuck_class] += triangles;
						}
						if (taskResult.simplifyValidation == clodSimplifyValidation_Rejected)
						{
							stats->last_resort_rejected++;
							stats->last_resort_rejected_triangles += triangles;
						}
					}
					else if (taskResult.topology)
					{
						stats->topology_accepted++;
						stats->topology_accepted_triangles += triangles;
						stats->topology_relaxed_vertices += task.relaxed;
					}
				}

				if (taskResult.terminal)
				{
					outputGroupEx(config, mesh, clusters, group, taskResult.bounds, depth, clodSimplifyMethod_None, taskResult.simplifyValidation, 0, output_context, output_callback, i, taskResult.threadIndex);
					continue;
				}

				const int refined = outputGroupEx(config, mesh, clusters, group, taskResult.bounds, depth, taskResult.simplifyMethod, taskResult.simplifyValidation, taskResult.replaced, output_context, output_callback, i, taskResult.threadIndex);

				// BR: the group's replacement vertices got their ids above, in group order, before its result was split
				if (taskResult.replaced)
				{
					if (clodReplacementStats* stats = config.simplify_update_stats)
					{
						stats->accepted++;
						stats->optimal_accepted += taskResult.replacedOptimal ? 1 : 0;
						stats->vertices += taskResult.replacements.parents.size();
						stats->vertices_moved += taskResult.replacements.moved;
					}
				}

				std::vector<Cluster> split = std::move(splits[i]);
				if (split.empty())
				{
					clodBounds terminalBounds = taskResult.bounds;
					terminalBounds.error = FLT_MAX;
					outputGroupEx(config, mesh, clusters, group, terminalBounds, depth, clodSimplifyMethod_None, clodSimplifyValidation_None, 0, output_context, output_callback, i, taskResult.threadIndex);
					continue;
				}

				for (size_t j = 0; j < group.size(); ++j)
					clusters[group[j]].indices = std::vector<unsigned int>();

				for (Cluster& cluster : split)
				{
					cluster.refined = refined;
					cluster.bounds = taskResult.bounds;
					clusters.push_back(std::move(cluster));
					pending.push_back(int(clusters.size()) - 1);
				}
			}
		}

		depth++;
	}

	if (pending.size())
	{
		ZoneScopedN("clusterlod::clodBuildEx::OutputRootGroup");
		assert(pending.size() == 1);
		const Cluster& cluster = clusters[pending[0]];

		clodBounds bounds = cluster.bounds;
		bounds.error = FLT_MAX; // terminal group, won't simplify further

		if (clodTopologyStats* stats = config.topology_stats)
		{
			stats->roots++;
			stats->root_triangles += cluster.indices.size() / 3;
		}

		outputGroupEx(config, mesh, clusters, pending, bounds, depth, clodSimplifyMethod_None, clodSimplifyValidation_None, 0, output_context, output_callback, 0, 0);
	}

	return clusters.size();
}

size_t clodBuild(clodConfig config, clodMesh mesh, void* output_context, clodOutput output_callback)
{
	struct OutputAdapterContext
	{
		void* outputContext;
		clodOutput outputCallback;
	};

	struct OutputAdapter
	{
		static int output(void* output_context, clodGroup group, const clodCluster* clusters, size_t cluster_count, size_t, unsigned int)
		{
			OutputAdapterContext* context = static_cast<OutputAdapterContext*>(output_context);
			return context->outputCallback ? context->outputCallback(context->outputContext, group, clusters, cluster_count) : -1;
		}
	};

	OutputAdapterContext adapter{ output_context, output_callback };
	return clodBuildEx(config, mesh, &adapter, &OutputAdapter::output, NULL);
}

void clodBuild_iterationTask(void* iteration_context, size_t task_index, unsigned int thread_index)
{
	clod::IterationContext* context = static_cast<clod::IterationContext*>(iteration_context);
	clod::runIterationTask(*context, task_index, thread_index);
}

size_t clodLocalIndices(unsigned int* vertices, unsigned char* triangles, const unsigned int* indices, size_t index_count)
{
	return meshopt_extractMeshletIndices(vertices, triangles, indices, index_count);
}

size_t clodBuildHierarchyBound(size_t group_count, size_t node_width, size_t level_count)
{
	// count nodes for each tree depth; we pad by level_count at each iteration to account for unknown level distribution in the forest
	size_t total = level_count;
	for (size_t frontier = group_count; frontier > 1; frontier = (frontier + node_width - 1) / node_width)
		total += frontier + level_count;

	return total;
}

size_t clodBuildHierarchy(clodNode* nodes, const clodGroup* groups, size_t group_count, size_t node_width, size_t level_count)
{
	using namespace clod;

	// reserve space for per-level roots
	size_t offset = level_count;

	std::vector<clodNode> row(group_count);
	std::vector<unsigned int> order(group_count);

	for (size_t level = 0; level < level_count; ++level)
	{
		// start each tree hierarchy from the groups of that level as leaves
		row.clear();
		for (size_t i = 0; i < group_count; ++i)
			if (groups[i].depth == int(level))
			{
				clodNode node = {};
				node.bounds = groups[i].simplified;
				node.group = int(i);
				row.push_back(node);
			}

		// build the tree going up one level at a time, using spatial grouping of centers as a proxy for locality
		while (row.size() > 1)
		{
			size_t count = row.size();
			meshopt_spatialClusterPoints(order.data(), row[0].bounds.center, count, sizeof(clodNode), node_width);

			for (size_t i = 0; i < count; ++i)
				nodes[offset + i] = row[order[i]];

			row.clear();
			for (size_t i = 0; i < count; i += node_width)
			{
				// spatialClusterPoints guarantees that each cluster except for last one is full
				size_t children = std::min(node_width, count - i);
				row.push_back(mergeNodes(nodes, offset + i, children));
			}

			offset += count;
		}

		// the root of the current level goes into the fixed section at the beginning
		assert(row.size() == 1);
		nodes[level] = row[0];
	}

	assert(offset <= clodBuildHierarchyBound(group_count, node_width, level_count));
	return offset;
}
#endif

/**
 * Copyright (c) 2016-2026 Arseny Kapoulkine
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
