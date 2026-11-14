# ColorMax changelog

ColorMax is a fork of PrusaSlicer that prints colored models as layer mixes of the filaments in a
multi-tool printer. The version shows in the title bar ("PrusaSlicer-2.9.7+ColorMax-0.3.1"); it is set
in `version.inc` (`COLORMAX_VERSION`), and releases are tagged `colormax-<version>` on GitHub.
Details: [doc/ColorMax.md](doc/ColorMax.md).

## 0.3.1 - 2026-11-13

- Preview runs only on request; a settings change keeps the previous preview, grayed, and Apply to model
  is available only with a current preview. After Preview, a note lists loaded spools no color uses.
- Works with any number of tools (tested with 2, 4, 8 and 20); spools on tools the printer doesn't
  have become "Not loaded".
- Color range chart: the model's colors as designed / closest printable pairs under the chart.
- 3D editor: Original / ColorMax view switch on converted models (and a fix for flashing colors and a
  crash in the Original view).
- Filament mixing roles: Mix / Accent / Solid only (replaces Mixable + Glitter/silk). The ColorMix
  glitters are accents: calibrated with a targeted 21-swatch set, not used in conversions on their own.
  Solid-only and accent filaments get a solid swatch on calibration plates.
- Calibration: warning when a filament set needs more mixed swatches than PrusaSlicer's 255 painting
  colors allow; plate files open with their own swatch list.
- Scanning: advice for scanners without an advanced mode (leave Brightness alone: it subtracts from every
  pixel); the reader warns when swatches are crushed to black.
- The Prusament PLA ColorMix set ships with a reference calibration: CORE One INDX, 0.1 mm layers, matte
  white core, 240 swatches incl. Cyan (held-out median 1.6 dE, P90 3.9).

## 0.3.0 - 2026-11-13

The first numbered version (the earlier builds were labeled "+ColorMax" only). It contains:

- Conversion of painted / colored models (colored 3MF, Bambu-style painting) into layer-mixed
  recipes for up to 8 tools, with a preview before anything changes, dual walls, patterned tops, white
  core, clean-up of specks and tool-change optimization.
- Prompt to open ColorMax when a colored model is loaded.
- Color calibration from calibration plates, read from a flatbed scan (recommended) or a photo; reprints
  of only the missing swatches; settings locked to the plate.
- Filament sets: editor, lookup in public filament databases (filamentcolors.xyz measured colors and TD,
  CC BY 4.0; SpoolmanDB catalog, MIT), "Not loaded" spools.
- Strengths of uncalibrated filaments from their TD (transmission distance).
- Color range chart: what a filament set can print, and where the model's colors fall.
