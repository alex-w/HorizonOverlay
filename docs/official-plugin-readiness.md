# Horizon Overlay Official Plug-in Readiness Notes

This note tracks the local work needed to move Horizon Overlay from a dynamic
user plug-in toward a Stellarium-style official plug-in.

## What Changed Locally

- The configuration window now uses a `StelDialog` subclass with a Qt Designer
  `.ui` file, matching the common Stellarium plug-in structure.
- The bottom toolbar has a Horizon Overlay toggle button in the official
  `065-pluginsGroup` group. Left click toggles the overlay; the secondary action
  opens the settings dialog.
- The settings dialog includes a `Show button on bottom toolbar` option. It is
  enabled by default and persists in the user-module `config.ini`.
- The toolbar icons are bundled through `resources/HorizonOverlay.qrc`, so the
  dynamic and future static plug-in builds use the same resource path.
- Overlay visibility is exposed as the `horizonOverlayVisible` Qt property so
  `StelAction` and the toolbar button stay synchronized with the real render
  state.
- Settings continue to use the user-module `config.ini` for dynamic plug-in
  compatibility. Color values keep their leading `#`, so restart persistence for
  `#RRGGBB` values is preserved.
- The performance monitor remains in the settings dialog as a diagnostic panel.
  It reports the last, average, and maximum Horizon Overlay draw cost plus FoV,
  active fill path, and sample count.

## Local Build And Install

Build against the local Stellarium v26.1 cache:

```bash
rsync -a --delete plugin/HorizonOverlay/ \
  /Users/songzihan/Documents/Codex/.build-cache/horizonoverlay/stellarium-26.1/plugins/HorizonOverlay/
cmake --build /Users/songzihan/Documents/Codex/.build-cache/horizonoverlay/build-stellarium-26.1 \
  --target HorizonOverlay -j 4
```

Install the dynamic library into the local Stellarium user module directory:

```bash
./scripts/install-to-user-modules.sh \
  /Users/songzihan/Documents/Codex/.build-cache/horizonoverlay/build-stellarium-26.1/plugins/HorizonOverlay/src/libHorizonOverlay.dylib
```

The install script does not overwrite an existing `config.ini` or
`obstructions.txt`.

## Manual Verification Checklist

- Open Stellarium, enable `Horizon Overlay`, and restart if requested.
- Confirm the bottom toolbar button appears in the plug-in button group.
- Toggle the toolbar button and confirm the overlay visibility changes.
- Open the settings dialog from the plug-in list or the toolbar secondary action.
- Change fill color, line color, opacity, line width, and obstruction path.
- Quit and restart Stellarium, then confirm those settings persist.
- Test normal FoV and high FoV. In high FoV, check the Performance tab for the
  active path: `shader mask`, `cpu mask`, `legacy mesh`, `line only`, or
  `disabled`.

## Official Upstream Path

- Keep the first upstream proposal small: local obstruction overlay, official
  dialog structure, toolbar toggle, settings persistence, and documented high
  FoV fallback behavior.
- Before opening a pull request, start with a GitHub Discussion or issue in the
  Stellarium repository describing the use case, screenshots, and why this is a
  plug-in rather than a landscape replacement.
- If maintainers are interested, prepare a draft pull request against the
  official source tree using the static plug-in build path.
- For official bundling, expect to migrate dynamic user-module configuration to
  Stellarium's preferred `QSettings`-backed configuration pattern and refresh
  translation catalogs from the `.ui` and dialog source files.
