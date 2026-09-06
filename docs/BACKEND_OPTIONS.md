# Renderer backend options on macOS

Research note supporting [METAL_ROADMAP.md](METAL_ROADMAP.md). It answers two
questions asked before the Vulkan spike began:

1. Can the existing OpenGL renderer be *combined* with Metal instead of replaced?
2. Is a full move to Metal realistic, and by which route?

Everything below is measured against this fork, not against X-Ray in general.

## What the code actually looks like

| Layer | Files | Lines |
| --- | ---: | ---: |
| `src/Layers/xrRender` (shared, API-agnostic-ish) | 294 | 50,807 |
| `src/Layers/xrRender_R2` (deferred pipeline) | 35 | 9,991 |
| `src/Layers/xrRenderGL` (GL device/resources) | 22 | 4,470 |
| `src/Layers/xrRenderPC_GL` (GL renderer module) | 12 | 4,247 |

The **GL-specific surface is 8,717 lines across 34 files**, not the 69,515 of the
renderer as a whole. 135 distinct `gl*` entry points are used. That is the real
size of the thing a second backend has to replace.

Three facts matter more than the line counts:

- **The extension point already exists.** `RendererModule` (`src/xrEngine/EngineAPI.h`)
  plus `IRender` (`src/xrEngine/Render.h`, 112 pure virtuals) is a contract that is
  already implemented twice — GL and DX11. Modules are statically linked and selected
  by name through the `renderer` console variable; `src/xr_3da/entry_point.cpp` holds
  the registry. Adding a third module changes four lines of registration.
- **`CBackend` has no pipeline-state-object concept.** `src/Layers/xrRender/R_Backend.h`
  is a single non-virtual class that sets blend, depth, raster and shader state one call
  at a time, immediate-mode, in D3D9 idiom, then draws. Both Vulkan and Metal require
  that state to be baked into an immutable pipeline object *before* the draw. This is
  the central structural mismatch, and it is identical for Vulkan and for Metal.
- **There is no cross-compiler and no SPIR-V anywhere.** GLSL and HLSL are authored by
  hand as two separate shader sets. The GL path builds an uber-shader preamble by string
  concatenation of `#define`s, runs a hand-rolled `#include` walker, and feeds
  `glCreateShader`/`glShaderSource`/`glCompileShader`, caching via
  `GL_ARB_get_program_binary`. `IRender::shader_compile` still returns `HRESULT` and takes
  a D3D-style target string. This is the largest unknown in any move off GL.

Two smaller leaks: the `D3DPT_*` primitive-topology vocabulary appears in 48 files, and
`R_Backend.h` defines constants as `const u32 CULL_CCW = D3DCULL_CCW;`. Cosmetic, but it
runs through code well above the backend boundary.

## Option A — run OpenGL and Metal side by side in one frame

Technically possible, practically pointless.

macOS offers no shared context between an OpenGL context and an `MTLDevice`. The only
zero-copy bridge is `IOSurface`/`CVPixelBuffer`, which shares *image memory* — it does not
share shader programs, buffers, samplers or state. A hybrid frame would mean rendering the
G-buffer in GL, flushing, wrapping the result as an `IOSurface`, and running post-processing
in Metal. Each handoff needs a `glFlush` and a fence, on the CPU, every frame.

The decisive point: **on Apple Silicon, Apple's OpenGL driver is already implemented on top
of Metal.** "Marrying GL to Metal" has therefore already happened — through a closed,
unprofilable Apple translation layer that is frozen at OpenGL 4.1 and formally deprecated.
Adding a second, hand-written bridge on top of that buys nothing except two drivers to debug.

Rejected.

## Option B — a translation layer (Zink)

Mesa's Zink implements OpenGL 4.6 on top of Vulkan. Stacked with MoltenVK the chain becomes
GL → Vulkan → Metal, with no renderer changes in this repo at all. The prize is real: it
would lift the ceiling from Apple's GL 4.1 to GL 4.6 — compute shaders, SSBOs, direct state
access, everything the current backend cannot reach.

The cost is three translation layers, each with its own overhead and its own bugs, and a
macOS Zink path that has never been production-grade. Zink asks for Vulkan features MoltenVK
does not fully provide, and debugging a fault means deciding which of three translators is
at fault.

Worth keeping as an *experiment* — it needs no code from us, so it can be tried in an
afternoon to see how the GL renderer behaves under a non-Apple GL implementation. Not a
shipping target.

## Option C — a Vulkan backend through MoltenVK

State of Vulkan on Apple as of 2026:

- **MoltenVK** provides nearly conformant **Vulkan 1.4** on macOS, iOS and tvOS, on both
  Apple Silicon and x86_64. Shader conversion is SPIR-V → MSL via SPIRV-Cross, performed at
  `vkCreateShaderModule` time; the Vulkan pipeline cache can serialize the already-converted
  MSL so that conversion is not repeated on every launch.
- **KosmicKrisp** (LunarG, merged into Mesa) is a second, newer Vulkan-on-Metal driver,
  Apple Silicon and Metal 4 only. LunarG's January 2026 write-up put it at Vulkan 1.3
  conformance with 1.4 "forthcoming"; the SDK shipped in August 2026 already reports 1.4
  (see the measured baseline below). LunarG calls it "the future of Vulkan on Apple."

The important consequence is not which one wins: it is that **Vulkan on Apple now has two
independent implementations**, both Apple-Silicon-first, one of them inside Mesa. The
single-point-of-failure risk that used to argue against MoltenVK is much weaker than it was.

Upstream relevance: OpenXRay has an open, unassigned, "help wanted" issue —
[#447 Vulkan Renderer](https://github.com/OpenXRay/xray-16/issues/447), open since 2019 —
asking for exactly an `xrRender_VK` module as an *additional* renderer, with shader
compilation through a cross-compiler. Work done on this route is upstreamable; a
macOS-only Metal backend is not.

Also already available in the tree: `Externals/imgui/backends/imgui_impl_vulkan.cpp` and
`imgui_impl_sdl2.cpp`. SDL2 is required at ≥ 2.0.18 (`cmake/XRay.Compiler.GNULike.cmake`),
well past the 2.0.6 that introduced `SDL_Vulkan_*`. The UI and windowing layers come for free.

## Measured baseline on the development machine

`vulkaninfo --summary`, Vulkan SDK 1.4.357 on macOS 27, Apple M3 Pro:

| | GPU0 | GPU1 |
| --- | --- | --- |
| driverID | `DRIVER_ID_MOLTENVK` | `DRIVER_ID_MESA_KOSMICKRISP` |
| driverInfo | MoltenVK 1.4.2 | `vulkan-sdk-1.4.357.1 (git-92ec601a34)` |
| apiVersion | 1.4.357 | 1.4.359 |
| conformanceVersion | 1.4.4.0 | 1.4.3.2 |
| deviceType | integrated | integrated |

The full capability dump from the Vulkan Hardware Capability Viewer is kept alongside this
page as [vulkan-caps-apple-m3-pro.json](vulkan-caps-apple-m3-pro.json).

Both drivers enumerate the same physical GPU, so a build can be pointed at either through
`VK_DRIVER_FILES` without reinstalling anything. Both are conformant at **Vulkan 1.4**, and
`VK_LAYER_KHRONOS_validation` 1.4.357 is present.

Two consequences for the design:

- There is **no old-driver floor to support**. macOS is the only platform this backend has to
  run on, and the oldest thing it will meet is Vulkan 1.4. Targeting 1.3+ and using dynamic
  rendering (`VK_KHR_dynamic_rendering`, core since 1.3) removes `VkRenderPass` and
  `VkFramebuffer` objects entirely. That is both less code and a closer match to Metal, where
  a render pass is described at encoder creation rather than baked into an object.
- `VK_KHR_portability_enumeration` must be requested at instance creation and the
  `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` flag set, or MoltenVK is not enumerated
  at all. `VK_KHR_portability_subset` must then be enabled on the device.

A driver reports its own `apiVersion` capped at what the instance asked for, and the two
drivers differ in how they treat that. Asking for 1.2, MoltenVK reported itself as `1.2.357`
while KosmicKrisp still reported `1.4.359`; asking for 1.4, MoltenVK reports `1.4.357`. So an
over-cautious `VkApplicationInfo::apiVersion` does not merely forbid newer features, it hides
what the driver can do, and it hides it inconsistently between drivers. Query
`vkEnumerateInstanceVersion` and ask for what is actually there.

The probe targets 1.3 and uses dynamic rendering. Both drivers advertise `dynamicRendering`,
and validation stays silent over the manual layout barriers it requires.

## Option D — a native Metal backend (`xrRender_MTL`)

Fewest layers, so the profiler shows our code rather than a translator's, and Metal is a
considerably smaller API than Vulkan — no manual memory suballocation, no descriptor set
layouts, far less ceremony. For a single developer, a Metal backend is genuinely faster to
write than a Vulkan one.

Against it: it locks the work to macOS, it cannot go upstream, and it makes us the sole
maintainer of a backend nobody else exercises. It also does not avoid any of the three
structural problems above — the PSO cache, the shader story and the D3D vocabulary are
required identically.

## Recommendation

**Vulkan through MoltenVK, as an additional renderer module, keeping GL as the reference.**

The reasoning is that roughly 80% of the work is backend-agnostic:

- introducing a pipeline-state-object cache above `CBackend`,
- establishing a SPIR-V shader path and resource-binding reflection,
- pulling the `D3DPT_*` / `D3DCULL_*` vocabulary out of the shared layers,
- fixing clip-space, depth-range (GL `-1..1` vs Vulkan/Metal `0..1`) and Y-axis conventions.

None of that is thrown away if we later decide to write `xrRender_MTL`; what would remain is
a comparatively thin command-encoding layer. Choosing Metal first, by contrast, throws away
the upstream path and the validation layers immediately.

Native Metal stays on the table as a later phase, to be opened only if profiling shows
MoltenVK itself is the bottleneck — not on principle.

## What the spike must prove

Ordered by risk, highest first:

1. **Shader translation.** Take a real project GLSL shader with its `#define` preamble,
   run it through glslang to SPIR-V and SPIRV-Cross to MSL, and confirm the uber-shader
   variant scheme survives. If this does not work, the whole route changes shape. This is
   the single most valuable thing the spike produces.
2. **Toolchain.** Vulkan SDK / MoltenVK present and linkable in the existing CMake +
   Ninja + arm64 build, without disturbing the GL build.
3. **Presentation.** `SDL_Vulkan_*` surface, physical device and queue selection, swapchain,
   clear colour presented, validation layers enabled in Debug.
4. **Conventions.** Depth range and Y-flip verified against the GL reference captures.
5. **Feature subset.** Confirm MoltenVK exposes what R2/R3 needs (MRT count, depth formats,
   clip distances — the GL path already needed an explicit `GL_CLIP_DISTANCE0..5` workaround).

Non-goal for the spike: rendering any game content. The default renderer must not change.
