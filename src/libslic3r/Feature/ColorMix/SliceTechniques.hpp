///|/ ColorMax engine - slicing techniques applied to the multi-material segmentation.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <functional>
#include <vector>

#include "ModelData.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp"

namespace Slic3r::ColorMix {

// Replaces FullSpectrum::remap_virtual_extruders_to_physical() when ColorMix slice settings are active.
//  1. Top-surface patches: the part of a virtual-extruder region within `top_layers` of an exposed
//     top (inset by `top_wall_inset`) is tiled with square cells in an ordered 8x8 Bayer pattern;
//     each cell goes to one physical component in proportion to the recipe.
//  1b. Dual walls: a virtual extruder with an inner word is split on every layer into the outer ring
//     (`dual_wall_width` wide, follows the outer word) and the rest (follows the inner word).
//  2. Everything else of the virtual region gets the physical extruder of its layer word.
//  3. White core: on every layer, the area deeper than `wall_depth` below the layer outline (and not
//     part of a top patch, nor within `top_shell_layers` of a top / `bottom_shell_layers` of a bottom
//     surface, so flat tops and bottoms keep their color) moves to `core_extruder`. Unpainted
//     interior goes to the core too, unless it belongs to an unpainted part reaching the surface.
// segmentation[layer][slot]: slot = 0-based extruder id; virtual slots are emptied.
// layer_outlines[layer]: union of the object's slices on that layer.
void apply_slice_techniques(std::vector<std::vector<ExPolygons>> &segmentation,
                            const std::vector<ExPolygons>        &layer_outlines,
                            const std::vector<double>            &print_z_per_layer,
                            unsigned int                          num_physical_extruders,
                            const FullSpectrum::VirtualExtruders &virtual_extruders,
                            const SliceSettings                  &settings,
                            const std::function<void()>          &throw_on_cancel,
                            int                                   top_shell_layers    = 0,
                            int                                   bottom_shell_layers = 0,
                            double                                first_layer_height  = 0.);

// Region cleanup (see SliceSettings::region_cleanup), on segmentation[layer][slot] before any
// other technique. Exposed for tests; apply_slice_techniques() calls it when enabled.
void cleanup_regions(std::vector<std::vector<ExPolygons>> &segmentation,
                     const std::vector<ExPolygons>        &layer_outlines,
                     const std::vector<double>            &print_z_per_layer,
                     const SliceSettings                  &settings,
                     const std::function<void()>          &throw_on_cancel,
                     double                                first_layer_height = 0.); // 0 = print_z[0]

// 8x8 Bayer threshold in [0, 64).
int bayer8(int x, int y);

// Physical extruder (1-based) of a cell with Bayer threshold t (0..63) for a recipe given as
// (physical extruder, share) pairs. Shares are converted to 64 units summing to exactly 64.
unsigned int bayer_pick(const std::vector<std::pair<unsigned int, double>> &shares, int threshold);

// (physical extruder, share) of a virtual extruder: from its explicit layer word when present,
// else from its component ratios.
std::vector<std::pair<unsigned int, double>> virtual_extruder_shares(const FullSpectrum::VirtualExtruder &ve);
// Same, weighted by the layer heights of `print_z` (top patches: match the predicted color).
std::vector<std::pair<unsigned int, double>> virtual_extruder_shares(const FullSpectrum::VirtualExtruder &ve, const std::vector<double> &print_z);

} // namespace Slic3r::ColorMix
