# Shader translation probe

Result of the highest-risk step of the Vulkan/Metal spike described in
[METAL_ROADMAP.md](METAL_ROADMAP.md) and [BACKEND_OPTIONS.md](BACKEND_OPTIONS.md):
can the existing hand-written GLSL reach Metal through SPIR-V without editing the
shader sources?

**Yes, through OpenGL-flavoured SPIR-V. Not yet through Vulkan-flavoured SPIR-V.**
That distinction is the finding, and it changes the cost of each backend.

Reproduce with [`tools/shader_translation_probe.py`](../tools/shader_translation_probe.py),
which mirrors `shader_sources_manager::load_includes` and `CRender::shader_compile`
from `src/Layers/xrRenderPC_GL/rgl_shaders.cpp` so the shaders see the same
`#include` expansion and the same `#define` preamble they see at runtime.

## What works

```text
GLSL 410  --(glslang -G)-->  OpenGL SPIR-V  --(spirv-cross --msl)-->  MSL
```

Nothing in `res/gamedata/shaders/gl` had to change. The uber-shader scheme
survives untouched, because the `#define` preamble is applied before glslang
exactly as the engine applies it before `glCompileShader`.

Sweeping every `.vs`/`.ps`/`.gs` under `res/gamedata/shaders/gl` with a single
fixed option set gives **225 of 286** shaders translated to valid MSL. The
remaining 61 are not translation failures; they are shaders whose option
combination does not match that one fixed set. Re-run with a matching variant and
they translate:

| Bucket | Count | Cause | With the right variant |
| --- | ---: | --- | --- |
| `deffer_base_aref_*` | 34 | `USE_R2_STATIC_SUN` makes `tcdh` a `float4`, so `tex2D(s_bumpX, I.tcdh)` has no overload | translates |
| `deffer_model_*`, `model_*` | 11 | compiled with `SKIN_NONE`; these are skinned and need `SKIN_0`..`SKIN_4` | translates |
| `combine_1*` | 3 | `plight_infinity` is gated behind a lighting-model define | translates |
| remainder | 13 | include-only files with no entry point, further option-gated identifiers | not individually triaged |

The engine already compiles each of these once per variant at runtime, so this is
the expected shape rather than a problem. The probe script takes `--variant` for
this reason.

## What does not work yet, and why it matters

`glslang -V` (Vulkan SPIR-V) rejects the tree on the first uniform:

```text
ERROR: 'non-opaque uniforms outside a block' : not allowed when using GLSL for Vulkan
```

`gl/common_cbuffers.h` declares them loose and says so:

```glsl
// TODO: OGL: Use constant buffers.
//cbuffer	dynamic_light
//{
	uniform float4	Ldynamic_color;
	uniform float4	Ldynamic_pos;
	uniform float4	Ldynamic_dir;
//}
```

`gl/common_samplers.h` does the same for textures: `#define Texture2D uniform sampler2D`,
with no `layout(binding = ...)`.

Consequences for the backend choice:

- A **native Metal** backend can consume the shader tree as it stands today. The
  chain above already produces compilable MSL.
- A **Vulkan/MoltenVK** backend cannot. It first needs every loose uniform moved
  into a uniform block or push constant, and explicit binding decorations on the
  samplers.

That work is bounded, mechanical, and is a `TODO` upstream already wants done - it
is the same change that would give the GL path real constant buffers instead of
per-uniform updates. But it is real work that the Metal route does not require, and
it belongs in the estimate. It does not overturn the recommendation in
[BACKEND_OPTIONS.md](BACKEND_OPTIONS.md), because the pipeline-state-object cache
and the D3D vocabulary cleanup still dominate, and because validation layers and
the upstream path remain Vulkan-only advantages. It does narrow the gap.

## Defects found in the shader tree

Both are upstream, both are invisible on the case-insensitive volume the engine is
developed on, and both are candidates to send upstream.

**Eleven `#include` spellings do not match the file on disk.** Any case-sensitive
filesystem - including a case-sensitive APFS volume, which macOS offers at format
time - fails to build these shaders:

| Include as written | File on disk | Files affected |
| --- | --- | ---: |
| `iostructs\v_TL.h` | `v_tl.h` | 11 |
| `iostructs\p_TL.h` | `p_tl.h` | 6 |
| `iostructs\p_TL_sun.h` | `p_tl_sun.h` | 1 |
| `iostructs\p_aa_AA_sun.h` | `p_aa_aa_sun.h` | 1 |
| `iostructs\p_aa_AA_combine.h` | `p_aa_aa_combine.h` | 1 |
| `iostructs\p_naa_AA_combine.h` | `p_naa_aa_combine.h` | 1 |
| `iostructs\v_TL0uv.h` | `v_tl0uv.h` | 1 |
| `iostructs\v_TL2uv.h` | `v_tl2uv.h` | 1 |
| `iostructs\v_aa_AA.h` | `v_aa_aa.h` | 1 |
| `combine_2_AA.ps` | `combine_2_aa.ps` | 1 |
| `combine_2_NAA.ps` | `combine_2_naa.ps` | 1 |

**`accum_volumetric_sun_normal .ps` is dead.** The file name contains a space
before the extension, so the engine can never open it, and its first line is
`#unfdef USE_MINMAX_SM` - not a preprocessor directive at all.

## Method

- glslang and SPIRV-Cross built from their Khronos repositories; the probe accepts
  `--glslang` and `--spirv-cross` so the Vulkan SDK copies can be used instead.
- The include walker is textual, like the engine's, and resolves case the way APFS
  would, so a case-sensitive host does not report the mismatches above as failures.
- Bindings and locations are auto-assigned (`--auto-map-bindings`,
  `--auto-map-locations`). A real backend must instead take the assignment from
  reflection and match it to `CBackend`'s texture and constant slots. That mapping
  is not part of this probe.

## Not established by this probe

- That the generated MSL is *correct*, only that it compiles through SPIRV-Cross.
  Nothing has been run on a GPU.
- Depth-range, clip-space and Y-axis conventions.
- Whether the resource binding model can be reflected onto the existing
  `R_constant_table` slots without changes.
