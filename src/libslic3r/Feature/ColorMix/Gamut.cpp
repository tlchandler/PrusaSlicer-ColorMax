///|/ ColorMax engine - the range of colors a filament set can print (color range chart).
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "Gamut.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <tuple>

namespace Slic3r::ColorMix {

namespace {

constexpr double PI = 3.14159265358979323846;
// Oklab chroma below which a color counts as gray.
constexpr double GRAY_CHROMA = 0.03;

bool in_srgb(const Oklab &ok)
{
    const LinRGB l = oklab_to_linear(ok);
    return std::all_of(l.begin(), l.end(), [](double c) { return c >= -1e-6 && c <= 1. + 1e-6; });
}

// The closest candidates: Oklab distance to shortlist, CIEDE2000 to choose.
std::pair<int, double> closest(const Lab &target, const std::vector<Candidate> &cands, const std::vector<Oklab> &cand_ok)
{
    const Oklab t = lab_to_oklab(target);
    std::vector<std::pair<double, int>> d;
    d.reserve(cands.size());
    for (size_t i = 0; i < cands.size(); ++i) {
        const double dl = t.L - cand_ok[i].L, da = t.a - cand_ok[i].a, db = t.b - cand_ok[i].b;
        d.emplace_back(dl * dl + da * da + db * db, int(i));
    }
    const size_t k = std::min<size_t>(40, d.size());
    std::partial_sort(d.begin(), d.begin() + k, d.end());
    int    best    = -1;
    double best_de = 1e9;
    for (size_t j = 0; j < k; ++j) {
        const double de = delta_e_2000(target, cands[size_t(d[j].second)].prediction.color);
        if (de < best_de) {
            best_de = de;
            best    = d[j].second;
        }
    }
    return {best, best_de};
}

} // namespace

double GamutChart::hue_of_column(int col) const { return std::fmod(GAMUT_FIRST_HUE + 360. * col / std::max(1, hues), 360.); }

Lab gamut_screen_color(double L, double hue_deg, double chroma_scale)
{
    const double h = hue_deg * PI / 180.;
    double lo = 0., hi = 0.5;
    for (int it = 0; it < 40; ++it) {
        const double c = 0.5 * (lo + hi);
        (in_srgb({L, c * std::cos(h), c * std::sin(h)}) ? lo : hi) = c;
    }
    const double c = lo * std::clamp(chroma_scale, 0., 1.);
    return linear_to_lab(oklab_to_linear({L, c * std::cos(h), c * std::sin(h)}));
}

GamutChart make_gamut_chart(const Bundle &bundle, const Predictor &predictor, const std::vector<unsigned int> &tool_of_filament,
                            const ConvertOptions &options, const std::vector<std::pair<Lab, std::string>> &points, int hues, int rows,
                            double chroma_scale)
{
    GamutChart chart;
    chart.hues         = std::max(1, hues);
    chart.chroma_scale = chroma_scale;
    rows               = std::max(2, rows);
    // Oklab lightness from light to dark (0.95 is about the white of paper, 0.3 a dark brown).
    for (int r = 0; r < rows; ++r)
        chart.lightness.push_back(0.95 - (0.95 - 0.30) * r / (rows - 1));

    ConversionResult res = convert_colors({}, bundle, predictor, tool_of_filament, options);
    if (!res.problem.empty()) {
        chart.problem = res.problem;
        return chart;
    }
    chart.candidates = std::move(res.candidates);
    std::vector<Oklab> cand_ok;
    for (const Candidate &c : chart.candidates)
        cand_ok.push_back(lab_to_oklab(c.prediction.color));

    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < chart.columns(); ++c) {
            GamutCell cell;
            cell.target = c < chart.hues ? gamut_screen_color(chart.lightness[size_t(r)], chart.hue_of_column(c), chroma_scale) :
                                           linear_to_lab(oklab_to_linear({chart.lightness[size_t(r)], 0., 0.}));
            std::tie(cell.candidate, cell.de) = closest(cell.target, chart.candidates, cand_ok);
            chart.cells.push_back(cell);
        }

    const double step = 360. / chart.hues;
    for (const auto &[lab, label] : points) {
        GamutPoint p;
        p.color = lab;
        p.label = label;
        std::tie(p.candidate, p.de) = closest(lab, chart.candidates, cand_ok);
        const Oklab  ok     = lab_to_oklab(lab);
        const double chroma = std::hypot(ok.a, ok.b);
        if (chroma < GRAY_CHROMA)
            p.x = chart.hues + 0.5;
        else {
            const double hue = std::atan2(ok.b, ok.a) * 180. / PI;
            p.x = std::fmod((hue - GAMUT_FIRST_HUE) / step + 0.5 + 2. * chart.hues, double(chart.hues));
        }
        const double top = chart.lightness.front(), bottom = chart.lightness.back();
        p.y = std::clamp((top - ok.L) / (top - bottom) * (rows - 1) + 0.5, 0.05, rows - 0.05);
        chart.points.push_back(std::move(p));
    }
    return chart;
}

} // namespace Slic3r::ColorMix
