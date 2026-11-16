# Binding ownership

## Contract

Descriptor indices serialized into GPU records are not ownership. A table
version retains the exact allocation and descriptor for every index it
contains, including unchanged rows. Reusable CPU packets retain their versions
independently of GPU completion. Accepted submissions release their own holds
only after all relevant queue signals complete.

The common primitives are in OpenRenderGraph: `OwnedDescriptorBinding`,
`BindingRecordBuilder`, `BindingTableVersion`, and `PublicationBindingBundle`.
`ExecutionResourceLease` is a typed handle to the existing publication bundle,
not another execution ledger. `ResourceCleanupQueue` performs CPU destruction
only; it does not establish GPU safety.

## Integration

- The BasicRenderer and CS ORG trees use identical source implementations for
  owned bindings, immutable binding records/pages, CPU cleanup, and descriptor
  lease callbacks. The rest of the two dependency trees are not kept in sync.
- Capturing an existing ORG resource view retains its immutable snapshot and
  descriptor lease without allocating or rewriting a descriptor. Capturing a
  fixed sampler retains its existing slot lease.
- BasicRenderer's texture-image table is built on shared ORG binding-table roots.
  Image and sampler indices are captured in the same record, which is appended
  to the journal. Publication artifacts retain the root with the exact versioned
  table buffer.
- Publication resource snapshots can retain table dependencies. Cached snapshot
  selection rejects a different dependency root.
- Shared binding tests exercise COW replacement, clearing, skipped changes,
  index reuse, creation failure, and deferred destruction. Execution admission
  tests cover bounded admission, multi-queue and uncertain submissions, and check
  that multi-queue completion does not revoke reusable CPU ownership.
- Both hosts expose their device-generation cleanup lane. The CS host accepts a
  resource owner before synchronous execution and retains it through accepted
  queue signals or an uncertain partial failure, as its async path does.
- DCLF imports allocate owned ORG descriptors and retain their DXVK SRV,
  imported image wrapper, and adopted device. Material-slot changes produce
  cleanup-owned binding blocks; main builds retain the blocks they use,
  including clean persistent records. Current-frame texture patches extend the
  same submission owner. Shadow builds retain used diffuse imports, with the
  sky epoch retaining the shadow build's root. Null and fixed sampler bindings
  are owned by the device generation.

Immutable binding ownership is independent of current visibility: an object
leaving the view does not release the bindings its material holds.

## Gaps

- Host completion and the legacy deferred-release APIs do not yet share one
  device-generation context, including wakeable no-next-frame retirement.
- Prepared compatibility and descriptor mutation paths have not had a full
  binding-version audit.
- DCLF keeps parallel numeric/owner arrays rather than ORG binding-table
  records, and some of its reusable ticket/upload paths have unaudited ownership.
- Device teardown does not yet cover both externally adopted and renderer-owned
  devices, so lifetime safety is not automatic.
- DCLF churns material slots under camera movement (last-reference retirement and
  recreation as objects leave the frustum); this is a DCLF capture/lifecycle issue,
  not a descriptor-lifetime one. See
  `external_refs/cs-org-dxvk/docs/development/dclf-async-publication.md`.
