# Obstruction Table Format

HorizonOverlay reads a plain text table. Each data row describes the obstruction altitude at one azimuth. The same parser is used for `.txt`, `.hrz`, and simple `.csv` files.

```text
Az Alt
0 16
45 24
90 34
180 10
270 46
360 16
```

## Fields

- `Az`: azimuth in degrees. North is `0`, east is `90`, south is `180`, and west is `270`.
- `Alt`: obstruction altitude in degrees. `0` means the true horizon; positive values mean buildings, trees, terrain, or other local obstructions block the sky up to that altitude.

## Supported Imports

The simplest import is still a two-column table:

```text
0 16
45 24
90 34
```

CSV and semicolon-separated rows are also accepted:

```csv
azimuth,altitude
0,16
45,24
90,34
```

Multi-column files are accepted when a header row identifies the azimuth and altitude columns:

```csv
name,bearing,elevation,note
north roof,0,16,garage
east trees,90,34,tree line
south wall,180,10,low wall
```

Recognized azimuth column names include `az`, `azi`, `azimuth`, `bearing`, `direction`, `compass`, and `heading`.

Recognized altitude column names include `alt`, `altitude`, `elevation`, `elev`, `height`, `horizon`, `horizon_alt`, `obstruction`, `obstruction_alt`, and `obstruction_altitude`.

## Parsing Rules

- Blank lines are ignored.
- Non-numeric header lines such as `Az Alt` are ignored.
- Spaces, tabs, commas, and semicolons can be used as separators.
- `#` starts a comment. Everything after `#` on that line is ignored.
- Degree-suffixed values such as `45deg` or `12°` are accepted as numeric values.
- Azimuth values are normalized to `0..360`.
- A positive azimuth that is an exact multiple of `360` is kept as `360`, so `360 16` can explicitly close the table.
- Altitude values are clamped to `-90..90`.
- If the same azimuth appears more than once, the later row overrides the earlier row.
- If `0` is missing, the first sample's altitude is copied to `0`.
- If `360` is missing, the altitude at `0` is copied to `360`.

## Shader Limit

The GPU shader fill path accepts up to `256` parsed obstruction samples. If the table has more samples, HorizonOverlay keeps working and automatically uses the CPU screen-mask fallback for wide-field fill rendering.

## Example

```text
# East-side buildings are high, south is lower.
Az Alt
0   12
45  25
90  38
135 22
180 8
225 14
270 19
315 15
360 12
```
