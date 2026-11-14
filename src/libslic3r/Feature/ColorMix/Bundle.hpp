///|/ ColorMax engine - filament bundles (data-driven filament sets + calibration data).
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ColorSpace.hpp"

namespace Slic3r::ColorMix {

// One spool type in a bundle.
// How a filament takes part in mixes.
//  Mix:    any layer recipe (and printed on its own).
//  Accent: e.g. glitter: calibrated with a targeted set of swatches; conversions don't use it on their
//          own (meant for colors where sparkle is asked for).
//  Solid:  printed on its own only (e.g. silk, marble); one solid swatch on calibration plates.
enum class MixRole { Mix, Accent, Solid };
const char *mix_role_key(MixRole role);              // "mix" / "accent" / "solid"
MixRole     mix_role_from_key(const std::string &key); // unknown -> Mix

struct BundleFilament
{
    std::string id;              // stable key, e.g. "cyan"
    std::string name;            // display name, e.g. "ColorMix Cyan"
    std::string color;           // nominal "#RRGGBB"
    unsigned int default_tool{0}; // 1-based tool suggestion, 0 = none
    MixRole role{MixRole::Mix};
    std::string filament_preset; // optional PrusaSlicer filament preset name
    double td{0.};               // transmission distance in mm (HueForge TD; 0 = unknown): how far light goes in
    bool   mixes() const { return role == MixRole::Mix; }
};

// Striping rules for layer words (layer alternation in Z).
struct RecipeRules
{
    int max_run{3};               // most consecutive layers of one filament (circular)
    int recurrence_window{4};     // every filament of a word appears in every window of this many layers
    int max_word_length{6};       // longest repeat, in layers
    int max_filaments{3};         // filaments per recipe
};

// A measured swatch: a layer word (bundle filament ids, bottom to top, repeating)
// or plain ratios, and its measured color.
struct Swatch
{
    std::vector<std::string> word;                        // preferred: exact word
    std::vector<std::string> inner_word;                  // dual walls: the inner wall's word (empty = same)
    std::vector<std::pair<std::string, double>> ratios;    // alternative: filament id -> share
    Lab measured;
    bool held_out{false};                                 // validation only, not used for fitting
    std::string note;
};

// Measurements valid for one set of print conditions.
struct Calibration
{
    std::string id;
    std::string date;
    std::string printer;
    std::string notes;
    double first_layer_height{0.2};
    std::vector<double> layer_schedule;  // repeating layer heights above the first layer; {h} = uniform
    // How the swatches were printed (the core shows through the walls: part of the measured color).
    bool   white_core{false};
    double wall_depth{0.};       // 0 = unknown
    double dual_wall_width{0.};  // 0 = unknown
    std::string plate_file;      // the plate file of the last plate measured for it (empty = unknown)
    std::vector<Swatch> swatches;
};

struct Bundle
{
    std::string id;
    std::string name;
    std::string material{"PLA"};
    std::string description;
    std::string source_path;      // file it was loaded from
    bool        user_bundle{false};
    std::vector<BundleFilament> filaments;
    RecipeRules rules;
    std::vector<Calibration> calibrations;

    int filament_index(const std::string &id) const;
    // Filaments with role Mix.
    std::vector<int> mixable_indices() const;
};

// Creating and editing filament sets (bundles) in the GUI.
// A stable id made from a display name ("Matte Sky Blue" -> "matte-sky-blue"), different from every
// id in `taken` (a number is appended when needed). `fallback` when the name has no letters or digits.
std::string make_id(const std::string &name, const std::vector<std::string> &taken, const std::string &fallback = "filament");
// What is wrong with a filament set before it can be saved and used (empty = fine): a name, at least
// two filaments that mix, unique filament ids and names, valid colors.
std::string check_bundle(const Bundle &bundle);

// Reads a bundle JSON file. Returns std::nullopt and fills `error` on failure.
std::optional<Bundle> load_bundle(const std::string &path, std::string *error = nullptr);
std::optional<Bundle> parse_bundle(const std::string &json_text, std::string *error = nullptr);
std::string           bundle_to_json(const Bundle &bundle);
bool                  save_bundle(const Bundle &bundle, const std::string &path, std::string *error = nullptr);

// Loads every *.json bundle from `system_dir` and `user_dir` (user bundles override system ones
// with the same id). Unreadable files are reported through `errors`.
std::vector<Bundle> load_bundles(const std::string &system_dir, const std::string &user_dir,
                                 std::vector<std::string> *errors = nullptr);

// Tools that may print the white core: the tool holding the bundle's White (filament id "white"),
// or a tool holding none of the bundle's filaments (an extra spool, e.g. opaque white). Ascending.
std::vector<unsigned int> allowed_core_tools(const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament,
                                             unsigned int num_physical);
// The tool holding the bundle's White, 0 if none.
unsigned int white_tool(const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament);
// Display color of the core tool: the bundle's White, or plain white for an extra spool.
std::string core_tool_color(const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament, unsigned int core_tool);

// The Prusament PLA ColorMix bundle as shipped (used when the resource file is missing).
Bundle builtin_colormix_bundle();

} // namespace Slic3r::ColorMix
