# Building and running Clear Sky on macOS

This document describes the current local development setup for the Clear Sky macOS fork. It builds a native ARM64 Debug executable and runs it against a legally acquired installation of S.T.A.L.K.E.R.: Clear Sky 1.5.10.

The repository contains engine source code only. It does not contain GOG installers, extracted game archives, textures, models, audio, scripts, saves, or other proprietary game assets.

## Verified configuration

- MacBook Pro with Apple M3 Pro
- 18 GB unified memory
- macOS 27 beta 8
- Apple Clang from the Command Line Tools
- GOG Clear Sky 1.5.10, English
- ARM64 Debug build
- OpenGL renderer (`renderer_r3` in the Clear Sky configuration)

macOS 27 is a beta system, so results can change between OS builds. The CMake deployment target remains macOS 14.0.

## Workspace layout

The local helper scripts expect the engine repository and private game workspace to be siblings:

```text
game mods/
├── xray-16/                    # this Git repository
└── SCS/                        # private, ignored game workspace
    ├── build/
    │   └── openxray-dev-arm64/
    ├── play/
    │   ├── fsgame.ltx
    │   ├── gamedata/
    │   └── _appdata_/
    ├── runtime/
    │   └── openxray-dev.app/
    └── debug/
```

Do not commit the `SCS` directory or any game data to this repository.

## Prerequisites

Install Apple's Command Line Tools:

```sh
xcode-select --install
```

Install the build tools and libraries with Homebrew:

```sh
brew install cmake ninja ccache sdl2-compat openal-soft jpeg-turbo libogg libvorbis theora lzo
```

Clone the fork with all submodules:

```sh
git clone --recursive --branch codex/metal-backend \
  https://github.com/illiasemyvolos/openxray-clear-sky-macos.git xray-16
```

For an existing checkout, synchronize the submodule URLs and fetch the pinned revisions:

```sh
git submodule sync --recursive
git submodule update --init --recursive
```

The GameSpy submodule currently points to the companion macOS fork at [illiasemyvolos/gamespy-macos](https://github.com/illiasemyvolos/gamespy-macos).

## Configure the Debug build

The commands below match the verified local build. Set the two paths for your workspace:

```sh
ENGINE_ROOT="/path/to/game mods/xray-16"
SCS_ROOT="/path/to/game mods/SCS"

/opt/homebrew/bin/cmake \
  -S "$ENGINE_ROOT" \
  -B "$SCS_ROOT/build/openxray-dev-arm64" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_UNITY_BUILD=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)" \
  '-DCMAKE_PREFIX_PATH=/opt/homebrew;/opt/homebrew/opt/openal-soft' \
  -DCMAKE_C_COMPILER_LAUNCHER=/opt/homebrew/bin/ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=/opt/homebrew/bin/ccache \
  -DCPACK_GENERATOR=ZIP
```

`CMAKE_UNITY_BUILD=OFF` keeps Debug stack traces and incremental source-level work easier to interpret.

## Build

```sh
SCS_ROOT="/path/to/game mods/SCS"

/opt/homebrew/bin/cmake \
  --build "$SCS_ROOT/build/openxray-dev-arm64" \
  --parallel 3
```

The executable and engine libraries are written to:

```text
xray-16/bin/arm64/Debug/
```

The three-job limit is intentional for the 18 GB development machine. It leaves memory and thermal headroom while compiling the large `xrGame` target.

## Prepare the private game directory

Extract your own Clear Sky 1.5.10 installation into the private `SCS` workspace and configure `play/fsgame.ltx` so the engine can find:

- game archives or extracted game data;
- `play/gamedata` overrides;
- `play/_appdata_` for settings, logs, screenshots, and saves.

The working installation uses English localization. Enhanced Edition assets are not part of this baseline.

## Development app wrapper

The verified setup launches the Debug executable through a small `.app` wrapper. Its executable is a symbolic link to the current build:

```text
SCS/runtime/openxray-dev.app/Contents/MacOS/xr_3da
  -> xray-16/bin/arm64/Debug/xr_3da
```

Create the wrapper after setting `ENGINE_ROOT` and `SCS_ROOT`:

```sh
ENGINE_ROOT="/path/to/game mods/xray-16"
SCS_ROOT="/path/to/game mods/SCS"

mkdir -p "$SCS_ROOT/runtime/openxray-dev.app/Contents/MacOS"
mkdir -p "$SCS_ROOT/runtime/openxray-dev.app/Contents/Resources"
ln -sfn "$ENGINE_ROOT/bin/arm64/Debug/xr_3da" \
  "$SCS_ROOT/runtime/openxray-dev.app/Contents/MacOS/xr_3da"

cat > "$SCS_ROOT/runtime/openxray-dev.app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>English</string>
    <key>CFBundleExecutable</key>
    <string>xr_3da</string>
    <key>CFBundleIdentifier</key>
    <string>org.openxray.clear-sky.dev</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>OpenXRay Clear Sky Dev</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.6-dev</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
</dict>
</plist>
PLIST

printf 'APPL????\n' > "$SCS_ROOT/runtime/openxray-dev.app/Contents/PkgInfo"
```

Launching through the bundle gives the process normal macOS application activation and window behavior while keeping the binary linked to the latest incremental build.

## Launch

Run from the configured `play` directory:

```sh
SCS_ROOT="/path/to/game mods/SCS"

cd "$SCS_ROOT/play"
"$SCS_ROOT/runtime/openxray-dev.app/Contents/MacOS/xr_3da" \
  -cs -fsltx fsgame.ltx -nointro
```

The local workspace also provides `Configure OpenXRay Dev.command`, `Build OpenXRay Dev.command`, and `Launch Dev Clear Sky.command` wrappers for these commands.

Do not launch the application bundle without the Clear Sky arguments: `-cs` selects the game mode and `-fsltx` selects the private filesystem configuration.

## Safe default frame limit

The current local baseline uses:

```text
rs_fps_limit 60
rs_fps_limit_in_menu 60
rs_v_sync on
```

At 120 FPS the M3 Pro test machine reached about 82 °C. With a 60 FPS limit it stayed near 55 °C and the fans did not audibly start. These are user-observed values rather than controlled hardware measurements, but the frame limit is a useful development default.

## Release assets

Do not publish a build that bundles Clear Sky data. A distributable engine package must require the user to supply their own game installation and must retain the licenses and copyright notices already present in this repository.
