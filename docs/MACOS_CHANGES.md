# Current macOS changes

This page records the fork-specific changes relative to the OpenXRay `dev` branch at commit `29030f81b`.

## Build prerequisites

Commit `9cfd295d6` makes the LuaJIT project discover the active macOS SDK with `xcrun` when CMake has not supplied `CMAKE_OSX_SYSROOT`.

The same change pins the GameSpy submodule to a revision with working macOS atomic reference counting. The companion GameSpy change replaces unsupported constant-return stubs with sequentially consistent `__atomic_add_fetch` and `__atomic_sub_fetch` operations.

Commit `b5944e2c8` updates `.gitmodules` so fresh recursive clones use the companion [gamespy-macos](https://github.com/illiasemyvolos/gamespy-macos) fork.

## Depth-only OpenGL passes

Commit `040d981b9` handles passes whose X-Ray pixel shader is intentionally null. Apple OpenGL requires a valid fragment stage in the separable program pipeline, so the resource manager attaches the input-free `dumb` shader before creating the pipeline.

This removed the repeatable Debug failure in the sun shadow path with a stack ending in `CSkeletonX::_Render` and restored shadow occlusion for the indoor sun-shaft test scene.

The same commit maps OpenGL error values such as `0x0502` to readable names such as `GL_INVALID_OPERATION` in the cross-platform error dialog.

## Volumetric spotlight clipping

Commit `ee1c57d0e` enables `GL_CLIP_DISTANCE0` through `GL_CLIP_DISTANCE5` while rendering a volumetric spotlight and disables them immediately afterward.

The vertex shader already produced six clip distances, but OpenGL ignores them unless their corresponding capabilities are enabled. This change keeps the volume slices inside the spotlight frustum and fixes the desk-lamp cone extending across unrelated surfaces.

## Render-graph pass sorting

`cmp_pass` in `src/Layers/xrRender/r__dsgraph_render.cpp` was not a strict weak
ordering, so `std::sort` in `R_dsgraph_structure::render_graph` had undefined
behavior.

The comparator returned `false` for passes that compare `equal()`, and otherwise
`left->second.ssa >= right->second.ssa`. For two passes that are not `equal()` but
share an SSA value, that reports `cmp(a, b)` and `cmp(b, a)` as both true. Such
ties are ordinary rather than rare: the graph builder feeds the same SSA into
every pass bucket of a multi-pass shader. The `equal()` short-circuit also broke
transitivity of equivalence independently of the `>=`.

libc++ responds by running the unguarded insertion pass of `__introsort` past the
front of the array and dereferencing whatever precedes it. The observed signature
is `EXC_BAD_ACCESS` in `cmp_pass` called from `std::__introsort`, called from
`R_dsgraph_structure::render_graph`, with a wild pointer in the faulting register.

The comparator now orders strictly by descending SSA and breaks ties with
`std::less<>` on the map key. Ordering changes only among equal-SSA buckets of
opaque geometry, which affects state-change counts rather than output.

This is an upstream defect, not a macOS one. It reproduces on the prebuilt
OpenXRay 1.6 macOS build as well, and is a candidate to send upstream.

## Debugger detection on Darwin

`xrDebug::DebuggerIsPresent` in `src/xrCore/xrDebug.cpp` probed for a debugger by calling
`ptrace(PTRACE_TRACEME)` and then `ptrace(PTRACE_DETACH)`, with `PTRACE_TRACEME` aliased to
`PT_TRACE_ME` on Apple and the BSDs. That is the Linux idiom, and on Darwin it is actively
harmful: `PT_TRACE_ME` does not report whether a debugger is attached, it asks to *become*
traced by the parent. The detach that follows does not reliably undo it.

The process is then flagged `P_TRACED` with a tracer - the launching shell - that never waits
on it. The kernel stops such a process on the next signal and nothing resumes it. It sits in
`TX`, `SIGKILL` cannot complete its teardown, and a debugger cannot take over because the
kernel already considers it traced: `lldb` refuses with "tried to attach to process already
being debugged", as root as well. Nothing short of a reboot clears it.

It is reachable on macOS from the abort branch of `xrDebug::Fail` and from `xrDebug::DoExit`.
Because X-Ray installs `handler_base` for `SIGTERM` on every thread it spawns, an ordinary
`kill` of the game is enough to enter that path.

Darwin and the BSDs now use the read-only `sysctl` query from Apple Technical Q&A QA1361,
checking `P_TRACED` in `kinfo_proc`. It cannot alter the caller's own trace state, so it is
safe on an assertion path. Linux keeps the `ptrace` probe.

Verified by sending `SIGTERM` to a running Debug build on Apple M3 Pro, macOS 27: before the
change the process wedged in `TX` and survived `SIGKILL`, `SIGCONT` and `sudo lldb`; after it,
the same signal leaves the process in `SN` and `kill -9` ends it.

This is an upstream defect, not a macOS-only concern - it affects every BSD target - and is a
candidate to send upstream.

## Verified behavior

- Clear Sky 1.5.10 reaches gameplay on Apple M3 Pro.
- The tested build is native ARM64 Debug.
- Sun shafts are occluded by the indoor walls in the reference scene.
- The desk-lamp volumetric cone is clipped to its spotlight frustum.
- A 60 FPS limit substantially reduces observed temperature compared with running at the 120 Hz display rate.

## Known limitations

- The renderer is still OpenGL; Metal work has not started.
- Full-game completion, every level transition, and save compatibility have not been validated.
- A `CPHSimpleCharacter::PhDataUpdate` physics assertion has been observed separately from the renderer crash.
- The render-graph sorting fix is not yet confirmed by an extended play session.
- Missing UI texture/tutorial messages can still appear when private Clear Sky data or localization overrides are incomplete.
- macOS 27 beta behavior may differ from supported stable macOS releases.
- Performance and temperature observations are preliminary and are not controlled benchmarks.

## Private local harness

The development machine has a `workspace` sibling directory containing build wrappers, an `.app` launcher, game data, logs, saves, and per-run diagnostic snapshots. It is deliberately outside this repository because it contains private paths and legally acquired game files.
