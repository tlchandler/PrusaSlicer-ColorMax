///|/ ColorMax engine - calibration swatch plates and measurement import.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <string>
#include <vector>

#include "PhotoCalibration.hpp"
#include "libslic3r/Point.hpp"
#include "Recipes.hpp"

namespace Slic3r {
class Model;
}

namespace Slic3r::ColorMix {

struct SwatchSpec
{
    Word        word;          // bundle filament indices
    Word        inner_word;    // dual walls: inner wall word (empty = single wall)
    bool        held_out{false};
    std::string label;         // e.g. "S07  Cyan 67% + White 33%  [Cy Cy Wh]"
};

// Swatches for one calibration plate: every pure mixable filament, every allowed pair and triple
// word up to `max_len` layers (one word per distinct ratio), plus `held_out` longer words kept
// aside to measure real accuracy.
// Dual-wall swatches (`dual_level`): 0 = none; 1 = basic: every (outer, inner) pair of pure
// filaments + 10 mixed pairs; 2 = full: pure pairs, 1:1 outer blends over each other pure inner,
// pure outers over 1:1 inner blends, 1:1:1 outers over each other pure inner, plus 10 held-out
// validation pairs.
// What plan_calibration_swatches() had to leave out to stay within PrusaSlicer's painting colors.
struct PlanReport
{
    int budget{0};             // blended swatches a plate can have
    int needed{0};             // blended swatches planned before dropping
    int dropped_validation{0};
    int dropped_dual{0};
    int dropped_core{0};       // regular single-wall blends: the set can't be calibrated properly
};
// Solid-only and accent filaments get one solid swatch each; accents (glitter) also a targeted set:
// 1:1 with every filament that mixes, 3:1 and 1:3 with White and Black, 1:1 with each other accent.
std::vector<SwatchSpec> plan_calibration_swatches(const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament,
                                                  const LayerSchedule &schedule, int max_len = 4, int held_out = 10,
                                                  int dual_level = 0, int fill_to_multiple_of = 0, int max_blended = -1,
                                                  PlanReport *report = nullptr);
// `max_blended`: painting states available for blended swatches (each needs its own virtual
// extruder); -1 = 255 minus 16 physical tools. Validation swatches are dropped first to fit.
// `fill_to_multiple_of` > 0 (swatches per board): append extra validation swatches (single-wall and,
// with dual walls, dual-wall ones, alternating) until the last board is full instead of leaving
// empty cells.

// Adds calibration boards to `model` (cleared first): each board is a frame holding a grid of
// swatch cells (`layout`), with see-through windows in the middle row, standing upright on the bed.
// Board k carries k marker dots on its top edge at the left end. Swatches fill the boards in
// reading order. All boards are one object standing on a one-layer rounded sheet (height
// `first_layer_height`) that ties them together on the bed and peels off after printing.
// Replaces model.virtual_extruders. `frame_tool` prints the frame and the sheet (0 = first loaded
// tool). Returns a text legend.
// Short fingerprint of a swatch plan ("3F2A-225"): the same settings give the same id; shown with
// the plate so measurements are entered against the plan that was printed.
std::string calibration_plan_id(const std::vector<SwatchSpec> &swatches);

std::string build_calibration_plate(Model &model, const std::vector<SwatchSpec> &swatches, const Bundle &bundle,
                                    const std::vector<unsigned int> &tool_of_filament, unsigned int num_physical_extruders,
                                    const BoardLayout &layout = BoardLayout(), unsigned int frame_tool = 0, double bed_depth = 210.,
                                    double first_layer_height = 0.2);

// Moves every object of `model` so that together they sit in the middle of the bed (prints in the
// middle of the bed usually come out best). `bed_center` in bed coordinates (mm).
void center_on_bed(Model &model, const Vec2d &bed_center);

// Board and cell of swatch `index`: {board (0-based), row, col}.
std::array<int, 3> swatch_position(int index, const BoardLayout &layout);
int                board_count(int num_swatches, const BoardLayout &layout);

// Calibration from measured colors (one per swatch, same order). Swatches whose measurement is
// missing (`has_value[i] == false`) are skipped.
// Slope test for choosing SliceSettings::patch_band_width: two rows of wedges (40 down to 5 degrees,
// steepest left) printed with layer mixing only (no top patches), one row in the bundle's
// highest-contrast 1:1 pair, one in an equal mix of its three most colorful filaments. Replaces
// the model's objects and virtual extruders; returns instructions with the band width of each
// wedge on `schedule`.
std::string build_slope_test(Model &model, const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament,
                             unsigned int num_physical, unsigned int frame_tool, const LayerSchedule &schedule, double bed_depth);
inline const std::vector<double> &slope_test_angles()
{
    static const std::vector<double> a = {40., 35., 30., 25., 20., 15., 10., 5.};
    return a;
}

// A measurement "L a b" or "#RRGGBB". The three numbers are separated by spaces, tabs or ';' (a comma
// inside a number is then a decimal comma: "62,1 -21,4 -30,2"), or by commas alone with '.' decimals
// ("62.1,-21.4,-30.2"). Anything ambiguous is rejected rather than misread.
bool parse_lab_measurement(const std::string &text, Lab &out);

// Measurements from a CSV / text file for a list of `rows` swatches. A line is a measurement, or a
// swatch number (1-based, or "S12") followed by one; numbered lines go to their swatch. A file
// without numbers must have exactly one line per swatch and no unreadable line between them
// (values would shift onto the wrong swatches). A leading header line is allowed.
struct CsvMeasurements
{
    std::vector<std::string> values;      // per row, "" = no value; "L a b" with '.' decimals or "#RRGGBB"
    int                      loaded{0};
    std::vector<int>         bad_lines;   // 1-based line numbers that couldn't be read
    std::string              error;       // non-empty: nothing should be applied
};
CsvMeasurements parse_measurement_csv(const std::string &text, size_t rows);

// A filament whose swatches read far from their expected colors while the others don't: it most
// likely didn't print (empty spool, clogged nozzle, wrong tool: the other layers then sag into the
// gaps and the swatch shows the mix without it), or the spool isn't the bundle's filament. Seen in
// practice: with a Cyan tool that printed nothing, every Cyan swatch was 24.5 dE from its
// uncalibrated prediction (median), the others 8.7. `swatches` indices (plan order) of the swatches that contain the filament
// (outer or inner wall) and were measured.
struct FilamentCheck
{
    int              filament{-1};       // bundle filament index
    double           median_with{0.};    // dE2000 to the uncalibrated prediction, swatches with it
    double           median_without{0.}; // the same for the other measured swatches
    std::vector<int> swatches;
};
std::vector<FilamentCheck> check_filaments(const Bundle &bundle, const std::vector<SwatchSpec> &swatches, const std::vector<Lab> &measured,
                                           const std::vector<bool> &has_value, const LayerSchedule &schedule);

Calibration make_calibration(const Bundle &bundle, const std::vector<SwatchSpec> &swatches,
                             const std::vector<Lab> &measured, const std::vector<bool> &has_value,
                             const LayerSchedule &schedule, const std::string &id, const std::string &printer);

// Saving a calibration under the name of an existing one updates it: the swatches of `old` that
// `updated` doesn't measure (same outer word, inner word and ratios) are kept, so a plate with only
// the missing swatches completes a calibration. Only when both were printed with the same layer
// heights (otherwise they describe different prints): returns false and changes nothing then.
// `kept`: number of swatches taken over from `old`.
bool merge_calibration(const Calibration &old, Calibration &updated, int *kept = nullptr);

// Bundle file name for a user copy of `bundle` (in the user bundles folder).
std::string user_bundle_file_name(const Bundle &bundle);

} // namespace Slic3r::ColorMix
