///|/ ColorMax engine - per-project data kept in Model and saved in the 3MF.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <map>
#include <string>
#include <vector>

namespace Slic3r::ColorMix {

// Slicing techniques applied to virtual-extruder (layer mixed) regions.
struct SliceSettings
{
    // White core: everything deeper than `wall_depth` from the outline of a painted
    // layer is printed with `core_extruder` (1-based, 0 = off).
    unsigned int core_extruder{0};
    double       wall_depth{0.9};
    // Top-surface patches: the top `top_layers` layers of a mixed region are tiled with
    // `top_cell_size` mm square cells in an 8x8 ordered (Bayer) pattern of its filaments.
    bool   top_pattern{false};
    double top_cell_size{1.5};
    int    top_layers{3};
    // Variable layer thickness: repeating layer heights above the first layer (empty = off).
    std::vector<double> layer_schedule;
    // Dual walls: width of the outer ring that follows the outer word of a dual-wall virtual
    // extruder (about one external perimeter); the rest of the region follows the inner word.
    double dual_wall_width{0.45};
    // Top patches on slopes: a surface gets patches where its layer bands are wider than this (mm;
    // a layer of height h on a slope at angle a from horizontal shows a band h / sin(a) wide).
    // 0 = only flat tops: areas more than 0.45 mm inside the outline within `top_layers` of a top.
    double patch_band_width{0.};
    // Region cleanup on the sliced color regions (before the techniques above):
    //  - smooth_borders: a point keeps a color only if most of the 5 layers around it have it there
    //    (removes the jagged, layer-to-layer flicker of borders painted along triangle edges),
    //  - runs along a layer's outline shorter than `min_run` mm go to the neighbouring color,
    //  - islands with less visible surface than `min_island_area` mm^2 go to their neighbours.
    // Calibration plates / slope tests: every part is one full-color cell, segmented as a whole
    // (overrides "max segmented region width").
    bool   whole_part_regions{false};
    bool   region_cleanup{false};
    bool   smooth_borders{true};
    double min_run{0.4};
    double min_island_area{1.0};

    bool any() const { return core_extruder > 0 || top_pattern || !layer_schedule.empty() || region_cleanup || whole_part_regions; }
    bool operator==(const SliceSettings &o) const
    {
        return core_extruder == o.core_extruder && wall_depth == o.wall_depth && top_pattern == o.top_pattern
            && top_cell_size == o.top_cell_size && top_layers == o.top_layers && layer_schedule == o.layer_schedule
            && dual_wall_width == o.dual_wall_width && patch_band_width == o.patch_band_width
            && region_cleanup == o.region_cleanup && whole_part_regions == o.whole_part_regions
            && smooth_borders == o.smooth_borders && min_run == o.min_run
            && min_island_area == o.min_island_area;
    }
    bool operator!=(const SliceSettings &o) const { return !(*this == o); }
};

struct ModelData
{
    SliceSettings slice;
    // Colors the model was designed with, indexed by painting state (1-based extruder slot of the
    // source file; index 0 unused). Filled on import of a colored 3MF or on the first conversion.
    std::vector<std::string> source_colors;
    // Settings of the last conversion, so it can be repeated or tweaked.
    std::string               bundle_id;
    std::string               calibration_id;
    std::vector<unsigned int> tool_of_filament;  // per bundle filament, 1-based tool (0 = not loaded)
    std::string               options_json;      // converter options as saved by the GUI (a JSON object)
    // Parameters of the calibration plate / slope test on the plater (a JSON object written by the
    // calibration dialog), so reopening the dialog rebuilds the same swatch list.
    std::string               plate_json;
    // Per palette slot: the tool its color came from, when that isn't the slot number itself
    // (slots added for parts added after a conversion or for kept touch-ups). Revert paints with it.
    std::vector<unsigned int> slot_tools;
    // slot_tools value of a slot that prints with its part's own extruder (an imported file's slot
    // beyond the printer's tools).
    static constexpr unsigned int OWN_EXTRUDER = 0xFFFF;
    // ColorMix data of a 3MF this build couldn't read (e.g. from a newer version): kept as it was
    // and written back unchanged on save, so it isn't lost. Not part of the comparisons.
    std::string               unreadable_json;
    // File form of pending_import_slots (only while a 3MF is written or read): the color slot
    // (1-based, 0 = none) of each object in model object order, and of each part of each object
    // (only when parts have their own, e.g. after loading as a multi-part object).
    std::vector<unsigned int>              import_slots;
    std::vector<std::vector<unsigned int>> import_part_slots;
    // Color slots of imported objects or parts (by ModelObject / ModelVolume id; ids are unique
    // across both) not applied yet. Opening the ColorMix dialog applies them
    // (apply_pending_import_slots()); until then the objects print as a stock import would.
    // Nothing uses them for slicing. Part of undo/redo; saved with the project.
    std::map<size_t, unsigned int> pending_import_slots;
    bool has_pending_import_slots() const
    {
        for (const auto &[id, slot] : pending_import_slots)
            if (slot > 0)
                return true;
        return false;
    }

    bool empty() const
    {
        return !slice.any() && source_colors.empty() && bundle_id.empty() && calibration_id.empty() && tool_of_filament.empty() &&
               options_json.empty() && plate_json.empty() && slot_tools.empty() && pending_import_slots.empty() && import_slots.empty() &&
               import_part_slots.empty();
    }
    bool operator==(const ModelData &o) const
    {
        return slice == o.slice && source_colors == o.source_colors && bundle_id == o.bundle_id
            && calibration_id == o.calibration_id && tool_of_filament == o.tool_of_filament && options_json == o.options_json
            && plate_json == o.plate_json && slot_tools == o.slot_tools;
    }
    bool operator!=(const ModelData &o) const { return !(*this == o); }
};

std::string model_data_to_json(const ModelData &d);
// Returns false (leaving `out` untouched) on malformed input.
bool model_data_from_json(const std::string &text, ModelData &out, std::string *error = nullptr);

// File name inside the 3MF archive.
inline constexpr const char *MODEL_DATA_3MF_FILE = "Metadata/Prusa_Slicer_colormix.json";

} // namespace Slic3r::ColorMix
