# Wiring the Vulkan renderer into the engine

The platform probe presents frames (see [METAL_ROADMAP.md](METAL_ROADMAP.md)), but it is a
standalone executable. This page measures what stands between that and a `renderer_vk` the
engine can select, because the answer is larger than "implement `IRender`".

## What a renderer module has to provide

`RendererModule::SetupEnv` in `src/Layers/xrRenderPC_GL/xrRender_GL.cpp` does not install one
interface, it installs five:

```cpp
GEnv.Render        = &RImplementation;
GEnv.RenderFactory = &RenderFactoryImpl;
GEnv.DU            = &DUImpl;
GEnv.UIRender      = &UIRenderImpl;
GEnv.DRender       = &DebugRenderImpl;   // Debug builds
```

Counted from the headers:

| Interface | Header | Pure virtuals |
| --- | --- | ---: |
| `IRender` | `src/xrEngine/Render.h` | 90 |
| `IDrawUtils` | `src/Include/xrRender/DrawUtils.h` | 46 |
| `IUIRender` | `src/Include/xrRender/UIRender.h` | 23 |
| `IRenderFactory` | `src/Include/xrRender/RenderFactory.h` | 6 |
| `IDebugRender` | `src/Include/xrRender/DebugRender.h` | 12 |
| | **total** | **177** (165 outside Debug) |

`IRenderFactory`'s six methods are not the cheap part. Each returns an object implementing a
further interface - `IRenderVisual`, `IUIShader`, `IFontRender`, `IWallMarkArray` and the rest
of `src/Include/xrRender/` - so the factory is the door into another two dozen small
interfaces. Those can be deferred: the engine only calls them when it has content to draw.

An earlier note in this repository put `IRender` at 112 methods. That figure counted the whole
header, including `IRender_Light`, `IRender_Glow` and `IRender_ObjectSpecific`, which are
implemented by objects the renderer hands out rather than by the renderer itself. The correct
count for `IRender` is 90.

## The hardcoded module count (done)

The registry used to be a fixed-size `std::array<RendererModule*, 2>`, and its size was
spelled out in seven places: the definition in `src/xr_3da/entry_point.cpp` plus the
declaration and definition of `CEngineAPI::CreateRendererList`, `CEngine::Initialize` and
`CApplication::CApplication`. Adding a third module meant touching all seven.

It is now a `xr_vector<RendererModule*>`. The project is C++17, so `std::span` was not
available; the vector is built as a local in `entry_point`, not at namespace scope, so nothing
allocates through `xrMemory` before xrCore is up. Dropping the fixed size also closed the
`#ifdef XR_PLATFORM_WINDOWS` hole that left a null trailing entry on non-Windows builds -
where the dedicated-server path previously indexed `modules[0]` it now checks the vector is
non-empty first. No Vulkan code is involved, so this is worth proposing upstream on its own.

## Staging

The 165 methods are boilerplate, not design, so they should be generated rather than typed:
a script that reads the headers and emits a stub class whose every method logs once and
returns a neutral value. That keeps the stubs in sync when upstream changes an interface, and
it makes the diff reviewable - a generated file plus the handful of methods actually
implemented.

Proposed order, each step leaving the OpenGL renderer selectable and working:

1. **Registry.** Replace the fixed array with a span or vector. No Vulkan code involved; can
   go upstream by itself.
2. **Generated stubs.** All five interfaces, every method a logged no-op. Target links, and
   selecting `renderer_vk` reaches `SetupEnv` and fails loudly rather than silently.
3. **Device and presentation.** Move the probe's instance, device, swapchain and frame loop
   behind `IRender::Create`, `OnDeviceCreate`, `Begin`/`End`. The engine's own window shows a
   cleared frame. This is the real exit condition of roadmap phase 1.
4. **Resources.** `IRenderFactory` and the visual/shader objects, enough to submit geometry.
5. **Pipeline state cache.** The structural work identified in
   [BACKEND_OPTIONS.md](BACKEND_OPTIONS.md): collecting `CBackend`'s immediate-mode state into
   hashed pipeline objects. Backend-agnostic, and the part that actually buys performance.

Steps 1 and 2 are mechanical. Step 3 is mostly code that already exists and is known to work.
Step 5 is the one that needs design, and it is deliberately last: it should be shaped by a
renderer that is already drawing, not guessed at in advance.
