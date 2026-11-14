///|/ ColorMax engine - choose a printable recipe for every target color.
///|/ Planner after an earlier standalone converter (keep-distinct branch and bound, color families,
///|/ blend penalty), extended with layer words, banding scores and calibrated prediction.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <set>
#include <string>
#include <vector>

#include "Predictor.hpp"

namespace Slic3r::ColorMix {

struct Target
{
    Lab         color;
    double      weight{1.};   // importance, e.g. dampened visible area
    std::string label;
    // Heights (mm above the bed) where the color is printed, as disjoint [z0, z1] ranges. Used to
    // estimate tool changes; empty = unknown.
    std::vector<std::pair<double, double>> z_ranges;
    // Heights of its up-facing surfaces (tops and shallow slopes): top patches there use all of a
    // recipe's filaments on the top layers.
    std::vector<std::pair<double, double>> top_z_ranges;
};

struct ConvertOptions
{
    bool          use_blends{true};
    bool          keep_distinct{true};
    bool          match_families{true};
    bool          effect_solids{true};      // solid-only filaments (silk etc.) may print a color on their own
    double        blend_penalty{0.75};      // dE2000 handicap of an unmeasured blend vs. a solid
    double        banding_weight{0.15};     // dE2000 per unit of banding score
    double        time_limit_s{20.};
    // Dual walls: also consider (outer word, inner word) pairs. The inner wall may run one layer
    // longer than the outer one and uses words of up to 4 layers with at most 2 filaments.
    bool          dual_walls{false};
    double        dual_penalty{0.6};        // dE2000 handicap: a second word costs tool changes
    // A model color within this of a spool's color - its catalog color or its predicted (measured)
    // one - prints with that spool, pure (0 = off).
    double        spool_snap_de{3.0};
    int           max_virtual{240};         // painting states left for virtual extruders
    LayerSchedule schedule;
    // Merge chosen recipes whose predictions are closer than this (dE2000; 0 = off): fewer
    // regions and tool changes for no visible difference.
    double        merge_de{1.0};
    // Tool changes: every tool used on a layer costs a change. With the targets' z ranges the
    // converter estimates the changes of the whole print and trades them against color error:
    // `change_weight` dE2000 (weighted mean) per tool change saved on every layer (0 = off).
    // Rotations of a layer word that keep its weights are free to choose, so words of different
    // colors are put in step (the same filament on the same layers) where possible.
    double        change_weight{0.5};
    unsigned int  core_tool{0};             // white core tool, present on every layer (0 = none)
    int           top_layers{0};            // top patches: layers under an up-facing surface (0 = off)
};

struct Candidate
{
    Recipe      recipe;
    Prediction  prediction;
    std::string name;     // e.g. "Cyan 67% + White 33%  [C C W]"
    // One filament on every layer of both walls (a dual-wall recipe with a one-filament outer word
    // is a blend: its inner wall differs).
    bool        pure() const { return recipe.word.size() == 1 && recipe.inner_word.empty(); }
};

struct ConversionResult
{
    std::vector<Candidate> candidates;
    std::vector<int>       choice;   // per target: candidate index (-1 = nothing loaded can print it)
    std::vector<double>    de;       // per target: CIEDE2000 target vs. predicted
    double                 weighted_mean_de{0.};
    double                 worst_de{0.};
    int                    blends_used{0};
    bool                   optimal{true};
    std::string            problem;  // non-empty when nothing could be done
    // Estimated tool changes of the print (sum over layers of tools used - 1), before and after
    // the tool-change optimization; -1 when the targets carry no z ranges.
    int                    tool_changes{-1};
    int                    tool_changes_before{-1};
};

// Estimated tool changes of printing `targets` with `choice` (indices into `candidates`):
// sum over layers of (number of tools used on the layer - 1). -1 without z ranges.
// Tools (1-based) used on each layer by the estimate above (empty sets on unused layers).
std::vector<std::set<unsigned int>> estimate_tools_per_layer(const std::vector<Target> &targets, const std::vector<Candidate> &candidates,
                                                             const std::vector<int> &choice, const std::vector<unsigned int> &tool_of_filament,
                                                             const LayerSchedule &schedule, unsigned int core_tool, int top_layers = 0);
int estimate_tool_changes(const std::vector<Target> &targets, const std::vector<Candidate> &candidates, const std::vector<int> &choice,
                          const std::vector<unsigned int> &tool_of_filament, const LayerSchedule &schedule, unsigned int core_tool,
                          int top_layers = 0);

// Color family (black, white, grey, brown, red, orange, yellow, green, blue, purple, pink).
std::string color_family(const Lab &lab);
// Extra cost for printing a color of family `target` with one of family `got` at distance `de`.
double family_penalty(const std::string &target, const std::string &got, double de);

// Area -> weight, dampened by a square root so small details still count; mean weight 1.
std::vector<double> area_weights(const std::vector<double> &areas);

// tool_of_filament: per bundle filament, 1-based tool it is loaded in (0 = not loaded).
ConversionResult convert_colors(const std::vector<Target> &targets, const Bundle &bundle,
                                const Predictor &predictor, const std::vector<unsigned int> &tool_of_filament,
                                const ConvertOptions &options);

std::string recipe_name(const Recipe &r, const Bundle &bundle);

} // namespace Slic3r::ColorMix
