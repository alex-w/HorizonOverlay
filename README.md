# HorizonOverlay for Stellarium

HorizonOverlay is a C++/Qt plug-in for Stellarium that draws a local obstruction horizon above the normal sky view. It is intended for observers who want to visualize buildings, trees, terrain, or other site-specific obstructions without replacing the active Stellarium landscape.

The plug-in keeps Stellarium's original landscape, foreground, and true horizon untouched. It adds a separate overlay from an `Az Alt` obstruction profile, with optional fill, outline, colors, opacity controls, and direct point editing inside Stellarium.

## Project Status

| Item | Status |
| --- | --- |
| Primary target | Stellarium `25.1.0` / Qt `6.8.x` |
| Main tested platform | macOS arm64 |
| Windows/Linux | Build workflow prepared, local runtime testing still needed |
| Distribution model | Dynamic user-directory Stellarium plug-in |
| ABI note | Rebuild for each Stellarium version |

Dynamic Stellarium plug-ins are ABI-bound to the host application. A binary built for one Stellarium version should not be treated as compatible with every other version.

## Features

- Draws a local obstruction outline in horizontal `Az/Alt` coordinates.
- Optionally fills the region between true horizon `Alt=0` and the obstruction height.
- Keeps the original Stellarium landscape unchanged.
- Provides a settings window for visibility, fill, outline, opacity, color, file selection, and reload.
- Supports direct hand editing: hold `Shift` and drag with the left mouse button to add or update points, and hold `Shift` while dragging with the right mouse button to erase nearby points.
- Automatically cleans hand-drawn profiles by merging nearby points, simplifying small jitter, and smoothing local spikes.
- Uses a GPU shader screen-space mask for very wide fields of view.
- Falls back to a CPU screen mask when shader setup is unavailable or the table exceeds the shader sample limit.
- Includes plug-in-local translations for 10 major languages.

## Quick Install

Copy the `HorizonOverlay` module folder into Stellarium's user module directory.

macOS:

```text
~/Library/Application Support/Stellarium/modules/HorizonOverlay/
```

Windows:

```text
%APPDATA%\Stellarium\modules\HorizonOverlay\
```

Linux:

```text
~/.stellarium/modules/HorizonOverlay/
```

Expected module contents:

```text
HorizonOverlay/
├── libHorizonOverlay.dylib   # macOS
├── libHorizonOverlay.so      # Linux
├── HorizonOverlay.dll        # Windows, name may also be libHorizonOverlay.dll depending on toolchain
├── config.ini
├── obstructions.txt
└── translations/
    └── horizonoverlay/
        ├── de.qm
        ├── es.qm
        ├── fr.qm
        ├── it.qm
        ├── ja.qm
        ├── ko.qm
        ├── pt_BR.qm
        ├── ru.qm
        ├── zh_CN.qm
        └── zh_TW.qm
```

On macOS, the helper script can install a locally built library:

```bash
./scripts/install-to-user-modules.sh /path/to/libHorizonOverlay.dylib
```

The script creates default `config.ini` and `obstructions.txt` only when those files do not already exist, so existing user settings are not overwritten.

## Using the Plug-in

1. Install the module folder.
2. Start Stellarium.
3. Open `Configuration` -> `Plugins`.
4. Enable `Horizon Overlay`.
5. Restart Stellarium if Stellarium asks for it.
6. Open the HorizonOverlay settings window.
7. Choose or reload an obstruction table, then adjust visibility, fill, outline, color, and opacity.

After the plug-in is loaded, a Horizon Overlay button appears in the bottom
plug-in toolbar group. Use it to toggle the overlay quickly; use the settings
dialog to show or hide this toolbar button, adjust colors and opacity, edit the
profile, and inspect the performance monitor.

To draw a profile directly in Stellarium:

1. Open HorizonOverlay settings.
2. Enable `Edit points`.
3. Hold `Shift` and drag with the left mouse button in the sky to add or update obstruction points.
4. Hold `Shift` and drag with the right mouse button near existing points to erase them.
5. Use `Clear points` to clear the current profile.
6. Use `Save table` to write the cleaned profile to the selected obstruction file.

Hand-drawn profiles are cleaned automatically. The plug-in merges very close azimuth samples, removes small nearly-collinear jitter, and smooths isolated spikes while preserving larger intentional changes such as roof lines, tree tops, and terrain steps.

## Obstruction Table Format

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

`Az` and `Alt` are degrees. `.txt`, `.hrz`, and simple `.csv` files are accepted. The importer also recognizes common column names such as `az`, `azimuth`, `bearing`, `direction`, `alt`, `altitude`, `elevation`, and `height`, so multi-column CSV files from measurement apps can be imported without manually deleting unrelated columns.

Azimuth follows the common compass convention:

- north: `0`
- east: `90`
- south: `180`
- west: `270`

See [docs/obstructions-format.md](docs/obstructions-format.md) for the full format, including CSV headers, comments, separators, duplicate azimuth handling, and automatic `0/360` closure.

## Rendering Modes

| Field of view | Fill renderer |
| --- | --- |
| `FOV <= 150 deg` | Legacy 3D fill mesh |
| `FOV > 150 deg` | GPU shader screen-space mask plus screen-space outline |
| Shader unavailable | CPU screen-mask fallback |
| More than `256` samples | CPU screen-mask fallback |

The shader path is the preferred wide-field renderer. It is enabled before the nominal `180 deg` limit because some wide projections can already make ordinary 3D mesh segments cross projection boundaries near the screen edge. In wide fields the plug-in keeps both the filled obstruction and the outline in screen space, avoiding ordinary 3D mesh or great-circle segments crossing the projection boundary behind the camera. The CPU path is a safety fallback for unsupported OpenGL/shader environments and oversized obstruction tables.

The settings window includes a small performance readout for local testing:

```text
Performance: 0.350 ms last, 0.420 ms avg, 1.100 ms max | FoV 220.0 deg | shader mask | 87 samples
```

This measures only HorizonOverlay's own `draw()` cost, not Stellarium's full frame time. The path field reports `legacy mesh`, `shader mask`, `cpu mask`, `line only`, or `disabled`, so high-FOV tests can confirm whether the fast shader path or fallback path is active.

Developer notes for the Stellarium-style dialog, toolbar action, local build,
and upstream-readiness checklist are in
[`docs/official-plugin-readiness.md`](docs/official-plugin-readiness.md).

## Build

Stellarium does not provide a stable external plug-in SDK. Build HorizonOverlay inside a matching Stellarium source tree.

1. Download the Stellarium source matching the target application version.
2. Copy `plugin/HorizonOverlay` into `plugins/HorizonOverlay` in the Stellarium source tree.
3. Add this line to Stellarium's `plugins/CMakeLists.txt`:

   ```cmake
   ADD_SUBDIRECTORY(HorizonOverlay)
   ```

4. Configure Stellarium with dynamic plug-ins enabled and the same Qt version used by the target app.
5. Build the `HorizonOverlay` target.

The plug-in CMake uses standard Qt package discovery:

```cmake
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets OpenGL)
```

It should not rely on local absolute paths such as `/tmp/...` or `/Users/...`.

On macOS, the dynamic module is intentionally emitted as a `.dylib`. Stellarium loads dynamic plug-ins at runtime and provides host symbols from the application process, so the target uses the conventional macOS dynamic lookup behavior for this plug-in style.

## GitHub Actions

This repository includes `.github/workflows/build.yml`. The workflow is set up to build HorizonOverlay against Stellarium `v25.1` and Qt `6.8.1` on:

- macOS
- Linux
- Windows

Each artifact is packaged as an installable module folder containing the plug-in binary, default config, default obstruction table, README, documentation, and compiled translations.

The workflow is useful for release packaging, but every generated binary should still be tested with the matching Stellarium version before being published as a stable release.

## Translations

HorizonOverlay ships a plug-in-local translation domain named `horizonoverlay`.

Runtime settings UI translations are loaded from:

```text
HorizonOverlay/translations/horizonoverlay/<locale>.qm
```

The settings dialog follows Stellarium's current application language and is rebuilt when Stellarium emits `languageChanged`.

Bundled translations:

- Simplified Chinese: `zh_CN`
- Traditional Chinese: `zh_TW`
- German: `de`
- Spanish: `es`
- French: `fr`
- Italian: `it`
- Japanese: `ja`
- Korean: `ko`
- Brazilian Portuguese: `pt_BR`
- Russian: `ru`

English is the fallback language and does not require a `.po` file.

Dynamic plug-in metadata has a known limitation: Stellarium translates external plug-in names and descriptions through its built-in global translation catalog. A user-directory dynamic plug-in cannot reliably extend that global catalog at runtime, so the plug-in-list name and description remain in English unless HorizonOverlay is later bundled into Stellarium itself.

## Configuration

Default `config.ini`:

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

## Repository Layout

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
│               └── *.po
└── scripts/
    └── install-to-user-modules.sh
```

## Known Limits

- Dynamic plug-ins must be rebuilt for the Stellarium version they are loaded into.
- The shader path accepts up to `256` obstruction samples; larger tables automatically use CPU fallback.
- The shader path depends on the Qt/OpenGL environment provided by Stellarium.
- The built-in performance readout measures CPU-side plug-in draw time; it is intended for quick local comparisons, not full GPU profiling.
- macOS is the primary tested platform so far.
- Windows and Linux artifacts still need runtime verification on real Stellarium installs.

## References

- [Stellarium plug-ins](https://stellarium.org/doc/25.1/plugins.html)
- [StelModule](https://stellarium.org/doc/25.1/classStelModule.html)
- [StelPainter](https://stellarium.org/doc/25.1/classStelPainter.html)
