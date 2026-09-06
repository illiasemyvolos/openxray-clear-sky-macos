# Debugging Clear Sky on macOS

Use a fixed scene, save, camera angle, resolution, power state, and frame limit when comparing renderer changes. Change one setting or one code path per run.

## Fast development loop

1. Close the game through its menu.
2. Build the changed targets.
3. Launch the Debug executable through the development app wrapper.
4. Load the same save and reproduce the scene.
5. Record the engine commit, FPS, visual result, and logs.

Incremental build:

```sh
SCS_ROOT="/path/to/game mods/SCS"
/opt/homebrew/bin/cmake --build "$SCS_ROOT/build/openxray-dev-arm64" --parallel 3
```

## Useful console settings

Open the in-game console with the grave/tilde key while using an English keyboard layout.

```text
rs_fps on
rs_fps_limit 60
rs_fps_limit_in_menu 60
rs_v_sync on
```

For a sun-shaft comparison, test separate launches with:

```text
r2_sun_shafts st_opt_medium
r2_sun_shafts st_opt_off
```

Restart between variants if the shader or render state may be cached. Disabling an effect is a diagnostic control, not a renderer fix.

## LLDB

Start LLDB from the game directory so relative filesystem paths resolve correctly:

```sh
ENGINE_ROOT="/path/to/game mods/xray-16"
SCS_ROOT="/path/to/game mods/SCS"

cd "$SCS_ROOT/play"
xcrun lldb -- "$ENGINE_ROOT/bin/arm64/Debug/xr_3da" \
  -cs -fsltx fsgame.ltx -nointro
```

At the LLDB prompt:

```text
run
bt all
```

Useful follow-up commands:

```text
thread list
thread backtrace all
frame variable
register read
```

The local `LLDB Dev Clear Sky.command` wrapper starts this session with the development app executable.

## Logs and captures

The private workspace stores runtime files under:

```text
SCS/play/_appdata_/logs/
SCS/play/_appdata_/launcher-dev.log
SCS/play/_appdata_/screenshots/
SCS/play/_appdata_/savedgames/
```

The local diagnostic harness creates one directory per run under `SCS/debug/sessions/`. A session can contain:

- `metadata.json`: binary path, engine commit, macOS version, arguments, and exit code;
- `console.log`: combined stdout and stderr;
- `process.tsv`: process CPU and resident memory sampled every five seconds;
- `before/` and `after/`: configuration, logs, and save snapshots;
- `notes.md`: scene, change, FPS, temperature, visual result, and screenshot name.

`process.tsv` does not measure GPU utilization, FPS, or temperature. CPU can exceed 100% because macOS reports usage across multiple cores.

## Known failure signatures

### Depth-only Apple OpenGL pipeline

The original macOS Debug build could stop while a sun shadow pass rendered a skinned mesh:

```text
CSkeletonX::_Render
CSkeletonX_PM::Render
R_dsgraph_structure::render_graph
render_sun::render
```

Apple OpenGL rejects a separable draw pipeline without a fragment stage. The current branch attaches the input-free `dumb` fragment shader when X-Ray requests a null pixel shader for a depth-only pass.

### Physics assertion

This stack is a separate physics-side failure and should not be grouped with the renderer pipeline issue:

```text
CPHSimpleCharacter::PhDataUpdate
CPHWorld::Step
CPHWorld::FrameStep
```

Capture the full log, active save, and `bt all` when it repeats. Continuing past an assertion can help classify it, but that run should not be used as a clean graphics comparison.

### Missing UI resources

Messages such as `CAN'T FIND TEXTURE` or `CAN'T FIND TUTORIAL` usually point to the private game-data layout or localization overrides. Check `fsgame.ltx` and extracted assets before treating these as renderer defects.

## Graphics regression scenes

The initial indoor Clear Sky scene revealed two useful regressions:

- sun shafts visible through solid walls;
- a volumetric desk-lamp cone extending outside its spotlight frustum.

Both are corrected in the current OpenGL baseline. Keep screenshots and a save for these scenes because they will become comparison cases for the Metal renderer.

## Reporting a result

Record at least:

```text
Engine commit:
Build type:
macOS build:
Scene/save:
Renderer and settings:
Frame limit:
Change under test:
FPS:
Temperature and sensor:
Visual result:
Log path:
Screenshot path:
```

Never attach proprietary game assets or saves containing redistributed game data to the public repository.
