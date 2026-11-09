# CLodCacheTool: scene caches

`CLodCacheTool` builds a scene's Cluster LOD cache offline, with no GPU. For a glTF scene it writes a **scene cache**
to any directory: the CLod data of every primitive, and a manifest that the renderer opens instead of the glTF.

Anything the scene references by path is read where it is, not copied. Its KTX2 textures are an example. The cache
holds only what had to be built.

## Building one

```powershell
CLodCacheTool.exe --out=D:\Caches\zorah --profile=CLodCacheTool\profiles\zorah_textured_public.v1.json `
    D:\Models\zorah_textured_public.v1\zorah_textured_public.v1\zorah_textured_public.v1.gltf
```

The run has three stages:

1. **Geometry.** Every primitive's CLod data is loaded from the cache if current, or built and saved.
   - Primitives are built largest first.
   - At most `--memory-budget-gb` of them run at once, estimated at `--bytes-per-triangle` each. The default budget is half of RAM.
   - Each primitive's data is dropped once saved, so the scene's geometry is never all in memory.
   - meshopt-compressed buffer views are freed when the last primitive reading them finishes.
2. **Textures.** The tool builds the texture requests the renderer makes for the scene's materials (`GlTFLoader::BuildMaterialTextureRequests`). Each request goes one of two ways:
   - If it would stream straight from its file (`IsProvidedPassthrough`), it is only counted. This is any block-compressed KTX2 with a full mip chain, read through the KTX2 provider.
   - Otherwise its processed-cache entry is written on the CPU (`ConditionTextureOnCpu`), so the renderer finds it.
3. **Manifest.** The tool writes `<out>/<scene>.brscene.json`.

The run prints progress every 10 s, the peak working set, and every failed primitive. The exit code is 1 unless every
selected primitive and texture is ready.

A rerun is resumable: finished primitives are cache hits. `--cache-only` checks a cache without building anything.

| Option | |
|---|---|
| `--out=dir` | Cache root and manifest directory. |
| `--profile=file.json` | Import profile (below). |
| `--only-mesh=regex`, `--skip-meshes=regex` | Select meshes by name (ECMAScript, whole-name match). |
| `--shard-index=i --shard-count=n` | Meshes whose index % n == i, to split a build across runs or machines. |
| `--memory-budget-gb=N`, `--bytes-per-triangle=N`, `--jobs=N` | Build admission (see Geometry). |
| `--cache-only` | Misses are reported, nothing is built, and the manifest is left alone. |
| `--no-textures` | Skip the texture stage. |
| `--clod-report=path`, `--clod-*=...`, `--debug` | As before. |

A directory argument queues only the scene files in it. A scene's own folder also holds its buffers and images, so
pass the scene file itself.

## Opening one

```powershell
BasicRenderer.exe --model D:\Caches\zorah\zorah_textured_public.v1.brscene.json
```

`br::import::LoadImportedAsset` recognises `*.brscene.json`. It then:

1. sets the cache root to the manifest's directory;
2. resolves the source, trying the relative path first and then the absolute one;
3. applies the recorded profile;
4. loads the glTF.

Each primitive is then a cache hit, and the loader reads none of its geometry.

The cache root is one per process, and renderer startup resolves it. The demo therefore applies the first manifest
among its `--model`s before initializing. The renderer's other caches (shaders) then live in the scene cache
directory too.

### What makes a cache portable

A glTF's CLod identity is `gltf://<file name>#<hash>`. The hash covers its JSON and its buffers' sizes; it does not
use the file's path. As a result:

- The scene and the cache can each be moved or copied.
- Editing the glTF changes the identity, and the loader warns that every primitive will rebuild.
- External buffers are not hashed, because that would mean 30 GB for Zorah. A buffer replaced in place with content
  of the same size keeps the old cache.

Textures that stream from their files are versioned by size and write time.

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

Expect the texture stage to report all 4,418 textures as streamed and none processed.
