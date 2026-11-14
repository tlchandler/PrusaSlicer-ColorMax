///|/ ColorMax engine - apply a color conversion to a Model (painting + virtual extruders).
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <map>
#include <string>
#include <vector>

#include "Converter.hpp"
#include "libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "ModelData.hpp"

namespace Slic3r {
class Model;
class ModelVolume;
class ModelObject;
}

namespace Slic3r::ColorMix {

// One color the model was designed with.
struct SourceColor
{
    unsigned int state{0};   // painting state (1-based source slot)
    std::string  color;      // "#RRGGBB"
    double       area{0.};   // visible (not bottom-facing) area, mm^2
    std::string  label;      // e.g. names of the parts using it
    // Heights above the bed where it is painted (disjoint [z0, z1] ranges, mm).
    std::vector<std::pair<double, double>> z_ranges;
    // Heights of its up-facing triangles (flatter than about 37 degrees).
    std::vector<std::pair<double, double>> top_z_ranges;
};

// Current extruder colors used to describe a not-yet-converted model:
// index 0 unused, then physical extruders 1..N, then virtual extruder ids.
std::vector<std::string> current_extruder_colors(const Model &model, const std::vector<std::string> &physical_colors);

// True when the model has paint or extruder assignments to convert.
bool has_convertible_colors(const Model &model);

// Saves the current painting (with unpainted triangles set to their volume's extruder) as the
// source painting, unless a source painting exists already. Fills model.colormix.source_colors
// from `extruder_colors` (see current_extruder_colors) for states that have no color yet.
void capture_source(Model &model, const std::vector<std::string> &extruder_colors);

// Like capture_source(), but the painting states (extruder ids) of volumes without a source painting
// always map to palette slots by their current colors - for parts already on the plater when a
// colored model brings its own palette.
void capture_source_by_color(Model &model, const std::vector<std::string> &extruder_colors);

// Source colors with their visible areas, from the source painting (or, before the first
// conversion, from the current painting). States without a stored source color take their color
// from `fallback_colors` (see current_extruder_colors).
std::vector<SourceColor> collect_source_colors(const Model &model, const std::vector<std::string> &fallback_colors);

struct ApplySummary
{
    int virtual_extruders{0};
    int solid_colors{0};
    // Source colors left unmapped because no painting state was left for their blend (they keep
    // printing with their volume's extruder). The caller should warn when > 0.
    int unmapped_colors{0};
    // Source colors nothing loaded can print (they print with their part's extruder, or tool 1 when
    // that was a virtual extruder replaced by this conversion).
    int unprintable_colors{0};
    std::vector<unsigned int> state_to_extruder; // index = source state, 0 = unmapped
};

// Rewrites the painting of every volume from the source painting, mapping each source color to
// the chosen tool (pure filament) or to a new virtual extruder (blend), and replaces
// model.virtual_extruders. `sources[k]` corresponds to `result.choice[k]`.
ApplySummary apply_conversion(Model &model, const std::vector<SourceColor> &sources, const ConversionResult &result,
                              const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament,
                              unsigned int num_physical_extruders);

// Painting a conversion produces for a volume with a source painting: source slot s becomes
// state_to_extruder[s]; unmapped slots fall back to the volume's extruder (tool 1 when that is a
// virtual extruder). apply_conversion() paints exactly this.
TriangleSelector converted_painting(const ModelVolume &volume, const std::vector<unsigned int> &state_to_extruder,
                                    unsigned int num_physical_extruders);

// How each source slot is painted now: the last conversion's record (options "applied_states")
// or, before any conversion / after a revert, the tool each slot came from (slot_tools, else slot k
// is tool k). Empty when there is nothing to compare with.
std::vector<unsigned int> expected_states(const Model &model, unsigned int num_physical_extruders);

// Volumes whose painting no longer is what `state_to_extruder` paints from their source (compared
// by what prints: unpainted and unprintable states count as the volume's extruder): touched up with
// the paint tool since. Their touch-ups are lost by the next conversion unless folded into the
// source painting first.
size_t count_repainted_volumes(const Model &model, const std::vector<unsigned int> &state_to_extruder, unsigned int num_physical_extruders);

// Makes the current painting of the repainted volumes their new source painting: each tool / blend
// state maps to the palette slot of its current color (a new slot when the color is new), and
// paints them from it again. Returns `state_to_extruder` extended by the new slots.
// `converted`: the record is a conversion's (a kept color may reuse any slot of that color);
// otherwise only a slot printing with the same tool.
std::vector<unsigned int> fold_repainted_into_source(Model &model, const std::vector<unsigned int> &state_to_extruder,
                                                     unsigned int num_physical_extruders, const std::vector<std::string> &extruder_colors,
                                                     bool converted = true);

// Importing a colored model (from another application) next to what is on the plater:
// - imported_color_count(): number of different source colors its parts use;
// - merge_imported_palette(): merges its palette (model.colormix.source_colors) into the plater's.
//   On an empty plater the imported palette is taken as is (slot k prints with tool k). Otherwise
//   the plater's tools and blends keep their ids as slots (nothing is recorded on the plater's
//   parts), the imported colors get slots after them (reusing one only when it is the same color
//   on the same tool, or any slot of that color in a converted project) and the imported parts
//   record that as their source painting; the slot's tool is recorded (slot_tools). In a converted
//   project the imported parts are painted as the conversion would; otherwise their painting and
//   extruders stay as the file assigns them (`plater_extruder_colors`: the plater's extruder
//   colors per tool). A converted project's parts (painted with tool / blend ids of their own
//   project) are repainted from their source colors. Returns false, changing nothing, when more
//   than 255 slots would be needed.
size_t imported_color_count(const Model &imported);
bool   merge_imported_palette(Model &plater_model, Model &imported, const std::vector<std::string> &plater_extruder_colors);

// An object the ColorMix techniques (white core, cleanup, patches, dual walls) apply to: converted
// (it has a source painting), part of a calibration plate / slope test, or painted with a ColorMix
// blend (a virtual extruder with a layer word). Ordinary painted objects slice as in stock.
bool is_colormix_object(const ModelObject &object, const SliceSettings &settings, const FullSpectrum::VirtualExtruders &virtual_extruders);

// Applies the color slots of imported objects that are still pending (ModelData::pending_import_slots):
// an object prints with its slot's tool when the printer has it; otherwise its slot is kept as its
// source painting (and it prints with its own extruder). Returns the number of objects beyond the
// printer's tools.
size_t apply_pending_import_slots(Model &model, unsigned int num_tools);

// Pending import slots are kept by object / part id in memory and by position in a 3MF:
// pending_slots_to_file() fills `out.import_slots` / `import_part_slots` from `model`'s pending
// slots; pending_slots_from_file() turns those of `model.colormix` into pending slots (a list that
// doesn't match the objects is dropped) and clears them.
void pending_slots_to_file(const Model &model, ModelData &out);
void pending_slots_from_file(Model &model);
// Objects `from.objects[i]` were copied to `to.objects[to_idxs[i]]` (with their parts in the same
// order): their pending slots go along (and are removed from `from`).
void copy_pending_slots(Model &from, Model &to, const std::vector<size_t> &to_idxs);
// Around ModelProcessing::convert_to_multipart_object(): the slot of each part of the merged
// object (in its order), and setting those as the parts' pending slots.
std::vector<unsigned int> multipart_pending_slots(const Model &model);
void set_multipart_pending_slots(Model &model, const std::vector<unsigned int> &part_slots);

// Virtual extruder ids were renumbered (old id -> new id, e.g. another tool count): keeps the record
// of the last conversion (its painted states, ModelData::options_json) in step.
void remap_conversion_record(ModelData &data, const std::map<unsigned int, unsigned int> &id_remap, unsigned int num_physical_extruders);

// Restores the source painting and removes the virtual extruders created by a conversion.
void revert_to_source(Model &model, unsigned int num_physical_extruders);

// What Delete All keeps of the ColorMix data: data this build couldn't read, and the record of the
// printer's extruder colors from before ColorMix changed them (so a later Revert restores those).
ModelData data_after_delete_all(const ModelData &data);
// The virtual extruders Delete All keeps: the project's own (not ColorMix's layer words), those a
// conversion replaced included.
FullSpectrum::VirtualExtruders blends_after_delete_all(const Model &model);

// Virtual extruder for a recipe: components with layer-count shares, the explicit layer word and
// the predicted color.
FullSpectrum::VirtualExtruder make_virtual_extruder(unsigned int id, const Recipe &recipe, const Prediction &prediction,
                                                    const std::vector<unsigned int> &tool_of_filament);

} // namespace Slic3r::ColorMix
