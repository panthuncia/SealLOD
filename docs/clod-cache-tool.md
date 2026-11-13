# CLodCacheTool: scene caches

`CLodCacheTool` builds a scene's Cluster LOD cache offline, with no GPU. For a glTF scene it writes a **scene cache**
to any directory: the CLod data of every primitive, and a manifest that the renderer opens instead of the glTF. The
manifest describes the scene (its placements, materials and textures), so the renderer loads it from the cache alone,
without opening the glTF.

Anything the scene references by path is read where it is, not copied. Its KTX2 textures are an example. The cache
holds only what had to be built.

## Building one

```powershell
CLodCacheTool.exe --out=D:\Caches\zorah --profile=CLodCacheTool\profiles\zorah_textured_public.v1.json `
    D:\Models\zorah_textured_public.v1\zorah_textured_public.v1\zorah_textured_public.v1.gltf
```

The run has four stages:

1. **Geometry.** Every primitive's CLod data is loaded from the cache if current, or built and saved.
   - Primitives are built largest first.
   - They are admitted while their estimated memory (`--bytes-per-triangle`, default 320) fits `--memory-budget-gb`. The
     default budget is three quarters of the RAM available when the run starts. A live check of the process's committed
     memory also holds admission back.
   - Each primitive's data is dropped once saved, so the scene's geometry is never all in memory.
   - meshopt-compressed buffer views are freed when the last primitive reading them finishes.
2. **Scene.** The tool describes the scene as the cache will hold it (`GlTFLoader::DescribeScene`, below).
3. **Textures.** The tool builds the texture requests the renderer makes for the described scene's materials
   (`GlTFLoader::SceneTextureRequests`), so only textures that kept meshes use. Each request goes one of two ways:
   - If it would stream straight from its file (`IsProvidedPassthrough`), it is only counted. This is any block-compressed KTX2 with a full mip chain, read through the KTX2 provider.
   - Otherwise its processed-cache entry is written on the CPU (`ConditionTextureOnCpu`), so the renderer finds it.
4. **Manifest.** The tool writes `<out>/<scene>.brscene.json` with the scene section. It then reads the manifest back
   and reports how many of the scene's primitives have CLod data.

The run prints progress every 10 s, the peak working set, and every failed primitive. The exit code is 1 unless every
selected primitive and texture is ready.

A rerun is resumable: finished primitives are cache hits. `--cache-only` checks a cache without building anything.

The scene section always covers every mesh the filters keep (profile `skipMeshes`, `--only-mesh`, `--skip-meshes`),
not just one shard's meshes. Every shard therefore writes the same description, and the last report reads
`Scene cache complete` once all shards have run.

Write the cache to a fast drive. On a QLC drive, sustained writes stall every worker in `NtWriteFile` once the drive's
write cache fills. A 16-shard slice of Zorah took 45 s writing to such a drive and 35 s to a TLC one.

| Option | |
|---|---|
| `--out=dir` | Cache root and manifest directory. |
| `--profile=file.json` | Import profile (below). |
| `--only-mesh=regex`, `--skip-meshes=regex` | Select meshes by name (ECMAScript, whole-name match). |
| `--shard-index=i --shard-count=n` | Meshes whose index % n == i, to split a build across runs or machines. |
| `--memory-budget-gb=N`, `--bytes-per-triangle=N`, `--jobs=N` | Build admission (see Geometry). |
| `--cache-only` | Misses are reported, nothing is built, and the manifest is left alone. |
| `--no-textures` | Skip the texture stage. |
| `--outcomes-csv=path` | One row per primitive: kind, triangles, start and end time. Use it to see admission and the tail. |
| `--clod-report=path`, `--clod-*=...`, `--debug` | As before. |

A directory argument queues only the scene files in it. A scene's own folder also holds its buffers and images, so
pass the scene file itself.

## Opening one

In the demo, open a manifest (or a folder of them) with Scene Explorer's Load Model, or place one from a scenario
(see [scripting.md](scripting.md)):

```csharp
var zorah = await ctx.Scene.LoadModel(@"D:\Caches\zorah\zorah_textured_public.v1.brscene.json");
```

Load Model on a directory opens each scene cache (`*.brscene.json`) in it. `br::import::LoadImportedAsset` takes a
directory only when it holds exactly one manifest.

For a manifest, the loader:

1. sets the cache root to the manifest's directory;
2. applies the recorded profile;
3. loads the scene **from the cache alone**, if the manifest has a scene section
   (`GlTFLoader::LoadImportedAssetFromSceneCache`):
   - each primitive's CLod data comes from the cache, read in parallel;
   - its mesh is made with the vertex format the manifest recorded;
   - materials and textures are made from the recorded descriptions;
   - the glTF is never opened, and the only files read besides the cache are the textures, from where they are;
4. otherwise imports the source glTF, whose primitives are then cache hits. The relative path is tried first, then the
   absolute one.

Step 4 also runs when a primitive has no current CLod data and the source is present: it builds the missing data. If
the source is gone, the scene loads without those primitives and logs how many were left out.

The source's location still matters without the source itself, because texture paths are relative to the source's
directory. Textures moved with the source go on working.

The cache root is one per process, and renderer startup resolves it. A manifest opened after startup still points
the CLod cache at its directory, but the renderer's other caches (shaders) stay in the root chosen at startup.

### What makes a cache portable

A glTF's CLod identity is `gltf://<file name>#<hash>`. The hash covers its JSON and its buffers' sizes; it does not
use the file's path. As a result:

- The scene and the cache can each be moved or copied.
- Editing the glTF changes the identity, and the loader warns that every primitive will rebuild.
- External buffers are not hashed, because that would mean 30 GB for Zorah. A buffer replaced in place with content
  of the same size keeps the old cache.

Textures that stream from their files are versioned by size and write time.

### The scene section

`BasicRenderer/Assets/Import/SceneCache.h` defines the scene section, with these tables:

| Table | Holds |
|---|---|
| `images` | Where each image's bytes are: a file, or a range of the `.glb` or `.bin`. Also whether the bytes are KTX2. |
| `textures` | An image, its sampler, and how a material uses it: sRGB, semantic, packed channels, normal convention. |
| `materials` | The `MaterialDescription` without textures, plus each slot's texture index. |
| `primitives` | The CLod identity: prim path and double-sided voxel normals. Also vertex flags and sizes, UV-set count and material. |
| `parts` | Name, matrix, instance matrices, and the primitives placed. |

The section is the importer's own intermediate form, not a copy of its output:

- A live glTF import describes the file into the same structures and makes its materials from them
  (`GltfSceneDescriber` and `GltfSceneRealizer` in `GlTFLoader.cpp`). A cached scene makes the same requests, with the
  same texture cache keys.
- `DescribeScene` checks that every texture realizes from its record with the key a live import requests, and the tool
  refuses to write a scene section that fails this.
- A primitive's format comes from `GlTFGeometryExtractor::DescribePrimitiveFormat`, which the extractor uses as well.

Some scenes cannot load from the cache alone. If the scene has animations or skinned nodes, or embeds images (data
URIs or inline buffers), the section records why (`notCacheableReason`), and the loader imports the source instead.

For Zorah the section is 8.3 MB of compact JSON: 9,504 parts, 16,066 placements, 7,249 primitives, 1,487 materials and
4,292 textures.

## Import profiles

A profile records what a scene's files do not say about themselves (`BasicRenderer/Assets/Import/ImportProfile.h`).
The manifest embeds it, so the tool and the renderer read the scene the same way.

```json
{
  "name": "zorah_textured_public.v1",
  "skipMeshes": "SM_Courtyard_Kite_Frame_A01_01.*|SM_Kite_VAT.*",
  "materialChannels": [
    { "texture": "metallicRoughness", "channels": 2, "metallic": 1, "roughness": 0 }
  ]
}
```

`materialChannels` rules are tried in order. Each can require a channel count (`channels`) and a name suffix
(`nameSuffix`). The channel count is known up front only for KTX2 images.

## glTF support added for this

| Feature | Notes |
|---|---|
| **KTX2 textures** | Recognised by their file identifier, whatever `mimeType` says. They stream through `MakeKtx2TextureMipProvider`. The tests are `Ktx2TextureTests`. |
| BCn / RGBA8 KTX2 | Each mip window reads its levels' bytes and inflates zstd level by level. |
| Basis Universal (ETC1S, UASTC) KTX2 | Transcoded once with libktx, to BC4, BC5 or BC7 by channel count. |
| `KHR_texture_basisu` | Its source is preferred over `source`. |
| `KHR_texture_transform` | One transform per material (`MaterialDescription::uvTransform`), applied to every slot. A material whose slots differ uses the first slot's and is reported. |
| `EXT_mesh_gpu_instancing` | A part carries `instanceMatrices`. The demo places it as one static group of all its instances. |
| Normals | glTF (OpenGL) normals load as stored, and the shader flips green (`MATERIAL_INVERT_NORMAL_GREEN`). A two-channel normal map reconstructs Z. Before this, every glTF normal map was decompressed, flipped and re-encoded. |
| `extensionsRequired` | A file requiring an extension the importer does not read is refused. |

The KTX2 dependency is KTX-Software (`ktx[tools]` in `vcpkg.json`), with zstd. Its command-line tools land in
`vcpkg_installed/<triplet>/tools/ktx`. `ktx info` and `ktx validate` are useful for checking a scene's textures.

## The Zorah scene

[`zorah_textured_public.v1`](https://developer.nvidia.com/rtx-kit) has:

- 2,812 meshes, 7,277 primitives, 3.31 G unique triangles and 16,119 instances;
- 30 GB of meshopt-compressed geometry in per-mesh buffers;
- 4,418 KTX2 textures (BC7, BC5 and BC4, zstd, full mip chains);
- 186 materials with texture transforms.

Its metallic-roughness textures are BC5 with **R = roughness, G = metallic**, which only its README states. The
profile in `CLodCacheTool/profiles/` encodes that, along with the meshes its `.cfg` skips.

Expect the texture stage to report every texture the scene uses as streamed and none processed. That is 4,292
textures: the meshes the profile skips use the rest of the 4,418.
