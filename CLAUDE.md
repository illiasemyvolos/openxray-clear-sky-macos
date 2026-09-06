# Working in this repository

A personal fork of [OpenXRay](https://github.com/OpenXRay/xray-16), taking
S.T.A.L.K.E.R.: Clear Sky 1.5.10 to run natively on macOS / Apple Silicon. Base is X-Ray
1.6.02 as OpenXRay maintains it. Current branch: `codex/metal-backend`.

## Rules that are not negotiable

- **No game content, ever.** No `.db*`/`.xdb` archives, no extracted textures, models, audio,
  scripts, level data or localization. The engine source is MIT; the game is GSC's. See the
  Legal section of [README.md](README.md).
- **No private paths, logs or saves in commits.** Absolute paths under a home directory belong
  in the workspace, not here.
- **OpenGL stays the default renderer and stays working.** `renderer_r3` is what people
  actually play. Vulkan work lives behind `XRAY_BUILD_VK`, which is `OFF` by default; with it
  off, neither `find_package(Vulkan)` nor `src/Layers/xrRenderPC_VK` is configured at all.
- **Keep upstream-extractable fixes in their own commit**, touching only engine files, so they
  can be cherry-picked into a pull request without the macOS-specific documentation.

## Layout

This repository is one half of a pair. Its sibling `../workspace` is private and untracked:
build directories, the `.app` launcher, legally acquired game data, the Vulkan SDK, diagnostic
sessions, and the `.command` scripts that configure, build, launch and profile. Nothing in
`../workspace` may be referenced from files committed here.

## Read these before changing the renderer

- [docs/CLEAR_SKY_MACOS.md](docs/CLEAR_SKY_MACOS.md) - how the build and the game data fit together.
- [docs/MACOS_CHANGES.md](docs/MACOS_CHANGES.md) - every fork-specific change and why, plus known limitations.
- [docs/BACKEND_OPTIONS.md](docs/BACKEND_OPTIONS.md) - why Vulkan-through-MoltenVK over native Metal or a translation layer, with the measured driver baseline.
- [docs/SHADER_TRANSLATION_PROBE.md](docs/SHADER_TRANSLATION_PROBE.md) - the GLSL to SPIR-V to MSL result, and the one thing that blocks Vulkan-flavoured SPIR-V.
- [docs/VK_MODULE_PLAN.md](docs/VK_MODULE_PLAN.md) - measured cost of wiring a renderer module, and the staged plan.
- [docs/METAL_ROADMAP.md](docs/METAL_ROADMAP.md) - phases and exit conditions.
- [docs/DEBUGGING_MACOS.md](docs/DEBUGGING_MACOS.md) - reproducible graphics tests.

## Where things stand

Playable OpenGL baseline on Apple M3 Pro. Four fork-specific engine changes: a null pixel
shader fallback for depth-only passes, explicit `GL_CLIP_DISTANCE` for volumetric spotlights,
an SDK-path fix in the LuaJIT project, and readable GL error names. Plus one upstream bug fix
worth sending back: `cmp_pass` in `src/Layers/xrRender/r__dsgraph_render.cpp` was not a strict
weak ordering and crashed inside `std::sort`.

The Vulkan work has reached a standalone platform probe (`src/Layers/xrRenderPC_VK/`) that
presents through MoltenVK with dynamic rendering and clean validation. It is not yet a
renderer module. Step 1 of [docs/VK_MODULE_PLAN.md](docs/VK_MODULE_PLAN.md) is done: the
renderer registry is a `xr_vector<RendererModule*>` rather than a fixed-size array. The next
step is step 2, the generated interface stubs.

## Building

The build is driven from `../workspace` by `.command` wrappers, because it needs Homebrew
CMake and Ninja, an explicit SDK root, and a deployment target. Read those scripts rather than
inventing a command line. Two warnings are expected and understood: Homebrew dylibs are built
for a newer macOS than `CMAKE_OSX_DEPLOYMENT_TARGET`, which only matters for distribution, and
`libxrMiscMath.a` is listed twice by upstream CMake.

Most compiler warnings in a full build are inherited from upstream X-Ray. Do not clean them up
opportunistically; they bury the ones that are ours.

## Conventions

Commit subjects are `type(scope): imperative summary`. Bodies explain the mechanism and the
reasoning, not the diff. State what was verified and on what hardware. Say plainly what a
change does not establish.

Documentation in `docs/` is expected to stay true. If a change makes a sentence there wrong,
fix the sentence in the same commit.
