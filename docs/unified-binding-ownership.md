# Unified ORG binding ownership migration

## Contract

Descriptor indices serialized into GPU records are not ownership. A table
version must retain the exact allocation and descriptor for every index it
contains, including unchanged rows. Reusable CPU packets retain their versions
independently of GPU completion. Accepted submissions release their own holds
only after all relevant queue signals complete.

The common primitives are in OpenRenderGraph: `OwnedDescriptorBinding`,
`BindingRecordBuilder`, `BindingTableVersion`, and `PublicationBindingBundle`.
`ExecutionResourceLease` is a typed handle to the existing publication bundle,
not another execution ledger. `ResourceCleanupQueue` performs CPU destruction
only; it does not establish GPU safety.

## Implemented integration

- The BasicRenderer and CS ORG trees use identical source implementations for
  owned bindings, immutable binding records/pages, CPU cleanup, and descriptor
  lease callbacks. Dependency trees are not wholesale synchronized.
- Capturing an existing ORG resource view retains its immutable snapshot and
  descriptor lease without allocating or rewriting a descriptor. Capturing a
  fixed sampler retains its existing slot lease.
- BasicRenderer's texture-image table replaces renderer-specific image hold
  chunks with shared ORG binding-table roots. Both image and sampler indices
  are captured in the same record that is appended to the existing journal.
  Publication artifacts retain the root with the exact versioned table buffer.
- Publication resource snapshots can retain table dependencies. Cached snapshot
  selection rejects a different dependency root.
- Shared binding tests exercise COW replacement, clearing, skipped changes,
  index reuse, creation failure, and deferred destruction. Execution admission
  tests check that multi-queue completion does not revoke reusable CPU ownership.
- Both hosts expose their device-generation cleanup lane. The CS host accepts a
  resource owner before synchronous execution and retains it through accepted
  queue signals or an uncertain partial failure, as its async path already did.
- DCLF imports now allocate owned ORG descriptors and retain their DXVK SRV,
  imported image wrapper, and adopted device. Material-slot changes produce
  cleanup-owned binding blocks; main builds retain the blocks they use,
  including clean persistent records. Current-frame texture patches extend the
  same submission owner. Shadow builds retain used diffuse imports, with the
  sky epoch retaining the shadow build's root. Null and fixed sampler bindings
  are owned by the device generation.

## Remaining acceptance work

This is not the completed migration. The standalone experimental execution
retirement queue has been removed; bounded admission, multi-queue and uncertain
submission coverage belongs to the existing execution admission tests. Host
completion and legacy deferred-release APIs must
converge on one device-generation context, including wakeable no-next-frame
retirement. Prepared compatibility and descriptor mutation paths still require
the complete binding-version audit.

DCLF still has parallel numeric/owner arrays rather than ORG binding-table
records, an age-based import registry, and some reusable ticket/upload paths
whose ownership has not been audited. Texture expiry must remain until that
coverage and pre-suppression readiness are complete. Device teardown must
cover both externally adopted devices and renderer-owned devices before
claiming automatic lifetime safety.

No performance or rendering acceptance is implied by compilation or synthetic
tests. Final acceptance requires matched SARP streaming counts, capped MO2
captures, rendering coverage, and separate validation runs.

## Validation, 2026-09-28

- BasicRenderer static library, CommunityShaders DLL, and both ORG
  external-resource test targets build.
- CS ORG: async compiler, D3D12 external resources, Vulkan synchronous host and
  Vulkan asynchronous host tests pass (4/4).
- BasicRenderer ORG: persistent graph, async compiler and external resources
  pass (3/3). Renderer boundary audit/fixtures and material ownership guard pass.
- RendererBuildInputAudit reports seven pre-existing untracked graph-extraction
  source/header inputs; this migration does not stage or alter those files.
- After the DCLF integration, the CS DLL and BasicRenderer ORG library build;
  BasicRenderer's SARP/ORG selection passed 22 tests. Runtime capture and
  matched performance comparison remain outstanding.

## DCLF follow-up: material membership is not visibility

The earlier "age-based import registry" and "expiry must remain" statements
above describe an intermediate migration state: DCLF subsequently removed
the 600-frame texture expiry queue. A camera-moving Tracy capture found that
the remaining import churn is caused by material-slot retirement/recreation,
not by changing diffuse/normal SRVs. All 20,106 observed `t0`/`t1` lookup
changes began from a null prior view, while each of 1,440
material/pass/register groups used a single SRV identity. One 300-frame
report recorded 8,100 last-reference material-slot retirements.

This is an open DCLF capture/lifecycle investigation, not a shared ORG
descriptor-lifetime problem: camera/frustum exit may be clearing an object's
material reference as though it had left the scene. Keep immutable binding
ownership independent of current visibility. Establish the exact object
transition before changing DCLF retirement; do not add an age-based texture
cache to mask the churn. The matching trace and implementation notes are in
`external_refs/cs-org-dxvk/docs/development/dclf-async-publication.md`.
