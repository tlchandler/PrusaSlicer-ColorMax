///|/ ColorMax engine - per-project data kept in Model and saved in the 3MF.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "ModelData.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace Slic3r::ColorMix {

std::string model_data_to_json(const ModelData &d)
{
    nlohmann::json j;
    j["version"] = 1;
    j["slice"]   = {{"core_extruder", d.slice.core_extruder},
                    {"wall_depth", d.slice.wall_depth},
                    {"top_pattern", d.slice.top_pattern},
                    {"top_cell_size", d.slice.top_cell_size},
                    {"top_layers", d.slice.top_layers},
                    {"layer_schedule", d.slice.layer_schedule},
                    {"dual_wall_width", d.slice.dual_wall_width},
                    {"patch_band_width", d.slice.patch_band_width},
                    {"region_cleanup", d.slice.region_cleanup},
                    {"whole_part_regions", d.slice.whole_part_regions},
                    {"smooth_borders", d.slice.smooth_borders},
                    {"min_run", d.slice.min_run},
                    {"min_island_area", d.slice.min_island_area}};
    j["source_colors"]    = d.source_colors;
    j["bundle_id"]        = d.bundle_id;
    j["calibration_id"]   = d.calibration_id;
    j["tool_of_filament"] = d.tool_of_filament;
    if (!d.slot_tools.empty())
        j["slot_tools"] = d.slot_tools;
    if (!d.import_slots.empty())
        j["import_slots"] = d.import_slots;
    if (!d.import_part_slots.empty())
        j["import_part_slots"] = d.import_part_slots;
    // Invalid embedded JSON is skipped: dumping a discarded value would make the whole file unreadable.
    for (const auto &[key, text] : {std::make_pair("options", &d.options_json), std::make_pair("plate", &d.plate_json)})
        if (!text->empty())
            if (nlohmann::json v = nlohmann::json::parse(*text, nullptr, false); !v.is_discarded())
                j[key] = std::move(v);
    return j.dump(2);
}

bool model_data_from_json(const std::string &text, ModelData &out, std::string *error)
{
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (j.value("version", 0) > 1)
            throw std::runtime_error("ColorMax project data is from a newer version");
        ModelData d;
        if (j.contains("slice")) {
            const auto &s          = j["slice"];
            d.slice.core_extruder  = s.value("core_extruder", 0u);
            d.slice.wall_depth     = std::clamp(s.value("wall_depth", 0.9), 0.2, 10.);
            d.slice.top_pattern    = s.value("top_pattern", false);
            d.slice.top_cell_size  = std::clamp(s.value("top_cell_size", 1.5), 0.5, 10.);
            d.slice.top_layers     = std::clamp(s.value("top_layers", 3), 1, 20);
            d.slice.dual_wall_width = std::clamp(s.value("dual_wall_width", 0.45), 0.2, 2.);
            d.slice.patch_band_width = std::clamp(s.value("patch_band_width", 0.), 0., 5.);
            d.slice.region_cleanup   = s.value("region_cleanup", false);
            d.slice.whole_part_regions = s.value("whole_part_regions", false);
            d.slice.smooth_borders   = s.value("smooth_borders", true);
            d.slice.min_run          = std::clamp(s.value("min_run", 0.4), 0., 5.);
            d.slice.min_island_area  = std::clamp(s.value("min_island_area", 1.0), 0., 100.);
            if (s.contains("layer_schedule"))
                d.slice.layer_schedule = s["layer_schedule"].get<std::vector<double>>();
            for (double h : d.slice.layer_schedule)
                if (!(h >= 0.02 && h <= 1.))
                    throw std::runtime_error("invalid layer height in ColorMax layer schedule");
            if (d.slice.layer_schedule.size() > 16)
                throw std::runtime_error("ColorMax layer schedule has more than 16 heights");
        }
        if (j.contains("source_colors"))
            d.source_colors = j["source_colors"].get<std::vector<std::string>>();
        d.bundle_id      = j.value("bundle_id", std::string());
        d.calibration_id = j.value("calibration_id", std::string());
        if (j.contains("tool_of_filament"))
            d.tool_of_filament = j["tool_of_filament"].get<std::vector<unsigned int>>();
        if (j.contains("slot_tools"))
            d.slot_tools = j["slot_tools"].get<std::vector<unsigned int>>();
        if (j.contains("import_slots"))
            d.import_slots = j["import_slots"].get<std::vector<unsigned int>>();
        if (j.contains("import_part_slots"))
            d.import_part_slots = j["import_part_slots"].get<std::vector<std::vector<unsigned int>>>();
        if (j.contains("options") && j["options"].is_object())
            d.options_json = j["options"].dump();
        if (j.contains("plate") && j["plate"].is_object())
            d.plate_json = j["plate"].dump();
        out = std::move(d);
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

} // namespace Slic3r::ColorMix
