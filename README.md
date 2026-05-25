# Stellarium HorizonOverlay

HorizonOverlay is a Stellarium C++/Qt plug-in that draws a local obstruction horizon over the normal sky view. It does not modify the active landscape, grass foreground, or Stellarium's own horizon; it adds a separate visual overlay from an `Az Alt` table.

The current target is Stellarium `25.1.0` with Qt `6.8.x`. Other versions may work only after rebuilding against that exact Stellarium version, because dynamic Stellarium plug-ins are ABI-bound to the host application.

## Features

- Reads a plain-text local obstruction table.
- Draws an obstruction outline in Alt/Az coordinates.
- Optionally fills the area between true horizon `Alt=0` and the obstruction height.
- Provides a settings window for visibility, fill, outline, opacity, colors, file selection, and reload.
- Uses a shader screen-space mask for very wide fields of view.
- Falls back to a CPU screen mask when shader setup is unavailable or the table exceeds the shader sample limit.
- Keeps a simple legacy 3D fill path for normal fields of view.

## Layout

```text
.
├── data/
│   ├── config.ini
│   └── obstructions.txt
├── docs/
│   └── obstructions-format.md
├── plugin/
│   └── HorizonOverlay/
│       ├── CMakeLists.txt
│       ├── src/
│       │   ├── CMakeLists.txt
│       │   ├── HorizonOverlay.cpp
│       │   └── HorizonOverlay.hpp
│       └── translations/
│           └── horizonoverlay/
│               ├── POTFILES.in
│               ├── horizonoverlay.pot
│               ├── ja.po
│               └── zh_CN.po
└── scripts/
    └── install-to-user-modules.sh
```

## Obstruction Table

Minimal example:

```text
Az Alt
0 16
45 24
90 34
180 10
270 46
360 16
```

`Az` and `Alt` are degrees. Azimuth uses the common compass convention: north is `0`, east is `90`, south is `180`, and west is `270`.

See [docs/obstructions-format.md](docs/obstructions-format.md) for the full format, including comments, separators, duplicate azimuth handling, and automatic `0/360` closure.

## Rendering Modes

- `FOV <= 180°`: uses the simple legacy 3D fill mesh.
- `FOV > 180°`: uses the GPU shader screen-space mask by default.
- Shader fallback: if OpenGL/shader setup fails, or if the obstruction table has more than `256` samples, the plug-in automatically uses the CPU screen mask instead.

The shader path should be much faster than the CPU fallback. The CPU path exists as a safety net and for oversized obstruction tables, not as the preferred normal rendering mode.

## Build

Stellarium does not provide a stable external plug-in SDK. Build the plug-in inside a matching Stellarium source tree.

1. Download the Stellarium source matching the target app version.
2. Copy `plugin/HorizonOverlay` to `plugins/HorizonOverlay` in the Stellarium source tree.
3. Add `ADD_SUBDIRECTORY(HorizonOverlay)` to Stellarium's `plugins/CMakeLists.txt`.
4. Configure Stellarium with dynamic plug-ins enabled and the same Qt version used by the target app.
5. Build the `HorizonOverlay` target.

On macOS the dynamic module is intentionally emitted as a `.dylib`. The CMake file uses standard Qt package discovery:

```cmake
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets OpenGL)
```

It does not rely on local absolute paths such as `/tmp/...` or `/Users/...`.

## GitHub Actions Builds

This repository includes `.github/workflows/build.yml`. On push, pull request, or manual workflow dispatch it builds HorizonOverlay against Stellarium `v25.1` and Qt `6.8.1` on:

- macOS: uploads `HorizonOverlay-macos`
- Linux: uploads `HorizonOverlay-linux`
- Windows: uploads `HorizonOverlay-windows`

Each artifact contains a `HorizonOverlay/` module folder with the plug-in binary, `config.ini`, and `obstructions.txt`, plus the README and format documentation.
Artifacts also include compiled plug-in-local translations under `HorizonOverlay/translations/horizonoverlay/`.

## Install

Copy the artifact's `HorizonOverlay` folder into Stellarium's user module directory.

macOS:

```text
~/Library/Application Support/Stellarium/modules/HorizonOverlay/
```

Linux:

```text
~/.stellarium/modules/HorizonOverlay/
```

Windows:

```text
%APPDATA%\Stellarium\modules\HorizonOverlay\
```

Expected module contents:

```text
HorizonOverlay/
├── libHorizonOverlay.dylib   # macOS
├── libHorizonOverlay.so      # Linux
├── libHorizonOverlay.dll     # Windows
├── config.ini
├── obstructions.txt
└── translations/
    └── horizonoverlay/
        ├── ja.qm
        └── zh_CN.qm
```

On macOS you can also use the helper script:

```bash
./scripts/install-to-user-modules.sh /path/to/libHorizonOverlay.dylib
```

The script installs the library as `libHorizonOverlay.dylib`. It creates default `config.ini` and `obstructions.txt` only when those files do not already exist, so user settings are not overwritten.
If Qt's `lconvert` is available, the script also compiles and installs bundled plug-in translations.

## Translations

Source strings follow Stellarium's built-in plug-in convention:

- `N_("...")` marks plug-in metadata strings for extraction.
- `q_("...")` is used as the normal Stellarium translation fallback.
- `translateUi("...")` marks plug-in-local runtime UI strings.
- `plugin/HorizonOverlay/translations/horizonoverlay/POTFILES.in` lists source files for `xgettext`.

Because this repository ships HorizonOverlay as a user-directory dynamic plug-in, it also builds a small plug-in-local translation domain named `horizonoverlay`. At runtime the module first tries:

```text
HorizonOverlay/translations/horizonoverlay/<locale>.qm
```

The selected locale follows Stellarium's application language, not the operating system language. For example, a Japanese Stellarium UI loads `ja.qm`, while a Simplified Chinese UI loads `zh_CN.qm`. If no matching plug-in-local translation exists, HorizonOverlay falls back to Stellarium's global `q_()` translations and then English.

Simplified Chinese and Japanese are currently provided as `zh_CN.po` and `ja.po`, and compiled to `zh_CN.qm` / `ja.qm` in GitHub Actions.

## Configuration

Default config:

```ini
[overlay]
visible=true
drawLine=true
drawFill=true
lineColor=#ffcc66
fillColor=#ff7a18
lineOpacity=0.95
fillOpacity=0.22
lineWidth=2.0
obstructionFile=obstructions.txt
```

The settings window writes changes to the module directory's `config.ini`. After editing the obstruction table manually, click `Reload table` in the settings window.

## Known Limits

- Dynamic plug-ins must be rebuilt for the Stellarium version they are loaded into.
- The shader path accepts up to `256` obstruction samples; larger tables automatically use CPU fallback.
- The shader path depends on the Qt/OpenGL environment provided by Stellarium.
- Only Stellarium `25.1.0` / Qt `6.8.x` has been the primary target so far.

## References

- [Stellarium plug-ins](https://stellarium.org/doc/25.1/plugins.html)
- [StelModule](https://stellarium.org/doc/25.1/classStelModule.html)
- [StelPainter](https://stellarium.org/doc/25.1/classStelPainter.html)
