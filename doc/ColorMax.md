# ColorMax Engine (PrusaSlicer fork)

Internally (source files, class names, JSON keys, file formats) the engine is called *ColorMix*,
e.g. `src/libslic3r/Feature/ColorMix` and the filament-set format `prusaslicer-colormix-bundle`.
Elsewhere in this document, *ColorMix* means Prusa's Prusament PLA ColorMix filaments.

The ColorMax Engine turns a colored model into layer-mixed prints with the spools you have
loaded. It extends PrusaSlicer 2.9.7's "Virtual Extruders - Color Mixing":

| | Stock 2.9.7 | ColorMax Engine |
|---|---|---|
| Colored 3MF import | Bambu per-triangle paint only | + standard 3MF `basematerials` colors, Bambu/Orca per-part filaments and `filament_colour` |
| Recipe choice | Manual, one virtual extruder at a time | Automatic for the whole model |
| Layer order | Derived from ratios (3 % rounding) | Explicit layer word per virtual extruder, striping rules, banding score |
| Color prediction | `prusa_fdm_mixer` on nominal hex | Same base + your measured swatches + fitted correction |
| Filament sets | n/a | Data-driven bundles (JSON); ships Prusament PLA ColorMix |
| Flat tops | Show whichever filament lands on the last layer | Top-surface patches (8x8 ordered pattern of the recipe) |
| Interior | Mixed like the walls | Optional white core behind the colored walls |
| Layer heights | Uniform | Optional repeating schedule (variable layer thickness) |
| Walls | One word for the whole region | Optional dual walls: outer ring and inner wall follow different words |
| Calibration | n/a | Swatch boards read from a flatbed scan (recommended), a photo, or a colorimeter |
| Undo a conversion | n/a | Original painting is kept; "Revert to original colors" |
| Region cleanup | n/a | Sliver runs, small islands and jagged borders absorbed into neighbours |
| Tool changes | n/a | Estimated per print; recipes and word phases chosen to save changes |

Not implemented: colors from textures or OBJ files (import a colored 3MF instead).

## Using it

1. **Open or import a colored 3MF** (e.g. from Bambu Studio, OrcaSlicer or PrusaSlicer). Its colors
   are kept as the model's *source colors*, and ColorMax offers to convert it. Files with only one
   color (e.g. from a CAD tool) load as plain models, exactly as in stock PrusaSlicer.
2. **Open ColorMax**: from that offer, or with the **ColorMax Engine** button under the filament
   combos (shown with 2 or more extruders).
3. **Check the settings.** The dialog has three sections. **1. Filaments**: the filament set
   (bundle), the color calibration (with **Calibrate...** next to it and a plain-language line on
   how accurate the predictions are; the full fit report is its tooltip) and the tool holding each
   spool. **2. How to print the colors**: the five main switches (*Mix colors*, *Dual walls*,
   *Patterned tops*, *Clean up specks*, *White core*); their parameters, variable layer thickness,
   the tool-change weight and the matching options are under **Advanced settings**. Every control
   has a tooltip.
4. **Preview.** Nothing is computed until you press **Preview** (in **3. Result**): the table shows
   every model color, the mix it will print with (e.g. `Cyan 67% + White 33%  [Cy Cy Wh]`), the
   predicted color and the difference (ΔE2000). A note lists loaded spools that no color uses.
5. **Apply to model** repaints the model with physical tools (pure spools) and new virtual extruders
   (blends), sets the tool colors and stores the techniques in the project. It is available once the
   preview matches the current settings.

Re-run any time: a conversion always starts from the source colors, and the dialog restores the
options used last time (a new project starts from the settings of your last conversion). **Revert
to original colors** restores the painting and the extruder colors from before the first
conversion. Conversions, reverts and the ColorMax settings are part of undo/redo.

### Details: colored imports, touch-ups and undo

- Only files using two or more different colors (or carrying paint) count as colored (two filament
  slots of the same color, e.g. two black spools, count as one). Their colors and each object's
  color slot are recorded and saved with the project, and kept by object through undo/redo,
  objects the loader drops and loading as a multi-part object (each part keeps its object's slot).
- Opening the ColorMax dialog assigns each object (or part) the tool of its slot (tool k, as the
  file assigns it; nothing gets painted). Objects whose slot is beyond the printer's tools keep their
  own extruder and their color as ColorMax data; until converted they print with tool 1, and a
  notification says so. Several basematerials groups each get their own slots.
- Importing a second colored file adds its colors to the palette as ColorMax data only: its parts
  keep their painting and tools exactly as the file assigns them, and record their colors as
  palette slots for the conversion. Parts already on the plater when a colored file arrives keep
  their own colors; every tool and blend id keeps its number as a palette slot (in the extruder's
  current color until the first conversion) and the file's colors get slots after them (a slot is
  reused only for the same color on the same tool, or for the same color in a converted project).
- The source painting is captured by the first conversion, so painting you change before converting
  counts; parts added after a conversion get source colors from their current extruders.
- Paint touch-ups made after a conversion are noticed at the next Preview, which offers to keep them
  (they become source colors; untouched areas keep their design colors). Repainting imported parts
  before a conversion is noticed the same way (compared by what prints).
- Parts imported into a converted project are painted as the conversion would: colors the project
  has print with their blends, new colors with the file's tools until the next conversion (a file
  slot beyond the printer's tools prints with the part's own extruder, also after a Revert).
  Importing the geometry of an already converted project repaints its parts from their source
  colors. Importing only the config of a project leaves the plater's ColorMax data alone.
- A conversion resets extruders (parts, modifiers, layer ranges) that named a replaced blend to
  tool 1. Revert paints every source color with the tool it came from, leaves parts that are all in
  their own extruder's color unpainted again, and restores the project's own virtual extruders and
  the extruder settings naming them.
- Undo/redo also covers the extruder colors a conversion or a calibration plate set (an undo / redo
  across one sets the colors of the state it goes to). New Project clears them and ColorMax's own
  blends (other virtual extruders stay, as in stock). Delete All keeps only the record of the
  printer's colors from before ColorMax (so a later Revert restores those) and brings back the
  project's own blends a conversion replaced.
- Known limits: on an empty plater the file's slots are slot k = tool k, so a plain part added later
  on tool k (or painted with blend k) converts as the file's color k; copies of an imported object
  made before the ColorMax dialog is opened don't get its tool; projects converted by builds that
  kept no conversion record aren't checked for touch-ups.

Techniques (all saved in the 3MF, `Metadata/Prusa_Slicer_colormix.json`). They apply to ColorMax
objects only - converted ones, calibration plates / slope tests, and objects painted with a
ColorMax blend; an ordinary painted object on the same plate slices as in stock PrusaSlicer.
Changing the top / bottom solid layers re-slices objects with a white core or dual walls.

- **White core** - everything deeper than *wall depth* (default 0.9 mm) under the painted outline
  prints with the core tool. Light that passes through the translucent walls is reflected back.
  The core tool is either the tool holding the bundle's White or a tool that holds none of the
  bundle's filaments (an extra spool, e.g. opaque white); it shows as white in PrusaSlicer.
  This applies to conversions and to calibration plates. The top and bottom solid shells (the
  print profile's top/bottom solid layers or minimum shell thickness) keep their color, so flat
  tops and visible bottoms aren't white. Unpainted interior (deeper than the segmentation's painted
  depth, which would print with the object's own extruder) goes to the core as well, except inside
  an unpainted part that reaches the surface (a separate part in its own color).
- **Top-surface patches** - the top *layers* layers of every blended region are tiled with
  *cell* mm squares, each in one of the recipe's filaments (8x8 Bayer thresholds, shifted one
  cell per layer). Which surfaces: *slopes with bands wider than* B mm (default 0.3 for new
  conversions). On a slope at angle a from horizontal a layer of height h shows as a band
  h / sin(a) wide; where that exceeds B, layer mixing separates into stripes, so the surface gets
  patches. With 0.1 mm layers, 0.3 mm means slopes flatter than about 19°. In the slicer: a point
  of layer L is patched when it lies farther than k * dz / tan(cutoff) outside the outline k layers
  up (k = 1..*layers*). Vertical walls never qualify; the outer ring of a layer (the top rim of a wall seen from the
  side) keeps the layer word unless the layer below sticks out there by more than the cutoff step
  (a real slope). B = 0 patches only flat tops (areas more than
  0.45 mm inside the outline near a top, which works out to about 34° at 0.1 mm). Print the
  **slope test** (calibration dialog) to choose B: wedges from 40° down to 5° in layer mixing only;
  the shallowest wedge that still looks even gives B (its band width is listed).
- **Variable layer thickness** - a repeating layer-height schedule above the first layer, e.g.
  `0.12, 0.10, 0.08`. A word now lands on thick and thin layers, so `C M Y` and `C Y M` are
  different colors. Calibrate with the same schedule (calibrations remember theirs, and swatches
  are always weighed on the schedule they were printed with; calibration plates always record an
  explicit schedule, the profile's layer height when none is entered, so a later profile change
  doesn't change how they print). The dialogs warn about heights outside the printer's limits;
  slicing refuses heights outside the
  printer's min/max layer height, more than 16 heights, or a varying schedule with Organic
  supports; the schedule replaces an object's own variable layer height (a warning says so).
  Without a schedule, an object's own variable layer height or height ranges make blends print
  other colors than predicted: slicing and the dialog warn about it.
  With a raft the schedule stays in step with the layer words (object layer L uses height
  L - 1 of the schedule).

- **Dual walls** - a recipe can give the inner wall its own word (`outer ... | inner ...`). On every
  layer the outer ring of the region (*outer wall width*, about one external perimeter) follows the
  outer word and the next ring the inner word; the white core sits behind both. The translucent
  outer wall shows the inner color through it, which adds lighter, darker and in-between colors.
  Flat tops and bottoms (the solid shells) follow the outer word only. The white core always starts
  deeper than the dual-wall rings (at least outer wall width + 0.45 mm).
  The split happens in the multi-material segmentation, so each ring becomes its own painted
  region that prints as one wall line (like PrusaSlicer's own painted borders). Uncalibrated, a
  dual-wall color is predicted as 69 % outer / 31 % inner; calibrate with dual-wall swatches.

- **Region cleanup** (on by default for new conversions) works on the sliced color regions, before
  the techniques above:
  - *smooth borders*: a point whose color appears on no neighbouring layer (single-layer flicker)
    takes the color most of the 5 layers around it have there; details two or more layers tall
    stay. Borders painted along triangle edges flicker from layer to layer; this is a simple,
    smoothness-only cleanup (it doesn't weigh color error);
  - *shortest run*: where a color region runs less than this along a layer's outline (default
    0.3 mm), its part near the surface goes to the neighbour it shares the longest border with;
  - *smallest island*: a region (followed across layers) with less visible surface than this -
    sides plus what shows from above or below, including the face on the bed - (default 0.5 mm²)
    goes to its neighbours. Lower it
    if small painted details disappear.

**Tool changes.** Each tool used on a layer costs one tool change (about 12 s on INDX). Source
colors carry the heights where they are painted, so the converter estimates the changes of the
whole print (sum over layers of tools used - 1, core included) and shows it in the summary.
With *fewer tool changes* > 0 (default 0.5) it then improves the choice by local search:
- every recipe may be replaced by a rotation of its layer word that keeps its weights (same
  predicted color), so different colors put the same filament on the same layers (C C M next to
  C C Y shares the C layers instead of needing C and Y on one layer);
- a color may switch to one of its 16 next-best recipes when the saved changes are worth more
  than the added error: the setting is the mean ΔE2000 one tool change less on every layer is
  worth.
A model color within 3 ΔE2000 of a spool's color - its catalog color or its predicted (measured) one -
prints with that spool, pure (`spool_snap_de`): lettering or details painted in the spools' own colors stay
crisp single filaments, even when calibration moves a spool's measured color away from its catalog color
and a blend would land a little closer.
Also, chosen recipes whose predictions are closer than 1 ΔE2000 are merged (pure filaments
first, then the most used).

The estimate assumes each color prints its word's tool(s) on every layer where it is painted,
and all of a recipe's tools on the top layers under its up-facing triangles. It does not see the
geometry the slicer produces (thin regions, patch zones), so it is a guide, not a count; the
G-code is. Measured on a test figurine (1149 layers, dual walls, CORE One INDX profile):

| Setting | Tool changes in the G-code | Estimated print time |
| --- | --- | --- |
| Cleanup, slope band and tool-change weight off | 2172 | 13 h 24 m |
| Region cleanup only | 2165 | 13 h 20 m |
| Slope band 0.3 mm only | 2074 | 12 h 50 m |
| Defaults (cleanup, band 0.3, tool-change weight 0.5) | 2060 | 13 h 4 m |
| Defaults without top patches | 1968 | 12 h 23 m |

On that model the tool-change weight changes almost nothing (its words have lengths 3 and 4,
which can't stay in step); it helps when several colors use words of the same length at the
same heights (see the converter test).

## Calibration

**Reference calibration.** The Prusament PLA ColorMix set ships with one calibration ("Reference:
CORE One INDX 0.1mm, matte white core"): 240 swatches including dual-wall ones, colored walls 0.9 mm over
a white core printed from a separate opaque matte White, flatbed scanner; held-out median 1.6 dE, P90
3.9. Fitted strengths: Cyan 0.65, Magenta 1.55, Yellow 0.88, White 0.75, Black 1.50, outer wall 75 %.
It covers the five mixing colors; it has no glitter swatches yet.
It is a good start on similar printers; your own calibration (it shows up next to it) is more accurate,
especially with a different core filament or layer height.

**What a plate holds.** Every pure filament that mixes, every pair and triple word up to the longest
pattern (4 layers by default), validation swatches, dual-wall swatches, and extra validation swatches
to fill the last board. Solid-only and accent filaments get one solid swatch each, so their real color
is measured. Each accent also gets a targeted set instead of every combination: 1:1 with every filament
that mixes, 3:1 and 1:3 with White and with Black, and 1:1 with each other accent - 21 swatches for the
two ColorMix glitters (every combination would crowd the regular swatches off the plate).

**PrusaSlicer's color limit.** Each mixed swatch needs its own painting color, and PrusaSlicer has 255,
less one per printer tool: 246 mixes on an 8-tool printer. Over that, validation swatches go first, then
dual-wall ones (the plan says how many). When even the basic swatches don't fit (eight or more
filaments set to *Mix* with the default settings), the plate can't be made: set some filaments to
*Accent* or *Solid only*, or some spools to *Not loaded*.

**Plate files keep their swatches.** A plate file stores its swatch list; while it is open (settings
locked) that list is used, so a plate stays readable and reprintable even when a newer ColorMax plans
different swatches for the same settings.

**ColorMax Engine > Calibrate...** The calibration dialog is laid out as three steps: *1. Print
the calibration plate*, *2. Scan the boards and read them* (plate file, progress per board, the
swatch table), *3. Save the calibration*. When it opens it restores the plate on the plater, or
else opens the last plate file used, so measurements already made are there without loading
anything. Plates (and the slope test) are placed in the middle of the bed, where prints usually
come out best.

1. Choose the bundle, the tools and the layer heights you print with. With 5 spools, single-wall
   swatches are 95 + 10 validation. *Dual-wall swatches* (default *Full*): *Basic* adds the 20 pure
   outer/inner pairs + 10 mixed pairs; *Full* adds 110 (pure pairs, 1:1 outers over pure inners,
   pure outers over 1:1 inners, 1:1:1 outers over pure inners, 10 validation pairs). The last board
   is always filled up with extra validation swatches (single- and dual-wall, alternating) instead
   of empty cells: *Full* gives 240 swatches (45 for validation) on 5 boards (about 11 h at 0.10 mm
   on a CORE One INDX). Boards print side by side in the same layers, so extra boards cost extrusion
   time but almost no extra tool changes.
   *White core* picks the core tool (default: the core tool of the current project, else the
   bundle's White). Use the core tool and filament you print with: the core shows through the
   walls and is part of the measured color.
2. **Create calibration plate** replaces the plater content with calibration *boards*: upright
   196 x 23 x 4 mm frames holding a 16 x 3 grid of 12 x 6 mm swatch cells (48 per board; 6 mm is ~60
   layers at 0.1 mm, so every reading averages many repeats of each layer word). Board *k* has *k*
   dots on its top edge at the left end. The frame prints in Black (for contrast), the cells with the
   chosen core behind them, like real prints. Each cell is a separate part painted in one color;
   such objects are segmented straight from the part slices (exact cell borders) instead of
   projecting the painting onto the layer contours. The plate file (below) holds the board / row /
   column of every swatch.
   All boards are one object ("ColorMax calibration boards"), standing 12 mm apart on a one-layer
   rounded sheet (5 mm beyond the boards, one first layer thick, in the frame color) that is part
   of the boards' first layer: it ties the boards together and gives the thin, tall boards a wide
   footprint; peel it off after printing. Supports and brim are switched off for the object (the
   project's supports would grow into the boards); these are ordinary per-object settings. Every
   part is named in the object list ("Board k: " + frame piece, marker, or the swatch's number and
   recipe; the sheet comes first).
   The paper around the board is the white reference. (See-through windows in the boards were tried
   for that and dropped: the paper seen through them read 25-45 % darker and tinted by the colored
   cell walls, and their tops printed as sagging bridges.)
   **Plate file.** Creating the plate asks where to save its *plate file* (JSON, readable as text): the
   settings the plate was made with (bundle, tools, layer heights, longest word, validation tiles,
   dual-wall level, core, board layout, which swatches it holds), the swatch list with board / row /
   column, and the measurements entered so far. Keep it with the photos.
3. Print it and measure. **Read a board (scan or photo)...** needs the plate file: it asks to open it (or, when none
   was saved, to save one from the current settings if they are still those of the plate). While a plate file is open the settings are locked (*Close plate
   file* unlocks them), and every reading, CSV load or typed value is written back into it, so a
   later session - e.g. after printing the missing swatches - opens it and continues. A project
   holding the plate remembers its plate file and opens it with the dialog. A plate file keeps its own
   swatch list, so a newer ColorMax still reads it as printed; one that doesn't match its filament
   set is refused rather than read against the wrong swatches.
   - **Flatbed scanner (recommended)**: cut a hole the size of a board into a sheet of plain white
     paper, lay the sheet on the glass and the board face-down in the hole; scan at 300 dpi, 24-bit,
     with every automatic correction off, as PNG or TIFF (TIFF is decoded by ColorMax itself:
     uncompressed, PackBits, LZW, Deflate or JPEG-compressed, 8 or 16 bits). A snug hole is best:
     where the paper doesn't reach the board the scan shows the lid, held 4 mm up by the board,
     as a gray band (the reading skips it). Leave Brightness, Contrast and Gamma at their defaults:
     Brightness subtracts from every pixel, which can't be undone (dark colors read too dark). If the
     reading says the paper is pure white and the scanner has an Exposure or Levels setting, lower
     the exposure a little or set the white point to about 240; without one, scan as is (about 1 dE).
   - **Phone/camera**: the board lies face-up on white paper with at least 2 cm of paper around it;
     two lamps, fixed white balance and exposure. Office paper with optical brighteners glows blue
     in daylight, which makes the colors read too yellow (in our tests Black and White came out ~8 b*
     units yellower than in a scan), so lamps or a scanner are better.
   - **Read a board (scan or photo)...** first shows these instructions (scanner first: it is the
     default and strongly recommended); the choice is remembered. Open the image,
     pick the board, click its four corners (the dotted corner first, then clockwise), check that
     the overlay sits on the cells, **Read colors**. Either face can be read: the cells go through
     the board, and the back face is recognized from the click order.
   - White reference: the paper 3 to 40 mm around the board. The reader fits a smooth illumination
     field (per channel, bilinear) to the brightest consistent, neutral paper points and drops the
     rest: the board's own shadow on the paper, a lid seen through a gap, the table beyond the
     sheet, colored things. It corrects exposure, white balance and uneven light.
   - Each cell is averaged in linear light over its full height (many layer periods) per column,
     and the median is taken across columns. A sharp image resolves the individual layers, and a
     per-pixel median would pick one filament instead of the blend.
   - Set the paper reflectance if you use a reference card (default 0.88). Camera color processing
     (tone mapping, saturation) is not undone by a white reference.
   - **Colorimeter**: center of each cell, `L a b` (D65/2°); the aperture must be smaller than 12 x 6 mm.
4. **Save calibration** first checks the readings: a filament whose swatches all read far from their
   uncalibrated predictions while the others don't (median > 15 dE and more than twice the others)
   most likely didn't print - an empty spool, a clogged nozzle or the wrong tool; the other layers
   then fill the gaps and the swatch shows the mix without it - or isn't the bundle's filament. It
   offers to leave those swatches out and clears their values. **only the swatches still
   missing** next to *Create calibration plate* then prints a smaller plate with exactly the
   swatches that have no value (also handy for single misprinted swatches); read its boards when
   printed (they map back onto the full list) and save again. The smaller plate gets its own plate
   file, which carries the measurements you already have.
5. Every plan has an id (e.g. `3F2A-240`, shown in the dialog and the plate file); saving
   measurements against a different plan than the plate on the plater asks first. The validation
   swatches are chosen with a fixed, portable shuffle, so the same settings give the same plan on
   every build. Photo cells that can't be read (outside the photo, not uniform) are left empty and
   listed, never saved as a color. Saving under the name of an existing calibration *updates* it
   (after asking): the new measurements replace or add to it and its other measurements are kept, so a
   plate with only the missing swatches completes it (not merged when the layer heights differ: replace
   or rename). The name defaults to the plate file's calibration, else to the newest calibration with
   the same layer heights. A calibration remembers its plate file, and the dialog opens the plate file
   of the selected filament set's newest calibration. A user
   bundle is saved back into its own file (written safely).
   Measurements use a '.' decimal point; "62,1 -21,4 -30,2" (decimal commas between spaces) is
   also understood, anything ambiguous is refused rather than misread.
   A saved calibration records the layer heights the plate was printed with (taken from the
   plate; asked when unknown), and whether it had a white core and which wall settings; the
   conversion summary says when the current settings differ.
   Values can also be typed, pasted or imported from a CSV (**Import values (CSV)...**). Lines starting with the swatch number
   ("12;62.1;-21.4;-30.2" or "S12 ...") go to that swatch, so a file may hold part of the list;
   a file without numbers must have exactly one line per swatch (one line per swatch in plate
   order: `L,a,b` or `#RRGGBB`, optional leading number).

**One bundle, several calibrations.** A bundle is one set of filaments. Its calibrations are kept
side by side in the user copy of the bundle (`<PrusaSlicer data dir>/colormix/bundles/<bundle id>.json`),
each recording its layer heights, white core and wall settings: print and save one calibration per
layer schedule you use (e.g. "0.10 mm" and "0.12/0.10/0.08"); the conversion dialog picks the
calibration and warns when its layer heights differ from the current ones.

**The model.** Measured swatches are reproduced exactly. Other recipes are predicted in two steps:

- A *base mixing model* fitted to the calibration. Layers don't contribute in proportion to their
  count: in our measurements Magenta and Black overpower White and Yellow (Magenta 25 % +
  White 75 % measured a* = 40 where the layer shares predict 21). The fit chooses between Prusa's
  mixer (prusa-fdm-mixer) and a *subtractive* mix - a weighted geometric mean of the filament
  colors in linear light, as for light filtered through translucent layers - and fits a *strength*
  per filament (its weight in a mix relative to the others) and the dual-wall outer share. With
  fewer than 12 mixed swatches Prusa's mixer is used unchanged.
- A Gaussian-process correction fitted to the remaining residuals (features: outer and inner wall
  weights; leave-one-out tuned).

The dialog reports the uncalibrated mixer's error, the fitted base model (strengths, outer share),
the corrected leave-one-out error and the held-out (validation) error.

Reference calibration (Prusament PLA ColorMix, CORE One INDX, 0.1 mm, matte white core, flatbed
scan, 240 swatches): uncalibrated mixer median 6.5 dE2000; fitted subtractive base 4.7; with the
correction 2.1 leave-one-out (P90 4.4) and 1.6 on the 45 held-out validation swatches (P90 3.9).

On Prusa's published measurements (prusa-fdm-mixer data set) the correction alone reduced the median
CIEDE2000 error from 7.0 to 4.5 with a rich calibration plate.

## Adding your own filaments (filament sets / bundles)

**New...** next to *Filament set* (conversion dialog) creates a filament set; **Edit...** changes the
selected one. The editor lists the filaments - name, color (double-click for a color picker, or
type #RRGGBB: the color of the spool as it looks printed), the tool it is usually loaded in,
*Mixing* (see below) - and *Use as*: mark your White (the white core) and a dark filament (the frame of the calibration
boards). A new set starts with the spools loaded in the printer (preset names, colors, tools);
*Use the printer's spools* refills the list. The striping rules are under *Advanced*. Saving
checks the set (a name, at least two filaments set to *Mix*, unique names, valid colors) and stores it
as a user bundle; a new set is uncalibrated - calibrate it for accurate colors. Editing keeps the
set's calibrations (it warns when a removed filament was measured); filament ids are kept, new
filaments get ids made from their names (`white` / `black` for the marked ones).

**Mixing** - how each filament takes part:

- **Mix**: blended with the others in layer recipes (and printed on its own).
- **Accent**: e.g. Prusa's ColorMix Gold / Silver Glitter - clear PLA Natural with glitter, made to be
  layered with the other colors for sparkle ("a 1:1 mix of any PLA with one of the glitters", extra
  greys with Black and White; Prusa's ColorMix blog). Conversions don't use accents yet (a way to ask
  for sparkle is planned); they are calibrated with a targeted set of swatches (below).
- **Solid only**: printed on its own, never in a mix (silk, marble, multicolor). The filament lookup
  suggests it for glitter / silk / multicolor filaments it finds.

For compatibility the keys `mixable` / `effect` are also read (when `role` is missing) and written.

**Look up filament...** in the editor searches public filament databases and fills the selected row
(name, color and TD; glitter, silk and multicolor filaments are set to Solid only). Search by brand, color name and material; every word must start a
word of the entry ("red" finds Red). Sources, downloaded once (about 8 MB) into
`<data dir>/colormix/filament_db.json` and updated with *Download / update database*:

- [filamentcolors.xyz](https://filamentcolors.xyz) - printed swatches measured with a colorimeter (CIE Lab), many with
  a TD; data CC BY 4.0. Shown first ("measured").
- [SpoolmanDB](https://github.com/Donkie/SpoolmanDB) - a large catalog with the makers' colors (not measured), a
  translucent flag and finish / pattern; MIT. A catalog entry of a filament that was also measured is dropped.

In November 2026 the databases held about 2,300 measured filaments (about 730 with a TD) and 6,300
catalog ones. Neither
has Prusament PLA ColorMix. A filament's **TD** (transmission distance, mm, as used by HueForge: how far
light travels into it) is kept in the filament set (`td`) and sets the strength of filaments nobody has
calibrated - translucent filaments show less in a layer mix:

- **Uncalibrated set:** strength = (3 mm / TD)^0.25 (1 when the TD is unknown), with Prusa's mixer. On
  Prusa's measured mixes (prusa-fdm-mixer fitting / holdout sets, 134 mixes whose Prusament filaments all
  have a HueForge TD) this lowers the mean error from 8.1 to 7.3 dE00 (fitting set) and 7.8 to 7.3
  (holdout; median 6.1 to 5.3). Exponents 0.15-0.35 work about as well; 0.5 and up are worse. The
  subtractive model doesn't beat Prusa's mixer without a calibration, with or without TD.
- **Calibrated set, filament not on the swatches** (e.g. a spool that didn't print): it borrows the fitted
  strength of the calibrated filaments of similar TD (interpolated in log TD, the nearest one beyond the
  ends); without TDs it falls back to the rule above. The fit report lists these ("estimated from TD").
- A calibration still fits every filament on the swatches; TD doesn't change those strengths.

TD alone doesn't explain the fitted strengths: in the reference calibration Black (TD around 0.1-0.3
for black PLA) fitted 1.50, about the same as Magenta (1.55). Opaque filaments level off, so TD is most useful
among the translucent mixing colors. Prusament doesn't publish TDs for ColorMix, so the ColorMix set has none: its filaments are all
calibrated from swatches (Polymaker's similar Panchroma Translucent PLA CMY set: Cyan 8.0, Magenta 7.8,
Yellow 14 - Polymaker wiki, "HEX codes and transmission distances").

### Color range chart

**Color range...** (next to the filament set) shows what the set can print: a grid of 24 hues (columns,
red first) by 9 lightness steps (rows, Oklab L 0.95 to 0.30) plus a column of grays. The top half of
each square is a screen color, the bottom half the closest color the spools can print (every recipe the
converter would consider, closest by CIEDE2000). Squares within 3 dE are framed; the others show the
distance. Under the chart, the model's colors: one pair each (as designed over the closest printable
color), farthest first, framed red when no recipe comes within 3 dE; hover for the recipe. (The closest
printable color; Preview may choose a slightly different mix, e.g. to keep two colors apart.) Options: the spools loaded or all spools of the set, a calibration or none,
dual walls, and vivid / medium / soft screen colors (100 / 60 / 35 % of the most saturated sRGB chroma
at that hue and lightness). **Save as PNG...** saves the chart with a caption of the settings.
(`Gamut.hpp`: `make_gamut_chart`.)

### Original / ColorMax view

On a converted model the 3D editor shows an **Original | ColorMax** switch next to the 3D editor /
Preview buttons (bottom left). **ColorMax** (the default) draws each part as it will print: every color
replaced by its mix, drawn in the mix's predicted color (pure spools in the spool's color). **Original**
draws the painting and colors the model had before the conversion (the source painting ColorMax keeps
for Revert). Only the view changes: nothing in the model, the project or the G-code. The switch is hidden
in Preview, on calibration plates and while the multi-material painting tool is open (it always shows
and edits the ColorMax painting). (`GLCanvas3D::_render_colormax_view_toggle`, `GLVolumeCollection::render`.)

Bundles are JSON files. System bundles live in `resources/colormix/bundles`, user bundles in
`<data dir>/colormix/bundles` (the **Filament sets folder** button opens it and drops a template there).
A user bundle with the same `id` replaces the system one.

```json
{
  "format": "prusaslicer-colormix-bundle",
  "version": 1,
  "id": "my-cmyk-pla",
  "name": "My CMYK PLA",
  "material": "PLA",
  "filaments": [
    { "id": "cyan",  "name": "Cyan",  "color": "#00ACD6", "default_tool": 1, "role": "mix" },
    { "id": "white", "name": "White", "color": "#E9E7E4", "default_tool": 4, "role": "mix" },
    { "id": "gold",  "name": "Gold glitter", "color": "#DDCAA9", "default_tool": 6, "role": "accent" },
    { "id": "silk",  "name": "Silk copper", "color": "#B87333", "default_tool": 7, "role": "solid" }
  ],
  "rules": { "max_run": 3, "recurrence_window": 4, "max_word_length": 6, "max_filaments": 3 },
  "calibrations": [
    {
      "id": "core-one-0.1mm", "date": "2026-11-09", "printer": "CORE One",
      "first_layer_height": 0.2, "layer_schedule": [0.1],
      "swatches": [
        { "word": ["cyan"], "measured": [62.1, -27.4, -31.0] },
        { "word": ["cyan", "cyan", "white"], "measured": [70.3, -19.2, -21.5] },
        { "ratios": { "cyan": 0.5, "white": 0.5 }, "measured": "#7FC4DF", "held_out": true },
        { "word": ["cyan"], "inner_word": ["black"], "measured": [41.0, -18.5, -24.0] }
      ]
    }
  ]
}
```

- `role`: `mix` (default), `accent` or `solid` (see *Mixing* above).
- `rules`: `max_run` = most consecutive layers of one filament (circular), `recurrence_window` =
  every filament of a word appears in every window of this many layers, `max_word_length` =
  longest repeat, `max_filaments` = filaments per recipe. The shipped ColorMix bundle uses
  max run 3 (allows Prusa's 3:1 and 2:1:1 presets). Use `2 / 3` for less striping.
- `word` lists filament ids bottom to top; layer L of the object uses `word[L % length]`.
  `inner_word` (optional) is the inner wall's word of a dual-wall swatch.
- `measured` is `[L, a, b]`, `{"L":..,"a":..,"b":..}` or `"#RRGGBB"`.

## Known issues

Known limitations (each needs an unusual sequence of steps):

- Changing the printer's tool count renumbers blends, but not the source slots that are
  implicitly a blend id, nor the blends and extruders recorded for Revert: a Revert after such a
  change can paint a blend's areas with a physical tool.
- After a colored import onto a non-empty plater, tools or blends added later can take the
  numbers of the file's slots and convert as those colors (the empty-plater case is described
  above).
- Parts imported onto a non-empty plater carry a source painting right away, so they count as
  ColorMax objects (e.g. next to a calibration plate its techniques apply to them, and uniformly
  painted multi-part objects use per-part segmentation).
- Virtual-extruder blends are part of undo/redo; Import Config replaces them without an undo
  snapshot, so undoing an earlier action can bring the old blends back.
- The recorded tool count isn't updated when a converted project has no blends.
- An import with more than 255 colors in total (which isn't merged) still records its objects'
  file slots, which opening the ColorMax dialog then treats as palette slots.
- Importing only the geometry of a project painted with virtual-extruder blends drops the blends, as in stock PrusaSlicer; their areas then show in
  tool 1's color, and the ColorMax dialog lists each of those painting states as a separate gray
  (#808080) source color. Open such a project as a project instead.

## Code map

| Area | Files |
|---|---|
| Color math, mixer port | `src/libslic3r/Feature/ColorMix/ColorSpace.*`, `Predictor.*` |
| Bundles | `Bundle.*`, `resources/colormix/bundles/` |
| Layer words, rules, schedules | `Recipes.*` |
| Optimizer | `Converter.*` |
| Model integration | `ModelConvert.*`, `ModelData.*`, `Model.hpp` (`colormix`, `colormix_source_facets`) |
| Calibration | `Calibration.*`, `PhotoCalibration.*` (board layout, scan / photo reading, TIFF decoding) |
| Filament databases | `FilamentDb.*` (filamentcolors.xyz, SpoolmanDB) |
| Color range chart | `Gamut.*` |
| Slicing | `SliceTechniques.*`, `PrintObjectSlice.cpp` (apply_mm_segmentation), `PrintApply.cpp`, `Slicing.cpp` (layer schedule), `VirtualExtruder.*` (explicit `sequence`) |
| 3MF | `Format/3mf.cpp` (source facets, project data, basematerials, Bambu / Orca painting and filaments) |
| GUI | `src/slic3r/GUI/ColorMixDialog.*`, `Sidebar.cpp`, `Plater.cpp`; Original / ColorMax view: `GLCanvas3D.cpp`, `3DScene.cpp` |
| Tests | `tests/libslic3r/test_colormix.cpp`, `tests/fff_print/test_colormix_slice.cpp` |

## Building on Windows

```
REM once: dependencies into any folder, here C:\src\PrusaSlicer-deps (CMake 4 needs the policy
REM override for old third-party projects)
set CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake -S deps -B deps\build -G "Visual Studio 17 2022" -A x64 -DDEP_DEBUG=OFF -DDESTDIR=C:\src\PrusaSlicer-deps
cmake --build deps\build --config Release -- /m:6

REM the application
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\src\PrusaSlicer-deps\usr\local
cmake --build build --config Release --target PrusaSlicer_app_gui libslic3r_tests fff_print_tests -- /m
```
