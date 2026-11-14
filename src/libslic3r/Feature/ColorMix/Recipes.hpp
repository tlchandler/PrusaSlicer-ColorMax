///|/ ColorMax engine - layer words (recipes), striping rules and layer schedules.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <vector>

#include "Bundle.hpp"

namespace Slic3r::ColorMix {

// Layer heights as the slicer will print them: layer 0 has first_layer_height,
// layer L >= 1 has heights[(L - 1) % heights.size()].
struct LayerSchedule
{
    double              first_layer_height{0.2};
    std::vector<double> heights{0.1};

    bool   uniform() const;
    double height(size_t layer_idx) const;
    double average() const;
    bool   operator==(const LayerSchedule &o) const { return first_layer_height == o.first_layer_height && heights == o.heights; }
};

// A word: the filament (bundle index) of each layer of one repeat. Layer L of the
// object uses word[L % word.size()] - the phase is the absolute layer index.
using Word = std::vector<int>;

struct Recipe
{
    Word                word;
    std::vector<double> weights;   // per bundle filament: height-weighted share, sums to 1
    int                 num_filaments{0};
    double              banding{0.};  // visible-banding score (Oklab x 100), lower is better
    // Dual walls: the inner wall's word and weights (empty = both walls follow `word`).
    Word                inner_word;
    std::vector<double> inner_weights;
    bool                dual() const { return !inner_word.empty(); }
};

// Circular checks of the striping rules.
int  circular_max_run(const Word &w);
bool satisfies_rules(const Word &w, const RecipeRules &rules);

// Height-weighted shares of each bundle filament for a word on a schedule (over the
// common period of the two, layers >= 1).
std::vector<double> word_weights(const Word &w, size_t num_bundle_filaments, const LayerSchedule &schedule);

// Every word over `filaments` (bundle indices) that satisfies `rules`, with its weights.
// With a uniform schedule, rotations are folded (they look the same); with a varying
// schedule each rotation is a distinct recipe because it lands differently on the
// thick and thin layers. Recipes with identical weights are merged (lowest banding kept).
// Pure filaments are included as one-letter words.
std::vector<Recipe> enumerate_recipes(const std::vector<int> &filaments, size_t num_bundle_filaments,
                                      const RecipeRules &rules, const LayerSchedule &schedule,
                                      const std::vector<Lab> &filament_colors);

// Banding score of a word: single-layer colors smoothed along Z with Gaussian windows of
// 0.2, 0.4 and 0.8 mm; largest deviation from the mean (lightness counted twice).
double banding_score(const Word &w, const LayerSchedule &schedule, const std::vector<Lab> &filament_colors);

// Most-even word for given counts (deficit rule), used for user-specified ratios.
Word even_word(const std::vector<std::pair<int, int>> &filament_counts);

} // namespace Slic3r::ColorMix
