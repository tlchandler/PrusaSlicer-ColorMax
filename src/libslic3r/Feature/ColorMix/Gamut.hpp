///|/ ColorMax engine - the range of colors a filament set can print (color range chart).
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <string>
#include <vector>

#include "Converter.hpp"

namespace Slic3r::ColorMix {

struct GamutCell
{
    Lab    target;           // the screen color of the cell
    int    candidate{-1};    // closest printable recipe (index into GamutChart::candidates)
    double de{0.};           // CIEDE2000 target vs. its prediction
};

struct GamutPoint
{
    Lab         color;
    std::string label;
    int         candidate{-1};
    double      de{0.};
    double      x{0.}, y{0.};   // position on the chart in cells (column, row), cell (c, r) spans [c, c+1) x [r, r+1)
};

// A grid of hues (columns, then one column of grays) by lightness (rows, lightest first). Each cell holds a
// screen color and the closest color the set can print.
struct GamutChart
{
    int                     hues{0};
    std::vector<double>     lightness;     // Oklab L per row
    double                  chroma_scale{1.};
    std::vector<GamutCell>  cells;         // row-major, lightness.size() rows x (hues + 1) columns
    std::vector<Candidate>  candidates;
    std::vector<GamutPoint> points;
    std::string             problem;       // non-empty when nothing can be printed

    int              columns() const { return hues + 1; }
    int              rows() const { return int(lightness.size()); }
    const GamutCell &cell(int row, int col) const { return cells[size_t(row * columns() + col)]; }
    double           hue_of_column(int col) const;   // degrees (Oklab hue)
};

// Hue of the first column: sRGB red, so the columns run red, orange, yellow, green, cyan, blue, purple.
constexpr double GAMUT_FIRST_HUE = 29.;

// The most saturated screen (sRGB) color of this Oklab lightness and hue, its chroma times `chroma_scale`.
Lab gamut_screen_color(double ok_lightness, double hue_deg, double chroma_scale);

// `points`: colors to place on the chart (e.g. the model's colors) with labels.
GamutChart make_gamut_chart(const Bundle &bundle, const Predictor &predictor, const std::vector<unsigned int> &tool_of_filament,
                            const ConvertOptions &options, const std::vector<std::pair<Lab, std::string>> &points, int hues = 24,
                            int rows = 9, double chroma_scale = 1.);

} // namespace Slic3r::ColorMix
