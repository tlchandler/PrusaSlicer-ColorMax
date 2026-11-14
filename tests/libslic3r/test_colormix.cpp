#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <set>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <boost/filesystem.hpp>
#include "libslic3r/Feature/ColorMix/Bundle.hpp"
#include "libslic3r/Feature/ColorMix/ColorSpace.hpp"
#include "libslic3r/Feature/ColorMix/Calibration.hpp"
#include "libslic3r/Feature/ColorMix/Converter.hpp"
#include "libslic3r/Feature/ColorMix/FilamentDb.hpp"
#include "libslic3r/Feature/ColorMix/Gamut.hpp"
#include "libslic3r/Feature/ColorMix/ModelConvert.hpp"
#include "libslic3r/Feature/ColorMix/ModelData.hpp"
#include "libslic3r/Feature/ColorMix/Predictor.hpp"
#include "libslic3r/Feature/ColorMix/Recipes.hpp"
#include "libslic3r/Feature/ColorMix/SliceTechniques.hpp"
#include "libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp"

#include <prusa_fdm_mixer.hpp>

using namespace Slic3r;
using namespace Slic3r::ColorMix;
using Catch::Approx;

TEST_CASE("ColorMix color space", "[ColorMix]")
{
    // Sharma et al. CIEDE2000 test pair 1.
    CHECK(delta_e_2000({50., 2.6772, -79.7751}, {50., 0., -82.7485}) == Approx(2.0425).margin(1e-3));
    CHECK(delta_e_2000({50., 0., 0.}, {50., -1., 2.}) == Approx(2.3669).margin(1e-3));
    for (const char *hex : {"#00AED5", "#D25B96", "#FFCE02", "#F1E8DD", "#544F4D"})
        CHECK(lab_to_hex(hex_to_lab(hex)) == normalize_hex(hex));
    CHECK(normalize_hex("abc") == "");
    CHECK(normalize_hex("00aed5") == "#00AED5");
}

TEST_CASE("ColorMix mixer port agrees with bundled prusa_fdm_mixer", "[ColorMix]")
{
    const std::vector<std::vector<std::pair<std::string, double>>> cases = {
        {{"#00AED5", 0.5}, {"#FFCE02", 0.5}},
        {{"#D25B96", 0.25}, {"#F1E8DD", 0.75}},
        {{"#00AED5", 1. / 3.}, {"#D25B96", 1. / 3.}, {"#FFCE02", 1. / 3.}},
        {{"#544F4D", 0.5}, {"#F1E8DD", 0.5}},
    };
    for (const auto &c : cases) {
        std::vector<prusa_fdm_mixer::Part>   parts;
        std::vector<std::pair<Lab, double>> labs;
        for (const auto &[hex, r] : c) {
            parts.push_back({hex, r});
            labs.emplace_back(hex_to_lab(hex), r);
        }
        const Lab ref  = hex_to_lab(prusa_fdm_mixer::mix(parts));
        const Lab mine = fdm_mixer_predict(labs);
        CHECK(delta_e_2000(ref, mine) < 1.0);
    }
}

TEST_CASE("ColorMix striping rules", "[ColorMix]")
{
    RecipeRules rules{3, 4, 6, 3};
    CHECK(satisfies_rules({0, 0, 0, 1}, rules));          // 3:1
    CHECK(!satisfies_rules({0, 0, 0, 0, 1}, rules));      // run of 4
    CHECK(!satisfies_rules({0, 0, 1, 0, 0}, rules));      // circular run of 4
    CHECK(satisfies_rules({0, 1, 0, 2}, rules));          // 2:1:1
    CHECK(!satisfies_rules({0, 0, 1, 1, 2, 2}, rules));   // 0 missing from window [1 1 2 2]
    RecipeRules strict{2, 3, 6, 3};
    CHECK(!satisfies_rules({0, 0, 0, 1}, strict));
    CHECK(satisfies_rules({0, 0, 1}, strict));
    CHECK(circular_max_run({0, 1, 1, 0, 0}) == 3);
}

TEST_CASE("ColorMix layer schedule weights", "[ColorMix]")
{
    LayerSchedule sched;
    sched.first_layer_height = 0.2;
    sched.heights            = {0.12, 0.10, 0.08};
    // Layer L >= 1 uses word[L % 3] and height[(L - 1) % 3]: M gets 0.12, Y 0.10, C 0.08.
    const std::vector<double> w = word_weights({0, 1, 2}, 3, sched);
    CHECK(w[0] == Approx(0.08 / 0.30));
    CHECK(w[1] == Approx(0.12 / 0.30));
    CHECK(w[2] == Approx(0.10 / 0.30));
    // A length-2 word on a length-3 schedule averages out to 50:50.
    const std::vector<double> w2 = word_weights({0, 1}, 2, sched);
    CHECK(w2[0] == Approx(0.5));

    LayerSchedule uniform;
    uniform.heights = {0.1};
    CHECK(word_weights({0, 0, 1}, 2, uniform)[0] == Approx(2. / 3.));
}

TEST_CASE("ColorMix recipe enumeration", "[ColorMix]")
{
    const Bundle      b = builtin_colormix_bundle();
    std::vector<Lab>  colors;
    for (const auto &f : b.filaments)
        colors.push_back(hex_to_lab(f.color));
    LayerSchedule uniform;
    const std::vector<Recipe> rs = enumerate_recipes(b.mixable_indices(), b.filaments.size(), b.rules, uniform, colors);
    std::set<std::vector<long>> seen;
    bool has_31 = false, has_pure = false;
    for (const Recipe &r : rs) {
        CHECK(satisfies_rules(r.word, b.rules));
        std::vector<long> key;
        for (double v : r.weights)
            key.push_back(std::lround(v * 1e4));
        CHECK(seen.insert(key).second); // no duplicate mixes
        has_31 |= r.num_filaments == 2 && std::abs(*std::max_element(r.weights.begin(), r.weights.end()) - 0.75) < 1e-6;
        has_pure |= r.num_filaments == 1;
    }
    CHECK(has_31);
    CHECK(has_pure);
    CHECK(rs.size() > 45);

    LayerSchedule varying;
    varying.heights = {0.12, 0.10, 0.08};
    CHECK(enumerate_recipes(b.mixable_indices(), b.filaments.size(), b.rules, varying, colors).size() > rs.size());
}

TEST_CASE("ColorMix Bayer pattern", "[ColorMix]")
{
    std::set<int> values;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            values.insert(bayer8(x, y));
    CHECK(values.size() == 64);
    CHECK(*values.begin() == 0);
    CHECK(*values.rbegin() == 63);
    // 3:1 recipe covers 48 of 64 cells with the main filament.
    int main = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            main += bayer_pick({{1, 0.75}, {2, 0.25}}, bayer8(x, y)) == 1;
    CHECK(main == 48);
}

TEST_CASE("ColorMix predictor calibration", "[ColorMix]")
{
    Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    Predictor uncal(b, nullptr, sched);
    CHECK(!uncal.calibrated());

    // Synthetic truth: the base mixer, 4 units darker. Measure a set of pairs.
    Calibration cal;
    cal.layer_schedule = {0.1};
    const std::vector<std::string> ids = {"cyan", "magenta", "yellow", "white", "black"};
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t j = i + 1; j < ids.size(); ++j)
            for (const std::vector<std::string> &w : {std::vector<std::string>{ids[i], ids[j]},
                                                     std::vector<std::string>{ids[i], ids[i], ids[j]},
                                                     std::vector<std::string>{ids[i], ids[j], ids[j]}}) {
                Swatch s;
                s.word = w;
                Lab truth = uncal.predict(Predictor::swatch_weights(b, s, sched)).color;
                truth.L -= 4.;
                s.measured = truth;
                cal.swatches.push_back(s);
            }
    Predictor p(b, &cal, sched);
    CHECK(p.calibrated());
    // A measured recipe reproduces exactly.
    Swatch s0 = cal.swatches.front();
    const Prediction exact = p.predict(Predictor::swatch_weights(b, s0, sched));
    CHECK(exact.measured);
    CHECK(delta_e_2000(exact.color, s0.measured) < 1e-6);
    // An unmeasured recipe is corrected toward the darker truth.
    Swatch q;
    q.word = {"cyan", "cyan", "cyan", "white"};
    const std::vector<double> wq = Predictor::swatch_weights(b, q, sched);
    Lab truth = uncal.predict(wq).color;
    truth.L -= 4.;
    CHECK(delta_e_2000(p.predict(wq).color, truth) < delta_e_2000(uncal.predict(wq).color, truth));
    CHECK(p.report().loo_median < p.report().base_median);
}

TEST_CASE("ColorMix converter", "[ColorMix]")
{
    const Bundle  b = builtin_colormix_bundle();
    LayerSchedule sched;
    Predictor     p(b, nullptr, sched);
    std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 6, 7};
    std::vector<Target> targets = {{hex_to_lab("#00AED5"), 1., "cyan"},
                                   {hex_to_lab("#FFCE02"), 1., "yellow"},
                                   {hex_to_lab("#2E9E5B"), 1., "green"},
                                   {hex_to_lab("#808080"), 1., "grey"}};
    ConvertOptions opt;
    opt.schedule = sched;
    const ConversionResult r = convert_colors(targets, b, p, tools, opt);
    REQUIRE(r.problem.empty());
    REQUIRE(r.choice.size() == 4);
    CHECK(r.candidates[size_t(r.choice[0])].pure());
    CHECK(r.de[0] < 0.5);
    CHECK(!r.candidates[size_t(r.choice[2])].pure()); // green needs a blend
    CHECK(color_family(r.candidates[size_t(r.choice[2])].prediction.color) == "green");
    CHECK(r.choice[2] != r.choice[3]);
}

TEST_CASE("Virtual extruder explicit sequence", "[ColorMix]")
{
    FullSpectrum::VirtualExtruder ve;
    ve.id         = 9;
    ve.components = {{1, 2. / 3.}, {4, 1. / 3.}};
    ve.sequence   = {1, 1, 4};
    const auto norm = FullSpectrum::normalize_virtual_extruders({ve});
    REQUIRE(norm.size() == 1);
    CHECK(norm.front().sequence == std::vector<unsigned int>{1, 1, 4});
    const auto layers = norm.front().resolve_all_layers({0.2, 0.3, 0.4, 0.5, 0.6, 0.7});
    CHECK(layers == std::vector<unsigned int>{1, 1, 4, 1, 1, 4});
    const std::string json = FullSpectrum::serialize_virtual_extruders_to_json({"#000000", "#000000", "#000000", "#FFFFFF"}, norm);
    const auto back = FullSpectrum::deserialize_virtual_extruders_from_json(json);
    REQUIRE(back.virtual_extruders.size() == 1);
    CHECK(back.virtual_extruders.front().sequence == ve.sequence);

    FullSpectrum::VirtualExtruder bad = ve;
    bad.sequence = {1, 4, 4};   // does not match the 2:1 ratio
    CHECK(FullSpectrum::normalize_virtual_extruders({bad}).front().sequence.empty());
}

TEST_CASE("ColorMix layer schedule slicing", "[ColorMix]")
{
    SlicingParameters sp;
    sp.first_object_layer_height = 0.2;
    sp.first_print_layer_height  = 0.2;
    sp.layer_height              = 0.1;
    sp.min_layer_height          = 0.05;
    sp.max_layer_height          = 0.25;
    sp.object_print_z_min        = 0.;
    sp.object_print_z_max        = 1.;
    sp.object_print_z_uncompensated_max = 1.;
    sp.object_shrinkage_compensation_z  = 1.;
    sp.valid = true;
    sp.set_layer_schedule({0.12, 0.10, 0.08});
    const std::vector<coordf_t> layers = generate_object_layers(sp, {0., 0.1});
    REQUIRE(layers.size() >= 4);
    CHECK(layers[1] == Approx(0.2));
    CHECK(layers[3] - layers[2] == Approx(0.12));
    CHECK(layers[5] - layers[4] == Approx(0.10));
    CHECK(layers[7] - layers[6] == Approx(0.08));
}

TEST_CASE("ColorMix bundle and project data JSON", "[ColorMix]")
{
    Bundle b = builtin_colormix_bundle();
    Calibration c;
    c.id             = "test";
    c.layer_schedule = {0.1};
    Swatch s;
    s.word     = {"cyan", "white"};
    s.measured = {60., -20., -10.};
    c.swatches.push_back(s);
    b.calibrations.push_back(c);
    std::string err;
    const auto back = parse_bundle(bundle_to_json(b), &err);
    REQUIRE(back.has_value());
    CHECK(back->filaments.size() == 7);
    CHECK(back->calibrations.size() == 1);
    CHECK(back->calibrations[0].swatches[0].measured.L == Approx(60.));
    CHECK(!parse_bundle("{\"format\":\"nope\"}", &err).has_value());

    ModelData d;
    d.slice.core_extruder  = 4;
    d.slice.top_pattern    = true;
    d.slice.layer_schedule = {0.12, 0.1, 0.08};
    d.source_colors        = {"", "#FF0000"};
    d.bundle_id            = b.id;
    ModelData d2;
    REQUIRE(model_data_from_json(model_data_to_json(d), d2));
    CHECK(d2 == d);
}

#include "libslic3r/Feature/ColorMix/PhotoCalibration.hpp"
#include "libslic3r/Feature/ColorMix/Calibration.hpp"

TEST_CASE("ColorMix photo calibration reads a board under uneven, tinted light", "[ColorMix]")
{
    BoardLayout L;
    REQUIRE(L.swatches_per_board() == L.rows * L.cols);
    // Truth: a spread of colors for every swatch slot.
    std::vector<Lab> truth;
    for (int i = 0; i < L.swatches_per_board(); ++i)
        truth.push_back(linear_to_lab({0.03 + 0.8 * ((i * 7) % 11) / 10., 0.03 + 0.8 * ((i * 5) % 13) / 12., 0.03 + 0.8 * ((i * 3) % 7) / 6.}));
    // Board placed in the photo by a rotation + scale (4 px/mm), with paper around it.
    const int    Wpx = 1100, Hpx = 500;
    const double sc = 4.2, ang = 0.06, ox = 120., oy = 90.;
    auto to_px = [&](double x, double y) { // face mm (y up) -> pixel (v down)
        const double X = x * std::cos(ang) - (L.height() - y) * std::sin(ang);
        const double Y = x * std::sin(ang) + (L.height() - y) * std::cos(ang);
        return std::array<double, 2>{ox + sc * X, oy + sc * Y};
    };
    auto to_mm = [&](double u, double v) {
        const double X = (u - ox) / sc, Y = (v - oy) / sc;
        const double x = X * std::cos(ang) + Y * std::sin(ang);
        const double yd = -X * std::sin(ang) + Y * std::cos(ang);
        return std::array<double, 2>{x, L.height() - yd};
    };
    // Light: brighter on the left and top, warm cast.
    auto light = [&](double x, double y) {
        const double l = 0.75 + 0.25 * (1. - x / L.width()) + 0.1 * (y / L.height()) - 0.05 * (x / L.width()) * (y / L.height());
        return std::array<double, 3>{0.9 * l * 1.08, 0.9 * l, 0.9 * l * 0.82};
    };
    const double paper = 0.88;
    PhotoImage img;
    img.width  = Wpx;
    img.height = Hpx;
    img.rgb.resize(size_t(Wpx) * Hpx * 3);
    const auto cells = L.swatch_cells();
    for (int v = 0; v < Hpx; ++v)
        for (int u = 0; u < Wpx; ++u) {
            const auto mm = to_mm(u + 0.5, v + 0.5);
            LinRGB refl{paper, paper, paper};
            const bool on_board = mm[0] >= 0 && mm[0] <= L.width() && mm[1] >= 0 && mm[1] <= L.height();
            if (on_board) {
                refl = {0.03, 0.03, 0.03}; // frame
                for (int r = 0; r < L.rows; ++r)
                    for (int c = 0; c < L.cols; ++c) {
                        const auto rc = L.cell_rect(r, c);
                        if (mm[0] < rc[0] || mm[0] > rc[2] || mm[1] < rc[1] || mm[1] > rc[3])
                            continue;
                        for (size_t k = 0; k < cells.size(); ++k)
                            if (cells[k][0] == r && cells[k][1] == c)
                                refl = lab_to_linear(truth[k]);
                    }
            }
            const auto li = light(mm[0], mm[1]);
            for (int ch = 0; ch < 3; ++ch)
                img.rgb[(size_t(v) * Wpx + u) * 3 + ch] = (unsigned char) std::lround(linear_to_srgb8(std::clamp(refl[ch], 0., 1.) * li[ch]));
        }
    BoardCorners corners = {to_px(0., L.height()), to_px(L.width(), L.height()), to_px(L.width(), 0.), to_px(0., 0.)};
    const PhotoReading pr = read_board_photo(img, corners, L);
    REQUIRE(pr.cells.size() == truth.size());
    CHECK(pr.paper_samples > 20);
    double worst = 0.;
    for (size_t i = 0; i < truth.size(); ++i)
        worst = std::max(worst, delta_e_2000(truth[i], pr.cells[i]));
    INFO("worst dE " << worst << " warning: " << pr.warning);
    CHECK(worst < 2.0);

    // A wrong corner order (board rotated by 180 degrees) puts every swatch on the wrong cell.
    BoardCorners flipped = {corners[2], corners[3], corners[0], corners[1]};
    bool ok = true;
    try {
        const PhotoReading bad = read_board_photo(img, flipped, L);
        double w2 = 0.;
        for (size_t i = 0; i < truth.size(); ++i)
            w2 = std::max(w2, delta_e_2000(truth[i], bad.cells[i]));
        ok = w2 > 10.;
    } catch (const std::exception &) {
        ok = true;
    }
    CHECK(ok);
}

TEST_CASE("ColorMix photo calibration averages resolved layer stripes", "[ColorMix]")
{
    // A sharp photo (12 px/mm) of 0.1 mm layers alternating between two filaments: the stripes are
    // resolved, and the reading must be the blend (linear-light mean), not one of the filaments.
    BoardLayout  L;
    const double sc = 12., ox = 60., oy = 60., layer = 0.1;
    const int    Wpx = int(2 * ox + sc * L.width()), Hpx = int(2 * oy + sc * L.height());
    const double paper = 0.88;
    const auto   cells = L.swatch_cells();
    std::vector<LinRGB> A, B;
    std::vector<std::vector<int>> word; // 0 = A, 1 = B per layer
    std::vector<Lab>  truth;
    const std::vector<LinRGB> pal = {{0.02, 0.02, 0.02}, {0.85, 0.85, 0.85}, {0.6, 0.05, 0.05}, {0.04, 0.3, 0.6}, {0.7, 0.6, 0.03}};
    const std::vector<std::vector<int>> words = {{0, 1}, {0, 0, 1}, {0, 1, 1, 1}, {0, 1, 1}};
    for (size_t k = 0; k < cells.size(); ++k) {
        A.push_back(pal[k % pal.size()]);
        B.push_back(pal[(k * 3 + 1) % pal.size()]);
        if (A.back()[0] == B.back()[0])
            B.back() = pal[(k + 2) % pal.size()];
        word.push_back(words[k % words.size()]);
        LinRGB m{0., 0., 0.};
        for (int w : word.back())
            for (int c = 0; c < 3; ++c)
                m[c] += (w ? B.back()[c] : A.back()[c]) / double(word.back().size());
        truth.push_back(linear_to_lab(m));
    }
    PhotoImage img;
    img.width  = Wpx;
    img.height = Hpx;
    img.rgb.resize(size_t(Wpx) * Hpx * 3);
    for (int v = 0; v < Hpx; ++v)
        for (int u = 0; u < Wpx; ++u) {
            LinRGB acc{0., 0., 0.};
            for (int sub = 0; sub < 4; ++sub) { // the sensor integrates over the pixel
                const double x = (u + 0.5 - ox) / sc, y = L.height() - (v + (sub + 0.5) / 4. - oy) / sc;
                LinRGB       refl{paper, paper, paper};
                if (x >= 0 && x <= L.width() && y >= 0 && y <= L.height()) {
                    refl = {0.03, 0.03, 0.03};
                    for (int r = 0; r < L.rows; ++r)
                        for (int c = 0; c < L.cols; ++c) {
                            const auto rc = L.cell_rect(r, c);
                            if (x < rc[0] || x > rc[2] || y < rc[1] || y > rc[3])
                                continue;
                            for (size_t k = 0; k < cells.size(); ++k)
                                if (cells[k][0] == r && cells[k][1] == c) {
                                    const int li = int(std::floor(y / layer));
                                    refl = word[k][size_t(li % int(word[k].size()))] ? B[k] : A[k];
                                }
                        }
                }
                for (int ch = 0; ch < 3; ++ch)
                    acc[ch] += 0.25 * 0.9 * refl[ch];
            }
            for (int ch = 0; ch < 3; ++ch)
                img.rgb[(size_t(v) * Wpx + u) * 3 + ch] = (unsigned char) std::lround(linear_to_srgb8(std::clamp(acc[ch], 0., 1.)));
        }
    auto to_px = [&](double x, double y) { return std::array<double, 2>{ox + sc * x, oy + sc * (L.height() - y)}; };
    BoardCorners corners = {to_px(0., L.height()), to_px(L.width(), L.height()), to_px(L.width(), 0.), to_px(0., 0.)};
    const PhotoReading pr = read_board_photo(img, corners, L);
    REQUIRE(pr.cells.size() == truth.size());
    double worst = 0.;
    for (size_t i = 0; i < truth.size(); ++i)
        worst = std::max(worst, delta_e_2000(truth[i], pr.cells[i]));
    INFO("worst dE " << worst << " warning: " << pr.warning);
    CHECK(worst < 2.0);
}

TEST_CASE("ColorMix photo calibration: scanner mode and the back face", "[ColorMix]")
{
    BoardLayout  L;
    const double sc = 6., ox = 120., oy = 120., paper = 0.88;
    const int    Wpx = int(2 * ox + sc * L.width()), Hpx = int(2 * oy + sc * L.height());
    const auto   cells = L.swatch_cells();
    std::vector<Lab> truth;
    for (size_t i = 0; i < cells.size(); ++i)
        truth.push_back(linear_to_lab({0.03 + 0.8 * ((i * 7) % 11) / 10., 0.03 + 0.8 * ((i * 5) % 13) / 12., 0.03 + 0.8 * ((i * 3) % 7) / 6.}));
    truth[0] = linear_to_lab({0.004, 0.25, 0.35}); // a saturated cyan: its red channel is nearly 0
    // background: reflectance outside the face; strips: paper only within 15 mm of the long edges
    // (scanner), otherwise paper everywhere. `mirrored`: the back face (x flipped).
    auto render = [&](double background, bool strips, double window, bool mirrored) {
        PhotoImage img;
        img.width  = Wpx;
        img.height = Hpx;
        img.rgb.resize(size_t(Wpx) * Hpx * 3);
        for (int v = 0; v < Hpx; ++v)
            for (int u = 0; u < Wpx; ++u) {
                double       x = (u + 0.5 - ox) / sc;
                const double y = L.height() - (v + 0.5 - oy) / sc;
                if (mirrored)
                    x = L.width() - x;
                LinRGB refl{background, background, background};
                if (strips && x > -3. && x < L.width() + 3. && ((y > L.height() && y < L.height() + 15.) || (y < 0. && y > -15.)))
                    refl = {paper, paper, paper};
                if (x >= 0 && x <= L.width() && y >= 0 && y <= L.height()) {
                    refl = {0.03, 0.03, 0.03};
                    for (int r = 0; r < L.rows; ++r)
                        for (int c = 0; c < L.cols; ++c) {
                            const auto rc = L.cell_rect(r, c);
                            if (x < rc[0] || x > rc[2] || y < rc[1] || y > rc[3])
                                continue;
                            for (size_t k = 0; k < cells.size(); ++k)
                                if (cells[k][0] == r && cells[k][1] == c)
                                    refl = lab_to_linear(truth[k]);
                        }
                }
                for (int ch = 0; ch < 3; ++ch)
                    img.rgb[(size_t(v) * Wpx + u) * 3 + ch] = (unsigned char) std::lround(linear_to_srgb8(std::clamp(refl[ch] * 0.9, 0., 1.)));
            }
        return img;
    };
    auto px = [&](double x, double y) { return std::array<double, 2>{ox + sc * x, oy + sc * (L.height() - y)}; };
    auto worst_de = [&](const PhotoReading &pr) {
        double w = 0.;
        for (size_t i = 0; i < truth.size(); ++i)
            w = std::max(w, delta_e_2000(truth[i], pr.cells[i]));
        return w;
    };
    const BoardCorners front = {px(0., L.height()), px(L.width(), L.height()), px(L.width(), 0.), px(0., 0.)};

    SECTION("scanner: paper strips on the glass") {
        const PhotoImage img = render(0.55, true, 0.5, false);
        PhotoReadOptions opt;
        opt.scanner          = true;
        const PhotoReading pr = read_board_photo(img, front, L, opt);
        INFO("worst dE " << worst_de(pr) << " warning: " << pr.warning);
        CHECK(pr.paper_samples >= 8);
        CHECK(worst_de(pr) < 2.0);
        // Without paper strips (only the lid, 4 mm above the glass, around the board) a scan can't be read.
        CHECK_THROWS(read_board_photo(render(0.55, false, 0.5, false), front, L, opt));
        CHECK(pr.crushed_cells == 0);
        // Scanner "Brightness" lowered: every value shifted down, dark channels crushed to 0 - warned about.
        PhotoImage dark = img;
        for (unsigned char &c : dark.rgb)
            c = (unsigned char) std::max(0, int(c) - 30);
        const PhotoReading prd = read_board_photo(dark, front, L, opt);
        CHECK(prd.crushed_cells > 0);
        CHECK(prd.warning.find("Brightness") != std::string::npos);
    }
    SECTION("back face: dots at the right end, clicked from the dotted corner clockwise") {
        const PhotoImage img = render(paper, false, paper, true);
        // In the image the front's top-left corner (x = 0) is at the right.
        const BoardCorners back = {px(L.width(), L.height()), px(L.width(), 0.), px(0., 0.), px(0., L.height())};
        const PhotoReading pr   = read_board_photo(img, back, L);
        INFO("worst dE " << worst_de(pr) << " warning: " << pr.warning);
        CHECK(worst_de(pr) < 2.0);
    }
}

TEST_CASE("ColorMix dual-wall virtual extruders and recipes", "[ColorMix]")
{
    FullSpectrum::VirtualExtruder ve;
    ve.id             = 9;
    ve.sequence       = {1, 1, 2};
    ve.inner_sequence = {5};
    ve.components     = {{1, 1. / 3.}, {2, 1. / 6.}, {5, 0.5}};
    const auto norm = FullSpectrum::normalize_virtual_extruders({ve});
    REQUIRE(norm.size() == 1);
    CHECK(norm.front().inner_sequence == std::vector<unsigned int>{5});
    const auto back = FullSpectrum::deserialize_virtual_extruders_from_json(
        FullSpectrum::serialize_virtual_extruders_to_json({"#000000", "#000000", "#000000", "#000000", "#FFFFFF"}, norm));
    REQUIRE(back.virtual_extruders.size() == 1);
    CHECK(back.virtual_extruders.front().inner_sequence == std::vector<unsigned int>{5});

    const Bundle  b = builtin_colormix_bundle();
    LayerSchedule sched;
    Predictor     p(b, nullptr, sched);
    std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    ConvertOptions opt;
    opt.schedule   = sched;
    opt.dual_walls = true;
    // A target exactly on a dual-wall prediction: cyan over black.
    Recipe r;
    r.word          = {0};
    r.weights       = word_weights({0}, b.filaments.size(), sched);
    r.inner_word    = {4};
    r.inner_weights = word_weights({4}, b.filaments.size(), sched);
    const Lab target = p.predict(r).color;
    const ConversionResult res = convert_colors({{target, 1., "deep cyan"}}, b, p, tools, opt);
    REQUIRE(res.problem.empty());
    size_t dual = 0;
    for (const Candidate &c : res.candidates)
        dual += c.recipe.dual();
    CHECK(dual > 100);
    CHECK(res.de[0] < 1.0);

    // Calibrated dual-wall swatches reproduce exactly.
    Calibration cal;
    cal.layer_schedule = {0.1};
    Swatch s;
    s.word       = {"cyan"};
    s.inner_word = {"black"};
    s.measured   = {40., -20., -25.};
    cal.swatches.push_back(s);
    Predictor pc(b, &cal, sched);
    CHECK(delta_e_2000(pc.predict(r).color, s.measured) < 1e-6);
    // ...and do not replace the pure cyan color.
    CHECK(delta_e_2000(pc.filament_colors()[0], hex_to_lab("#00AED5")) < 1e-6);
}

TEST_CASE("ColorMix calibration plan sizes", "[ColorMix]")
{
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const auto none  = plan_calibration_swatches(b, tools, sched, 4, 10, 0);
    const auto basic = plan_calibration_swatches(b, tools, sched, 4, 10, 1);
    const auto full  = plan_calibration_swatches(b, tools, sched, 4, 10, 2);
    CHECK(none.size() == 105);
    CHECK(basic.size() == 105 + 30);
    CHECK(full.size() == 105 + 20 + 30 + 30 + 20 + 10);
    size_t dual = 0;
    for (const SwatchSpec &s : full)
        dual += !s.inner_word.empty();
    CHECK(dual == 110);
    BoardLayout L;
    CHECK(L.swatches_per_board() == 48);
    CHECK(L.width() == Approx(196.));
    CHECK(board_count(int(full.size()), L) == 5);
}

#include "libslic3r/Model.hpp"

TEST_CASE("ColorMix calibration plate is centered on the bed", "[ColorMix]")
{
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const auto plan = plan_calibration_swatches(b, tools, sched, 4, 10, 2, BoardLayout().swatches_per_board());
    Model model;
    build_calibration_plate(model, plan, b, tools, 5, BoardLayout(), 5, 220.);
    center_on_bed(model, Vec2d(125., 110.));
    BoundingBoxf3 bb;
    for (const ModelObject *o : model.objects)
        for (size_t i = 0; i < o->instances.size(); ++i)
            bb.merge(o->instance_bounding_box(i));
    REQUIRE(bb.defined);
    CHECK(bb.center().x() == Approx(125.).margin(0.01));
    CHECK(bb.center().y() == Approx(110.).margin(0.01));
    CHECK(bb.min.z() == Approx(0.).margin(1e-6));
}

TEST_CASE("ColorMix calibration boards are assembled correctly and fit the bed", "[ColorMix]")
{
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const auto plan = plan_calibration_swatches(b, tools, sched, 4, 10, 2, BoardLayout().swatches_per_board());
    BoardLayout L;
    Model model;
    const double first_layer = 0.25;
    build_calibration_plate(model, plan, b, tools, 8, L, 5, 220., first_layer);
    const int boards = board_count(int(plan.size()), L);
    REQUIRE(boards > 1);
    // One object: all boards on a one-layer sheet.
    REQUIRE(model.objects.size() == 1);
    const ModelObject *o = model.objects.front();
    const BoundingBoxf3 raw = o->raw_mesh_bounding_box();
    CHECK(raw.size().z() == Approx(L.height() + 2.).margin(1e-3)); // + marker dots
    REQUIRE(o->volumes.front()->name.find("Sheet") == 0);
    const BoundingBoxf3 sheet = o->volumes.front()->mesh().transformed_bounding_box(o->volumes.front()->get_matrix());
    CHECK(sheet.min.z() == Approx(raw.min.z()).margin(1e-6));
    CHECK(sheet.size().z() == Approx(first_layer).margin(1e-5));
    CHECK(o->volumes.front()->mesh().stats().number_of_facets > 12); // rounded corners
    CHECK(its_volume(o->volumes.front()->mesh().its) > 0.f); // faces point outward
    // Every board volume stands on its board's slot, inside the sheet; boards are `pitch` apart.
    std::vector<double> front(size_t(boards), 1e9), back(size_t(boards), -1e9);
    for (size_t i = 1; i < o->volumes.size(); ++i) {
        const ModelVolume *v = o->volumes[i];
        int k = 0;
        REQUIRE(std::sscanf(v->name.c_str(), "Board %d:", &k) == 1);
        REQUIRE(k >= 1);
        REQUIRE(k <= boards);
        const BoundingBoxf3 vb = v->mesh().transformed_bounding_box(v->get_matrix());
        INFO(v->name);
        CHECK(vb.size().y() == Approx(L.thickness).margin(1e-3));
        CHECK(vb.min.x() > sheet.min.x() + 1.);
        CHECK(vb.max.x() < sheet.max.x() - 1.);
        CHECK(vb.min.y() > sheet.min.y() + 1.);
        CHECK(vb.max.y() < sheet.max.y() - 1.);
        front[size_t(k - 1)] = std::min(front[size_t(k - 1)], vb.min.y());
        back[size_t(k - 1)]  = std::max(back[size_t(k - 1)], vb.max.y());
    }
    for (int k = 0; k < boards; ++k) {
        CHECK(back[size_t(k)] - front[size_t(k)] == Approx(L.thickness).margin(1e-3));
        CHECK(front[0] - front[size_t(k)] == Approx(k * L.pitch()).margin(1e-3));
    }
    // On a 250 x 220 bed.
    const BoundingBoxf3 bb = o->instance_bounding_box(0);
    CHECK(bb.min.x() >= 0.);
    CHECK(bb.max.x() <= 250.);
    CHECK(bb.min.y() >= 0.);
    CHECK(bb.max.y() <= 220.);
    CHECK(bb.min.z() == Approx(0.).margin(1e-6));
    // No supports (the project's supports would grow between the boards), no brim (the sheet replaces it).
    REQUIRE(o->config.has("support_material"));
    CHECK(!o->config.get().opt_bool("support_material"));
    REQUIRE(o->config.has("brim_type"));
    CHECK(o->config.get().opt_enum<BrimType>("brim_type") == btNoBrim);
    // Every part is named in the object list.
    for (const ModelVolume *v : o->volumes)
        CHECK(!v->name.empty());
    // The sheet, then per board: 4 frame pieces, k marker dots and every cell.
    size_t volumes = 1;
    for (int k = 1; k <= boards; ++k)
        volumes += size_t(4 + k + L.swatches_per_board());
    CHECK(o->volumes.size() == volumes);
    // The first swatch cell carries the swatch's label (the sheet and the frame come first).
    CHECK(o->volumes[6]->name == "Board 1: " + plan.front().label);
}

TEST_CASE("ColorMix core tool: bundle White or an empty slot only", "[ColorMix]")
{
    const Bundle b = builtin_colormix_bundle();
    // C M Y W K in T1..T5, glitters not loaded, 8 tools.
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    CHECK(white_tool(b, tools) == 4);
    CHECK(allowed_core_tools(b, tools, 8) == std::vector<unsigned int>{4, 6, 7, 8});
    CHECK(core_tool_color(b, tools, 4) == b.filaments[size_t(b.filament_index("white"))].color);
    CHECK(core_tool_color(b, tools, 8) == "#FFFFFF");
    // Glitters loaded in T6/T7: they cannot be the core.
    const std::vector<unsigned int> glitter = {1, 2, 3, 4, 5, 6, 7};
    CHECK(allowed_core_tools(b, glitter, 8) == std::vector<unsigned int>{4, 8});
    // White not loaded: only empty slots.
    const std::vector<unsigned int> no_white = {1, 2, 3, 0, 5, 0, 0};
    CHECK(white_tool(b, no_white) == 0);
    CHECK(allowed_core_tools(b, no_white, 5) == std::vector<unsigned int>{4});
    CHECK(allowed_core_tools(b, glitter, 7) == std::vector<unsigned int>{4});
}

TEST_CASE("ColorMix calibration plan fills the last board with validation swatches", "[ColorMix]")
{
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const BoardLayout L;
    const int per = L.swatches_per_board();
    for (int dual = 0; dual <= 2; ++dual) {
        INFO("dual level " << dual);
        const auto base = plan_calibration_swatches(b, tools, sched, 4, 10, dual);
        const auto full = plan_calibration_swatches(b, tools, sched, 4, 10, dual, per);
        CHECK(full.size() % size_t(per) == 0);
        CHECK(board_count(int(full.size()), L) == board_count(int(base.size()), L));
        // Same swatches first, the extras are all validation swatches, no duplicates.
        for (size_t i = 0; i < base.size(); ++i)
            CHECK((full[i].word == base[i].word && full[i].inner_word == base[i].inner_word));
        size_t extra_dual = 0;
        for (size_t i = base.size(); i < full.size(); ++i) {
            CHECK(full[i].held_out);
            extra_dual += !full[i].inner_word.empty();
        }
        if (dual > 0 && full.size() - base.size() >= 2) // filler alternates single- and dual-wall swatches
            CHECK(extra_dual > 0);
        else if (dual == 0)
            CHECK(extra_dual == 0);
        for (size_t i = 0; i < full.size(); ++i)
            for (size_t j = i + 1; j < full.size(); ++j)
                CHECK(!(full[i].word == full[j].word && full[i].inner_word == full[j].inner_word));
    }
}

namespace {
ExPolygon rect_mm(double x0, double y0, double x1, double y1)
{
    return ExPolygon(Polygon({{scaled<coord_t>(x0), scaled<coord_t>(y0)}, {scaled<coord_t>(x1), scaled<coord_t>(y0)},
                              {scaled<coord_t>(x1), scaled<coord_t>(y1)}, {scaled<coord_t>(x0), scaled<coord_t>(y1)}}));
}
double area_mm2(const ExPolygons &e)
{
    double a = 0.;
    for (const ExPolygon &p : e)
        a += p.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}
} // namespace

TEST_CASE("ColorMix region cleanup: slivers, small islands and jagged borders", "[ColorMix]")
{
    const size_t nl = 20;
    std::vector<double>                  print_z(nl);
    std::vector<ExPolygons>              outlines(nl, ExPolygons{rect_mm(0, 0, 20, 20)});
    std::vector<std::vector<ExPolygons>> seg(nl, std::vector<ExPolygons>(4));
    for (size_t L = 0; L < nl; ++L) {
        print_z[L] = 0.2 + 0.1 * double(L);
        ExPolygons others;
        seg[L][1] = {rect_mm(5, 0, 5.2, 5)};                    // 0.2 mm run along the outline: a sliver
        if (L >= 8 && L <= 12)
            seg[L][2] = {rect_mm(10, 0, 10.8, 4)};              // 0.8 mm x 0.5 mm of surface: an island
        seg[L][3] = {rect_mm(12, 0, 18, 6)};                    // a real region
        if (L == 10)
            seg[L][3] = union_ex(ExPolygons{rect_mm(11, 0, 12, 6), rect_mm(12, 0, 18, 6)}); // one-layer bump
        for (size_t s = 1; s < 4; ++s)
            append(others, seg[L][s]);
        seg[L][0] = diff_ex(outlines[L], union_ex(others));
    }
    SliceSettings st;
    st.region_cleanup  = true;
    st.min_run         = 0.3;
    st.min_island_area = 0.5;
    cleanup_regions(seg, outlines, print_z, st, [] {});
    for (size_t L = 0; L < nl; ++L) {
        INFO("layer " << L);
        // The sliver gives up its part near the surface (the rest is hidden inside).
        CHECK(intersection_ex(seg[L][1], ExPolygons{rect_mm(0, 0, 20, 0.5)}).empty());
        CHECK(seg[L][2].empty());
        CHECK(area_mm2(seg[L][3]) == Approx(36.).epsilon(0.01));
        double total = 0.;
        for (const ExPolygons &r : seg[L])
            total += area_mm2(r);
        CHECK(total == Approx(400.).epsilon(0.001));
    }
    // Without cleanup settings nothing small is removed.
    SliceSettings off;
    off.region_cleanup  = true;
    off.smooth_borders  = false;
    off.min_run         = 0.;
    off.min_island_area = 0.;
    std::vector<std::vector<ExPolygons>> seg2(nl, std::vector<ExPolygons>(2));
    for (size_t L = 0; L < nl; ++L) {
        seg2[L][1] = {rect_mm(5, 0, 5.2, 5)};
        seg2[L][0] = diff_ex(outlines[L], seg2[L][1]);
    }
    cleanup_regions(seg2, outlines, print_z, off, [] {});
    CHECK(!seg2[5][1].empty());
}

TEST_CASE("ColorMix top patches follow the slope cutoff", "[ColorMix]")
{
    // A ridge: both x sides slope (`step` mm inward per 0.1 mm layer), the y sides are vertical.
    auto run = [](double step, double band) {
        const size_t nl = 30;
        std::vector<double>                  print_z(nl);
        std::vector<ExPolygons>              outlines(nl);
        std::vector<std::vector<ExPolygons>> seg(nl, std::vector<ExPolygons>(3));
        for (size_t L = 0; L < nl; ++L) {
            print_z[L]  = 0.2 + 0.1 * double(L);
            outlines[L] = {rect_mm(step * double(L), 0, 40 - step * double(L), 40)};
            seg[L][2]   = outlines[L];
        }
        FullSpectrum::VirtualExtruder ve;
        ve.id         = 3;
        ve.sequence   = {1, 2};
        ve.components = {{1, 0.5}, {2, 0.5}};
        SliceSettings st;
        st.top_pattern      = true;
        st.top_layers       = 3;
        st.patch_band_width = band;
        apply_slice_techniques(seg, outlines, print_z, 2, {ve}, st, [] {});
        // Middle layers: does the tool the word doesn't use on that layer appear (patches)?
        int patched = 0;
        for (size_t L = 5; L < 20; ++L)
            patched += !seg[L][(L % 2 == 0) ? 1 : 0].empty();
        return patched;
    };
    CHECK(run(0.5, 0.3) > 10);  // 11 degrees: bands 0.51 mm, wider than 0.3 -> patches
    CHECK(run(0.1, 0.3) == 0);  // 45 degrees: bands 0.14 mm -> layer mixing only
    CHECK(run(0.25, 0.3) == 0); // 22 degrees: bands 0.27 mm -> layer mixing only
    CHECK(run(0.25, 0.15) > 10); // ... unless the cutoff is lowered
}

TEST_CASE("ColorMix converter: fewer tool changes and merged look-alike recipes", "[ColorMix]")
{
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    Predictor p(b, nullptr, sched);
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    std::vector<Target> targets;
    const char *hex[] = {"#3E8C5A", "#7A4E9E", "#C46A3A", "#4FA3B8", "#9E9E3A", "#B85C7A", "#5A6E3E", "#8C8CC4"};
    for (size_t i = 0; i < 8; ++i) {
        Target t{hex_to_lab(hex[i]), 1., hex[i]};
        t.z_ranges = {{0., 12.}};
        if (i % 3 == 0)
            t.z_ranges = {{2., 6.}, {8., 15.}};
        targets.push_back(t);
    }
    ConvertOptions opt;
    opt.schedule     = sched;
    opt.core_tool    = 4;
    opt.change_weight = 0.;
    const ConversionResult plain = convert_colors(targets, b, p, tools, opt);
    REQUIRE(plain.problem.empty());
    REQUIRE(plain.tool_changes > 0);
    CHECK(plain.tool_changes == plain.tool_changes_before);
    CHECK(plain.tool_changes == estimate_tool_changes(targets, plain.candidates, plain.choice, tools, sched, 4));
    // Merged: no two different recipes in use predict colors closer than merge_de.
    std::set<int> used(plain.choice.begin(), plain.choice.end());
    for (int a : used)
        for (int c : used)
            if (a < c)
                CHECK(delta_e_2000(plain.candidates[size_t(a)].prediction.color, plain.candidates[size_t(c)].prediction.color) >= opt.merge_de);

    opt.change_weight = 2.;
    const ConversionResult fewer = convert_colors(targets, b, p, tools, opt);
    INFO("tool changes " << plain.tool_changes << " -> " << fewer.tool_changes << ", mean dE " << plain.weighted_mean_de << " -> "
                         << fewer.weighted_mean_de);
    CHECK(fewer.tool_changes < plain.tool_changes);
    CHECK(fewer.tool_changes == estimate_tool_changes(targets, fewer.candidates, fewer.choice, tools, sched, 4));
    CHECK(fewer.weighted_mean_de < plain.weighted_mean_de + 3.);
    // Rotations keep the color: every choice predicts what its cost says.
    for (size_t k = 0; k < targets.size(); ++k)
        CHECK(fewer.de[k] == Approx(delta_e_2000(targets[k].color, fewer.candidates[size_t(fewer.choice[k])].prediction.color)));
    // Without z ranges there is no estimate.
    for (Target &t : targets)
        t.z_ranges.clear();
    CHECK(convert_colors(targets, b, p, tools, opt).tool_changes == -1);
}

TEST_CASE("ColorMix model colors that are a spool's color print with that spool", "[ColorMix]")
{
    // After calibration a spool's measured color differs from its catalog color; lettering painted in
    // either must still print with the pure spool, not a blend that happens to land closer.
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    Calibration cal;
    cal.id             = "test";
    cal.layer_schedule = {0.1};
    auto pure = [](const char *f, Lab l) {
        Swatch s;
        s.word     = {f};
        s.measured = l;
        return s;
    };
    cal.swatches = {pure("magenta", {49.4, 61.8, 5.3}), pure("yellow", {88.5, -16.1, 87.3}), pure("white", {92.7, 0., 0.2}),
                    pure("black", {30.1, -0.6, 1.3})};
    Bundle cb = b;
    cb.calibrations.push_back(cal);
    Predictor p(cb, &cb.calibrations.back(), sched);
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    ConvertOptions opt;
    opt.schedule = sched;
    // Catalog magenta (about 10 dE from the measured one), and a slightly off measured yellow.
    std::vector<Target> targets = {{hex_to_lab(b.filaments[size_t(b.filament_index("magenta"))].color), 1., "catalog magenta"},
                                   {Lab{88.0, -15.0, 85.5}, 1., "yellow"},
                                   {hex_to_lab("#3A7FB0"), 1., "a blue"}};
    const ConversionResult r = convert_colors(targets, cb, p, tools, opt);
    REQUIRE(r.problem.empty());
    CHECK(r.candidates[size_t(r.choice[0])].pure());
    CHECK(r.candidates[size_t(r.choice[0])].recipe.word.front() == b.filament_index("magenta"));
    CHECK(r.candidates[size_t(r.choice[1])].pure());
    CHECK(r.candidates[size_t(r.choice[1])].recipe.word.front() == b.filament_index("yellow"));
    CHECK_FALSE(r.candidates[size_t(r.choice[2])].pure()); // an ordinary color still gets its mix
    // Off: the closest prediction wins.
    opt.spool_snap_de = 0.;
    const ConversionResult r0 = convert_colors(targets, cb, p, tools, opt);
    CHECK_FALSE(r0.candidates[size_t(r0.choice[0])].pure());
}

TEST_CASE("ColorMix color range chart", "[ColorMix]")
{
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    Predictor p(b, nullptr, sched);
    ConvertOptions opt;
    opt.schedule = sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 6, 7};
    const Lab magenta = p.filament_colors()[size_t(b.filament_index("magenta"))];
    const GamutChart c = make_gamut_chart(b, p, tools, opt, {{magenta, "magenta"}, {hex_to_lab("#808080"), "gray"}}, 24, 9);
    REQUIRE(c.problem.empty());
    REQUIRE(c.cells.size() == size_t(9 * 25));
    for (const GamutCell &cell : c.cells) {
        CHECK(cell.candidate >= 0);
        CHECK(cell.de >= 0.);
    }
    // A spool's own color is printable; grays land in the gray column.
    REQUIRE(c.points.size() == 2);
    CHECK(c.points[0].de < 0.5);
    CHECK(c.points[0].x < 24.);
    CHECK(c.points[1].x > 24.);
    // Screen colors: the lightest row is lighter than the darkest, the first column is red.
    CHECK(c.cell(0, 0).target.L > c.cell(8, 0).target.L);
    CHECK(c.cell(4, 0).target.a > 30.);
    // Nothing loaded: nothing printable.
    const GamutChart none = make_gamut_chart(b, p, std::vector<unsigned int>(7, 0), opt, {}, 24, 9);
    CHECK_FALSE(none.problem.empty());
}

TEST_CASE("ColorMix mixing roles: files, converter and calibration plan", "[ColorMix]")
{
    // Files of earlier builds: "effect" / "mixable" -> Solid; new files keep the role.
    const std::string legacy = R"({"format": "prusaslicer-colormix-bundle", "version": 1, "id": "t", "filaments": [
        {"id": "a", "color": "#FF0000"}, {"id": "b", "color": "#00FF00", "mixable": false},
        {"id": "c", "color": "#0000FF", "effect": true}, {"id": "d", "color": "#FFFFFF", "role": "accent", "mixable": false, "effect": true}]})";
    const std::optional<Bundle> lb = parse_bundle(legacy);
    REQUIRE(lb.has_value());
    CHECK(lb->filaments[0].role == MixRole::Mix);
    CHECK(lb->filaments[1].role == MixRole::Solid);
    CHECK(lb->filaments[2].role == MixRole::Solid);
    CHECK(lb->filaments[3].role == MixRole::Accent);
    const std::optional<Bundle> back = parse_bundle(bundle_to_json(*lb));
    REQUIRE(back.has_value());
    for (size_t i = 0; i < 4; ++i)
        CHECK(back->filaments[i].role == lb->filaments[i].role);

    // The ColorMix glitters are accents: never in a conversion's candidates.
    const Bundle b = builtin_colormix_bundle();
    const int gold = b.filament_index("gold-glitter"), silver = b.filament_index("silver-glitter");
    CHECK(b.filaments[size_t(gold)].role == MixRole::Accent);
    LayerSchedule sched;
    Predictor p(b, nullptr, sched);
    ConvertOptions opt;
    opt.schedule = sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 6, 7};
    const ConversionResult r = convert_colors({}, b, p, tools, opt);
    for (const Candidate &c : r.candidates)
        for (int f : c.recipe.word)
            CHECK((f != gold && f != silver));

    // Plate: a solid swatch per accent, its targeted set, and no other glitter mixes.
    PlanReport rep;
    const std::vector<SwatchSpec> plan = plan_calibration_swatches(b, tools, sched, 4, 10, 2, 48, 255 - 1 - 8, &rep);
    auto has = [&](const Word &w) { return std::any_of(plan.begin(), plan.end(), [&](const SwatchSpec &s) { return s.word == w && s.inner_word.empty(); }); };
    const int white = b.filament_index("white"), black = b.filament_index("black"), cyan = b.filament_index("cyan");
    for (int a : {gold, silver}) {
        CHECK(has({a}));
        for (int m : b.mixable_indices())
            CHECK(has({m, a}));
        CHECK(has({white, white, white, a}));
        CHECK(has({white, a, a, a}));
        CHECK(has({black, black, black, a}));
        CHECK(has({black, a, a, a}));
    }
    CHECK(has({gold, silver}));
    size_t with_glitter = 0;
    for (const SwatchSpec &s : plan) {
        std::set<int> f(s.word.begin(), s.word.end());
        f.insert(s.inner_word.begin(), s.inner_word.end());
        with_glitter += f.count(gold) || f.count(silver) ? 1 : 0;
    }
    CHECK(with_glitter == 2 + 2 * (5 + 4) + 1);
    CHECK(rep.dropped_core == 0);
    CHECK(has({cyan}));

    // All seven mixing: the basic swatches still fit, the dual-wall ones don't.
    Bundle all = b;
    for (BundleFilament &f : all.filaments)
        f.role = MixRole::Mix;
    PlanReport rep2;
    plan_calibration_swatches(all, tools, sched, 4, 10, 2, 48, 255 - 1 - 8, &rep2);
    CHECK(rep2.budget == 246);
    CHECK(rep2.dropped_core == 0);
    CHECK(rep2.dropped_dual > 0);
    // Eight mixing: the basic swatches don't fit - the set can't be calibrated as it is.
    all.filaments.push_back({"orange", "Orange", "#F08020", 8, MixRole::Mix, {}});
    std::vector<unsigned int> tools8 = tools;
    tools8.push_back(8);
    PlanReport rep3;
    plan_calibration_swatches(all, tools8, sched, 4, 10, 2, 48, 255 - 1 - 8, &rep3);
    CHECK(rep3.dropped_core > 0);
}

TEST_CASE("ColorMix strength from TD for uncalibrated filaments", "[ColorMix]")
{
    CHECK(Predictor::td_strength(0.) == Approx(1.));
    CHECK(Predictor::td_strength(Predictor::TD_REF) == Approx(1.));
    CHECK(Predictor::td_strength(0.3) > Predictor::td_strength(5.));   // opaque shows more than translucent
    // Uncalibrated: a translucent filament shows less in a 50/50 mix.
    Bundle b;
    b.id = "td-test";
    BundleFilament red, white;
    red.name = "Red";     red.color = "#C02020";
    white.name = "White"; white.color = "#F0F0F0";
    b.filaments = {red, white};
    LayerSchedule sched;
    const Lab plain = Predictor(b, nullptr, sched).predict(std::vector<double>{0.5, 0.5}).color;
    b.filaments[0].td = 12.;   // translucent red
    b.filaments[1].td = 1.;
    const Lab with_td = Predictor(b, nullptr, sched).predict(std::vector<double>{0.5, 0.5}).color;
    CHECK(with_td.L > plain.L);   // more of the white shows
    CHECK(with_td.a < plain.a);
}

TEST_CASE("ColorMix works without a spool that isn't loaded", "[ColorMix]")
{
    // A spool ran out: it is "not loaded" (tool 0) and no recipe may use it.
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    Predictor p(b, nullptr, sched);
    const int cyan = b.filament_index("cyan");
    std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    tools[size_t(cyan)] = 0;
    ConvertOptions opt;
    opt.schedule   = sched;
    opt.dual_walls = true;
    const std::vector<Target> targets = {{hex_to_lab("#2080D0"), 1., "blue"}, {hex_to_lab("#30A040"), 1., "green"}, {hex_to_lab("#E04040"), 1., "red"}};
    const ConversionResult r = convert_colors(targets, b, p, tools, opt);
    REQUIRE(r.problem.empty());
    REQUIRE_FALSE(r.candidates.empty());
    for (const Candidate &c : r.candidates) {
        CHECK(std::find(c.recipe.word.begin(), c.recipe.word.end(), cyan) == c.recipe.word.end());
        CHECK(std::find(c.recipe.inner_word.begin(), c.recipe.inner_word.end(), cyan) == c.recipe.inner_word.end());
    }
    for (int k : r.choice)
        CHECK(k >= 0);
}

TEST_CASE("ColorMix dual-wall recipe with a one-filament outer wall is a blend", "[ColorMix]")
{
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    Candidate c;
    c.recipe.word          = {4};          // Black outer wall
    c.recipe.inner_word    = {1, 1, 1, 4}; // Magenta 75% + Black 25% inner wall
    c.recipe.weights       = word_weights(c.recipe.word, b.filaments.size(), sched);
    c.recipe.inner_weights = word_weights(c.recipe.inner_word, b.filaments.size(), sched);
    CHECK(!c.pure());
    const FullSpectrum::VirtualExtruder ve = make_virtual_extruder(9, c.recipe, Prediction{}, {1, 2, 3, 4, 5, 0, 0});
    CHECK(ve.sequence == std::vector<unsigned int>{5});
    CHECK(ve.inner_sequence == std::vector<unsigned int>{2, 2, 2, 5});
    const auto norm = FullSpectrum::normalize_virtual_extruders({ve});
    REQUIRE(norm.size() == 1);
    CHECK(norm.front().inner_sequence == std::vector<unsigned int>{2, 2, 2, 5});
    // The converter must not treat such a recipe as a solid filament.
    Predictor p(b, nullptr, sched);
    std::vector<Target> targets = {{hex_to_lab("#5F4D54"), 1., "dark"}};
    ConvertOptions opt;
    opt.dual_walls = true;
    opt.schedule   = sched;
    const ConversionResult res = convert_colors(targets, b, p, {1, 2, 3, 4, 5, 0, 0}, opt);
    REQUIRE(res.choice.front() >= 0);
    const Candidate &chosen = res.candidates[size_t(res.choice.front())];
    if (chosen.recipe.dual())
        CHECK(res.blends_used == 1);
}

// Diagnostic: how much of the color range stock virtual extruders and the ColorMix engine reach,
// all predicted with the same uncalibrated mixing model. Run: libslic3r_tests "[ColorMixGamut]" -s
TEST_CASE("ColorMix gamut: stock virtual extruders vs ColorMix engine", "[.][ColorMixGamut]")
{
    const Bundle b = builtin_colormix_bundle();
    std::vector<int> mix = b.mixable_indices();
    std::vector<Lab> lab;
    for (const BundleFilament &f : b.filaments)
        lab.push_back(hex_to_lab(f.color));
    auto predict = [&](const std::vector<std::pair<int, double>> &w) {
        std::vector<std::pair<Lab, double>> parts;
        for (const auto &[f, s] : w)
            parts.emplace_back(lab[size_t(f)], s);
        return fdm_mixer_predict(parts);
    };
    // Stock: ratios of up to 3 filaments (3.3 % steps), turned into a layer cycle the way
    // FullSpectrum does (quantised to 3 % error, evenly interleaved).
    auto stock_cycle = [](const std::vector<double> &r) {
        std::vector<int> c;
        for (int n = 2; n <= 64; ++n) {
            c.clear();
            for (double x : r)
                c.push_back(std::max(1, int(std::lround(x * n))));
            const int s = std::accumulate(c.begin(), c.end(), 0);
            double err = 0.;
            for (size_t i = 0; i < r.size(); ++i)
                err = std::max(err, std::abs(r[i] - double(c[i]) / s));
            if (err <= 0.03)
                break;
        }
        int g = c[0];
        for (int x : c)
            g = std::gcd(g, x);
        for (int &x : c)
            x /= g;
        const int len = std::accumulate(c.begin(), c.end(), 0);
        std::vector<int> seq, emitted(c.size(), 0);
        for (int slot = 0; slot < len; ++slot) {
            size_t best = 0;
            double bd   = -1e9;
            for (size_t i = 0; i < c.size(); ++i)
                if (const double d = double((slot + 1) * c[i]) / len - emitted[i]; d > bd) {
                    bd   = d;
                    best = i;
                }
            ++emitted[best];
            seq.push_back(int(best));
        }
        return seq;
    };
    std::vector<Lab> stock_any, stock_clean, stock_rules;
    const int N = 30;
    for (size_t i = 0; i < mix.size(); ++i) {
        stock_any.push_back(lab[size_t(mix[i])]);
        stock_clean.push_back(lab[size_t(mix[i])]);
        stock_rules.push_back(lab[size_t(mix[i])]);
        for (size_t j = i + 1; j < mix.size(); ++j)
            for (size_t k = j; k <= mix.size(); ++k) {
                const bool three = k < mix.size() && k != j;
                if (k == j)
                    continue;
                for (int a = 1; a < N; ++a)
                    for (int bb = 1; a + bb <= N; ++bb) {
                        const int c3 = N - a - bb;
                        if (three ? c3 < 1 : c3 != 0)
                            continue;
                        std::vector<double> r = {double(a) / N, double(bb) / N};
                        std::vector<std::pair<int, double>> w = {{mix[i], r[0]}, {mix[j], r[1]}};
                        if (three) {
                            r.push_back(double(c3) / N);
                            w.push_back({mix[k], r[2]});
                        }
                        // The printed color follows the layer cycle, not the requested ratio.
                        const auto seq = stock_cycle(r);
                        Word word(seq.begin(), seq.end());
                        for (auto &[f, share] : w)
                            share = 0.;
                        for (int c : seq)
                            w[size_t(c)].second += 1. / double(seq.size());
                        const Lab p = predict(w);
                        stock_any.push_back(p);
                        if (word.size() <= 6 && circular_max_run(word) <= 3)
                            stock_clean.push_back(p);
                        if (satisfies_rules(word, b.rules))
                            stock_rules.push_back(p);
                    }
                if (!three)
                    break;
            }
    }
    // ColorMix: words under the bundle's striping rules, single and dual walls, uniform and
    // variable layer heights.
    auto ours = [&](const LayerSchedule &sched, bool dual) {
        Predictor p(b, nullptr, sched);
        std::vector<Lab> out;
        const std::vector<Recipe> singles = enumerate_recipes(mix, b.filaments.size(), b.rules, sched, p.filament_colors());
        for (const Recipe &r : singles)
            out.push_back(p.predict(r).color);
        if (dual) {
            RecipeRules in_rules       = b.rules;
            in_rules.max_run           = b.rules.max_run + 1;
            in_rules.recurrence_window = b.rules.recurrence_window + 1;
            in_rules.max_word_length   = std::min(4, b.rules.max_word_length);
            in_rules.max_filaments     = 2;
            const std::vector<Recipe> inners = enumerate_recipes(mix, b.filaments.size(), in_rules, sched, p.filament_colors());
            for (const Recipe &o : singles)
                for (const Recipe &in : inners) {
                    if (in.weights == o.weights)
                        continue;
                    Recipe r        = o;
                    r.inner_word    = in.word;
                    r.inner_weights = in.weights;
                    out.push_back(p.predict(r).color);
                }
        }
        return out;
    };
    LayerSchedule uni;
    LayerSchedule var;
    var.heights = {0.12, 0.10, 0.08};
    const std::vector<Lab> ours_single = ours(uni, false), ours_var = ours(var, false), ours_dual = ours(uni, true),
                           ours_both   = ours(var, true);

    // Targets: an sRGB grid (12 steps per channel).
    std::vector<Lab> grid;
    for (int r = 0; r < 12; ++r)
        for (int g = 0; g < 12; ++g)
            for (int bl = 0; bl < 12; ++bl) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", r * 255 / 11, g * 255 / 11, bl * 255 / 11);
                grid.push_back(hex_to_lab(hex));
            }
    auto distinct = [](std::vector<Lab> v, double spacing) {
        std::vector<Lab> kept;
        for (const Lab &c : v)
            if (std::none_of(kept.begin(), kept.end(), [&](const Lab &k) { return delta_e_2000(k, c) < spacing; }))
                kept.push_back(c);
        return kept;
    };
    auto report = [&](const char *name, const std::vector<Lab> &set) {
        const std::vector<Lab> d = distinct(set, 1.0);
        int within3 = 0, within5 = 0, within10 = 0;
        double sum = 0.;
        for (const Lab &t : grid) {
            double best = 1e9;
            for (const Lab &c : d)
                best = std::min(best, delta_e_2000(t, c));
            within3 += best <= 3.;
            within5 += best <= 5.;
            within10 += best <= 10.;
            sum += best;
        }
        std::printf("%-44s %6zu recipes  %5zu colors >=1 dE apart  %5zu >=2 dE apart | sRGB grid within 3/5/10 dE: %5.1f%% %5.1f%% %5.1f%%  mean %.1f\n",
                    name, set.size(), d.size(), distinct(d, 2.0).size(), 100. * within3 / grid.size(), 100. * within5 / grid.size(),
                    100. * within10 / grid.size(), sum / grid.size());
    };
    report("stock FullSpectrum, any ratio (striped too)", stock_any);
    report("stock FullSpectrum, cycles <= 6, run <= 3", stock_clean);
    report("stock FullSpectrum, cycles within ColorMix rules", stock_rules);
    report("ColorMix, single wall", ours_single);
    report("ColorMix, single wall + variable layers", ours_var);
    report("ColorMix, dual walls", ours_dual);
    report("ColorMix, dual walls + variable layers", ours_both);
    CHECK(!ours_both.empty());
}

TEST_CASE("ColorMix region cleanup keeps details seen only from above", "[ColorMix]")
{
    // A 4 x 4 mm logo painted on a flat top (the last 3 layers), not touching the side walls.
    const size_t nl = 10;
    std::vector<double>                  print_z(nl);
    std::vector<ExPolygons>              outlines(nl, ExPolygons{rect_mm(0, 0, 20, 20)});
    std::vector<std::vector<ExPolygons>> seg(nl, std::vector<ExPolygons>(2));
    for (size_t L = 0; L < nl; ++L) {
        print_z[L] = 0.2 + 0.1 * double(L);
        if (L >= 7)
            seg[L][1] = {rect_mm(8, 8, 12, 12)};
        seg[L][0] = diff_ex(outlines[L], seg[L][1]);
    }
    SliceSettings st;
    st.region_cleanup  = true;
    st.min_run         = 0.3;
    st.min_island_area = 0.5;
    cleanup_regions(seg, outlines, print_z, st, [] {});
    CHECK(area_mm2(seg[9][1]) == Approx(16.).epsilon(0.01));
}

TEST_CASE("ColorMix white core leaves flat tops and bottoms in their color", "[ColorMix]")
{
    // A 20 mm square block, 30 layers, painted entirely with physical extruder 1; core = 2.
    const size_t nl = 30;
    std::vector<double>                  print_z(nl);
    std::vector<ExPolygons>              outlines(nl, ExPolygons{rect_mm(0, 0, 20, 20)});
    std::vector<std::vector<ExPolygons>> seg(nl, std::vector<ExPolygons>(2));
    for (size_t L = 0; L < nl; ++L) {
        print_z[L] = 0.2 + 0.1 * double(L);
        seg[L][0]  = outlines[L];
    }
    SliceSettings st;
    st.core_extruder = 2;
    st.wall_depth    = 0.9;
    apply_slice_techniques(seg, outlines, print_z, 2, {}, st, [] {}, /*top shells*/ 5, /*bottom shells*/ 4);
    for (size_t L = 0; L < nl; ++L) {
        INFO("layer " << L);
        const bool shell = L + 5 >= nl || L < 4;
        if (shell)
            CHECK(seg[L][1].empty());
        else
            CHECK(area_mm2(seg[L][1]) == Approx(18.2 * 18.2).epsilon(0.02));
    }
}

TEST_CASE("ColorMix calibration plan stays within the painting states", "[ColorMix]")
{
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    sched.heights = {0.08, 0.12, 0.16};
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const BoardLayout L;
    for (int budget : {247, 120}) {
        const auto plan = plan_calibration_swatches(b, tools, sched, 4, 10, 2, L.swatches_per_board(), budget);
        const long blended = std::count_if(plan.begin(), plan.end(), [](const SwatchSpec &s) { return s.word.size() > 1 || !s.inner_word.empty(); });
        INFO("budget " << budget << ", plan " << plan.size());
        CHECK(blended <= budget);
        // Fitted swatches are kept before validation ones.
        CHECK(std::count_if(plan.begin(), plan.end(), [](const SwatchSpec &s) { return !s.held_out; }) > 0);
        Model model;
        build_calibration_plate(model, plan, b, tools, 8, L, 5, 220.);
        CHECK(model.virtual_extruders.size() == size_t(blended));
    }
}

TEST_CASE("ColorMix photo reading flags cells outside the photo", "[ColorMix]")
{
    // A uniform grey board on white paper, with its right end cut off by the image border.
    BoardLayout  L;
    const double sc = 4., ox = 60., oy = 60., paper = 0.88;
    const int    Wpx = int(ox + sc * (L.width() - 40.)), Hpx = int(2 * oy + sc * L.height());
    PhotoImage img;
    img.width  = Wpx;
    img.height = Hpx;
    img.rgb.resize(size_t(Wpx) * Hpx * 3);
    for (int v = 0; v < Hpx; ++v)
        for (int u = 0; u < Wpx; ++u) {
            const double x = (u + 0.5 - ox) / sc, y = L.height() - (v + 0.5 - oy) / sc;
            double refl = paper;
            if (x >= 0 && x <= L.width() && y >= 0 && y <= L.height()) {
                refl = 0.03;
                for (int r = 0; r < L.rows; ++r)
                    for (int c = 0; c < L.cols; ++c) {
                        const auto rc = L.cell_rect(r, c);
                        if (x >= rc[0] && x <= rc[2] && y >= rc[1] && y <= rc[3])
                            refl = 0.3;
                    }
            }
            const unsigned char b = (unsigned char) std::lround(linear_to_srgb8(refl * 0.9));
            img.rgb[(size_t(v) * Wpx + u) * 3 + 0] = img.rgb[(size_t(v) * Wpx + u) * 3 + 1] = img.rgb[(size_t(v) * Wpx + u) * 3 + 2] = b;
        }
    auto px = [&](double x, double y) { return std::array<double, 2>{ox + sc * x, oy + sc * (L.height() - y)}; };
    const BoardCorners corners = {px(0., L.height()), px(L.width(), L.height()), px(L.width(), 0.), px(0., 0.)};
    const PhotoReading pr = read_board_photo(img, corners, L);
    REQUIRE(pr.valid.size() == pr.cells.size());
    const auto cells = L.swatch_cells();
    int invalid = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto r = L.cell_rect(cells[i][0], cells[i][1]);
        if (!pr.valid[i]) {
            ++invalid;
            CHECK(r[0] > L.width() - 45.); // only cells near the cut-off end
        } else if (r[2] < L.width() - 45.)
            CHECK(delta_e_2000(pr.cells[i], linear_to_lab({0.3, 0.3, 0.3})) < 2.);
    }
    CHECK(invalid > 0);
    CHECK(pr.warning.find("outside the image") != std::string::npos);
}

TEST_CASE("ColorMix photo reading: scanner lid in an oversized paper hole, board shadow, clipped paper", "[ColorMix]")
{
    // Seen on real scans and photos: the hole cut into the paper was ~17 mm
    // wider than the board, so the lid (held 4 mm up by the board) showed as a gray band around it;
    // the paper itself was clipped at 255; in photos the board shaded the paper next to it.
    BoardLayout  L;
    const double sc = 5., ox = 260., oy = 260.;
    const int    Wpx = int(2 * ox + sc * L.width()), Hpx = int(2 * oy + sc * L.height());
    const auto   cells = L.swatch_cells();
    std::vector<Lab> truth;
    for (size_t i = 0; i < cells.size(); ++i)
        truth.push_back(linear_to_lab({0.03 + 0.8 * ((i * 7) % 11) / 10., 0.03 + 0.8 * ((i * 5) % 13) / 12., 0.03 + 0.8 * ((i * 3) % 7) / 6.}));
    // `gap` mm of `gap_refl` around the board, then paper; `shadow`: paper darkened near the board;
    // `gain`: exposure (paper * gain > 1 clips).
    auto render = [&](double gap, double gap_refl, double shadow, double gain) {
        PhotoImage img;
        img.width  = Wpx;
        img.height = Hpx;
        img.rgb.resize(size_t(Wpx) * Hpx * 3);
        for (int v = 0; v < Hpx; ++v)
            for (int u = 0; u < Wpx; ++u) {
                const double x = (u + 0.5 - ox) / sc, y = L.height() - (v + 0.5 - oy) / sc;
                const double dx = std::max({0., -x, x - L.width()}), dy = std::max({0., -y, y - L.height()});
                const double d  = std::max(dx, dy);
                double       f  = 0.88;
                if (d > 0. && d < gap)
                    f = gap_refl;
                else if (d > 0.)
                    f = 0.88 * (1. - shadow * std::exp(-d / 3.));
                LinRGB refl{f, f, f};
                if (d == 0.) {
                    refl = {0.03, 0.03, 0.03};
                    for (size_t k = 0; k < cells.size(); ++k) {
                        const auto rc = L.cell_rect(cells[k][0], cells[k][1]);
                        if (x >= rc[0] && x <= rc[2] && y >= rc[1] && y <= rc[3])
                            refl = lab_to_linear(truth[k]);
                    }
                }
                for (int ch = 0; ch < 3; ++ch)
                    img.rgb[(size_t(v) * Wpx + u) * 3 + ch] = (unsigned char) std::lround(linear_to_srgb8(std::clamp(refl[ch] * gain, 0., 1.)));
            }
        return img;
    };
    auto px = [&](double x, double y) { return std::array<double, 2>{ox + sc * x, oy + sc * (L.height() - y)}; };
    const BoardCorners front = {px(0., L.height()), px(L.width(), L.height()), px(L.width(), 0.), px(0., 0.)};
    auto worst_de = [&](const PhotoReading &pr) {
        double w = 0.;
        for (size_t i = 0; i < truth.size(); ++i)
            w = std::max(w, delta_e_2000(truth[i], pr.cells[i]));
        return w;
    };
    PhotoReadOptions scanner;
    scanner.scanner = true;
    SECTION("scanner: lid seen through a 17 mm gap") {
        const PhotoReading pr = read_board_photo(render(17., 0.62, 0., 1.0), front, L, scanner);
        INFO("worst dE " << worst_de(pr) << " warning: " << pr.warning);
        CHECK(worst_de(pr) < 2.0);
        CHECK(pr.paper_gap >= 16.);
        CHECK(pr.warning.find("hole cut to fit") != std::string::npos);
    }
    SECTION("scanner: paper touching the board") {
        const PhotoReading pr = read_board_photo(render(0., 0., 0., 1.0), front, L, scanner);
        INFO("worst dE " << worst_de(pr) << " warning: " << pr.warning);
        CHECK(worst_de(pr) < 2.0);
        CHECK(pr.warning.empty());
    }
    SECTION("scanner: clipped paper is reported") {
        const PhotoReading pr = read_board_photo(render(0., 0., 0., 1.25), front, L, scanner);
        CHECK(pr.paper_clipped > 0.9);
        CHECK(pr.warning.find("pure white") != std::string::npos);
    }
    SECTION("scanner: only the lid around the board is refused") {
        CHECK_THROWS(read_board_photo(render(60., 0.45, 0., 1.0), front, L, scanner));
    }
    SECTION("camera: the board shades the paper next to it") {
        const PhotoReading pr = read_board_photo(render(0., 0., 0.3, 0.45), front, L);
        INFO("worst dE " << worst_de(pr) << " warning: " << pr.warning);
        CHECK(worst_de(pr) < 2.0);
    }
}

TEST_CASE("ColorMix calibration check finds a filament that didn't print", "[ColorMix]")
{
    // A real plate whose Cyan tool printed nothing: so every swatch with Cyan showed the mix
    // of its other filaments (pure Cyan showed the white core).
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const auto plan = plan_calibration_swatches(b, tools, sched, 4, 10, 2, BoardLayout().swatches_per_board());
    const int  cyan = b.filament_index("cyan"), white = b.filament_index("white");
    Predictor  nominal(b, nullptr, sched);
    std::vector<Lab>  measured(plan.size());
    std::vector<bool> has(plan.size(), true);
    auto predict = [&](Word w, Word in) {
        Recipe r;
        r.word    = w;
        r.weights = word_weights(w, b.filaments.size(), sched);
        if (!in.empty()) {
            r.inner_word    = in;
            r.inner_weights = word_weights(in, b.filaments.size(), sched);
        }
        return nominal.predict(r).color;
    };
    size_t with_cyan = 0;
    for (size_t i = 0; i < plan.size(); ++i) {
        Word w = plan[i].word, in = plan[i].inner_word;
        const bool c = std::count(w.begin(), w.end(), cyan) + std::count(in.begin(), in.end(), cyan) > 0;
        with_cyan += c ? 1 : 0;
        w.erase(std::remove(w.begin(), w.end(), cyan), w.end());
        in.erase(std::remove(in.begin(), in.end(), cyan), in.end());
        if (w.empty())
            w = in.empty() ? Word{white} : in;
        if (in.empty() && !plan[i].inner_word.empty())
            in = w;
        // The other swatches: their prediction plus a few dE (an uncalibrated mixer is never exact).
        Lab l = predict(w, in == w ? Word{} : in);
        l.L += double(int(i % 7) - 3);
        measured[i] = l;
    }
    const auto checks = check_filaments(b, plan, measured, has, sched);
    REQUIRE(checks.size() == 1);
    CHECK(checks.front().filament == cyan);
    CHECK(checks.front().swatches.size() == with_cyan);
    CHECK(checks.front().median_with > 15.);
    // A good plate: nothing flagged.
    for (size_t i = 0; i < plan.size(); ++i)
        measured[i] = predict(plan[i].word, plan[i].inner_word);
    CHECK(check_filaments(b, plan, measured, has, sched).empty());
}

TEST_CASE("ColorMix bundle save replaces the file safely", "[ColorMix]")
{
    Bundle b = builtin_colormix_bundle();
    const std::string path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("colormix_bundle_%%%%%%.json")).string();
    REQUIRE(save_bundle(b, path));
    b.name = "second save";
    REQUIRE(save_bundle(b, path)); // over an existing file
    CHECK(!boost::filesystem::exists(path + ".tmp"));
    std::string err;
    const auto loaded = load_bundle(path, &err);
    boost::filesystem::remove(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->name == "second save");
}

TEST_CASE("ColorMix region cleanup keeps details on the face on the bed", "[ColorMix]")
{
    // A coaster printed face-down: text painted on the bottom face (the first 3 layers).
    const size_t nl = 10;
    std::vector<double>                  print_z(nl);
    std::vector<ExPolygons>              outlines(nl, ExPolygons{rect_mm(0, 0, 20, 20)});
    std::vector<std::vector<ExPolygons>> seg(nl, std::vector<ExPolygons>(2));
    for (size_t L = 0; L < nl; ++L) {
        print_z[L] = 0.2 + 0.1 * double(L);
        if (L < 3)
            seg[L][1] = {rect_mm(8, 8, 12, 12)};
        seg[L][0] = diff_ex(outlines[L], seg[L][1]);
    }
    SliceSettings st;
    st.region_cleanup  = true;
    st.min_run         = 0.3;
    st.min_island_area = 1.0;
    cleanup_regions(seg, outlines, print_z, st, [] {});
    CHECK(area_mm2(seg[0][1]) == Approx(16.).epsilon(0.01));
}

TEST_CASE("ColorMix slope rule leaves the top rim of vertical walls in the layer word", "[ColorMix]")
{
    // A block with vertical walls and a flat top; a blend, top patches with the slope rule.
    const size_t nl = 20;
    std::vector<double>                  print_z(nl);
    std::vector<ExPolygons>              outlines(nl, ExPolygons{rect_mm(0, 0, 30, 30)});
    std::vector<std::vector<ExPolygons>> seg(nl, std::vector<ExPolygons>(3));
    for (size_t L = 0; L < nl; ++L) {
        print_z[L] = 0.2 + 0.1 * double(L);
        seg[L][2]  = outlines[L];
    }
    FullSpectrum::VirtualExtruder ve;
    ve.id         = 3;
    ve.sequence   = {1, 2};
    ve.components = {{1, 0.5}, {2, 0.5}};
    SliceSettings st;
    st.top_pattern      = true;
    st.top_layers       = 3;
    st.patch_band_width = 0.3;
    apply_slice_techniques(seg, outlines, print_z, 2, {ve}, st, [] {});
    const ExPolygons ring = diff_ex(outlines[0], offset_ex(outlines[0], -scaled<float>(0.4)));
    for (size_t L = nl - 3; L < nl; ++L) {
        INFO("layer " << L);
        const size_t word_slot  = ve.sequence[L % 2] - 1;
        const size_t other_slot = 1 - word_slot;
        CHECK(intersection_ex(seg[L][other_slot], ring).empty()); // the rim: one tool
        CHECK(!seg[L][other_slot].empty());                       // the top itself: patches
    }
}

TEST_CASE("ColorMix dual walls: flat tops and bottoms show the outer word", "[ColorMix]")
{
    const size_t nl = 20;
    std::vector<double>                  print_z(nl);
    std::vector<ExPolygons>              outlines(nl, ExPolygons{rect_mm(0, 0, 20, 20)});
    std::vector<std::vector<ExPolygons>> seg(nl, std::vector<ExPolygons>(3));
    for (size_t L = 0; L < nl; ++L) {
        print_z[L] = 0.2 + 0.1 * double(L);
        seg[L][2]  = outlines[L];
    }
    FullSpectrum::VirtualExtruder ve;
    ve.id             = 3;
    ve.sequence       = {1};
    ve.inner_sequence = {2};
    ve.components     = {{1, 0.5}, {2, 0.5}};
    SliceSettings st;
    st.dual_wall_width = 0.45;
    apply_slice_techniques(seg, outlines, print_z, 2, {ve}, st, [] {}, /*top shells*/ 3, /*bottom shells*/ 3);
    for (size_t L : {size_t(0), size_t(2), nl - 3, nl - 1}) {
        INFO("shell layer " << L);
        CHECK(seg[L][1].empty()); // no inner word in the top / bottom shells
    }
    CHECK(!seg[10][1].empty()); // behind the side walls it is there
}

TEST_CASE("ColorMix border smoothing keeps details two layers tall", "[ColorMix]")
{
    // A 2-layer stripe of slot 1 (e.g. a text stroke) on layers 10-11 inside slot 0.
    const size_t nl = 20;
    std::vector<double>                  print_z(nl);
    std::vector<ExPolygons>              outlines(nl, ExPolygons{rect_mm(0, 0, 20, 20)});
    std::vector<std::vector<ExPolygons>> seg(nl, std::vector<ExPolygons>(2));
    for (size_t L = 0; L < nl; ++L) {
        print_z[L] = 0.2 + 0.1 * double(L);
        if (L == 10 || L == 11)
            seg[L][1] = {rect_mm(0, 0, 8, 6)};
        seg[L][0] = diff_ex(outlines[L], seg[L][1]);
    }
    SliceSettings st;
    st.region_cleanup  = true;
    st.min_run         = 0.;
    st.min_island_area = 0.;
    cleanup_regions(seg, outlines, print_z, st, [] {});
    CHECK(area_mm2(seg[10][1]) == Approx(48.).epsilon(0.01));
    CHECK(area_mm2(seg[11][1]) == Approx(48.).epsilon(0.01));
}

TEST_CASE("ColorMix measurement CSV: numbered lines go to their swatch, misaligned files are refused", "[ColorMix]")
{
    SECTION("numbered, with a header, a gap and a decimal-comma line") {
        const auto csv = parse_measurement_csv("tile;L;a;b\n1;62.1;-21.4;-30.2\n3;50,5;10,0;-5,25\nS4;#FF0000\n", 5);
        REQUIRE(csv.error.empty());
        CHECK(csv.loaded == 3);
        CHECK(csv.values[0] == "62.10 -21.40 -30.20");
        CHECK(csv.values[1].empty());
        CHECK(csv.values[2] == "50.50 10.00 -5.25");
        CHECK(csv.values[3] == "#FF0000");
    }
    SECTION("numbered with an unreadable line: the others still land right") {
        const auto csv = parse_measurement_csv("1;62.1;-21.4;-30.2\n2;---\n3;40;1;2\n", 3);
        REQUIRE(csv.error.empty());
        CHECK(csv.values[2] == "40.00 1.00 2.00");
        CHECK(csv.bad_lines == std::vector<int>{2});
    }
    SECTION("unnumbered: exactly one line per swatch") {
        CHECK(parse_measurement_csv("62.1 -21.4 -30.2\n40 1 2\n", 2).error.empty());
        CHECK(!parse_measurement_csv("62.1 -21.4 -30.2\n40 1 2\n", 3).error.empty());          // too few
        CHECK(!parse_measurement_csv("62.1 -21.4 -30.2\n---\n40 1 2\n", 2).error.empty());     // would shift
        CHECK(!parse_measurement_csv("1;62.1;-21.4;-30.2\n40 1 2\n", 2).error.empty());         // mixed
    }
    SECTION("single measurements") {
        Lab l;
        CHECK(parse_lab_measurement("62,1 -21,4 -30,2", l));
        CHECK(l.a == Approx(-21.4));
        CHECK(!parse_lab_measurement("62.1,-21.4", l));
        CHECK(!parse_lab_measurement("62,1,-21,4,-30,2", l));
    }
}

TEST_CASE("ColorMix saving under an existing calibration's name completes it", "[ColorMix]")
{
    Calibration old;
    old.id             = "plate 1";
    old.layer_schedule = {0.1};
    auto sw = [](std::vector<std::string> w, std::vector<std::string> in, double L) {
        Swatch s;
        s.word       = std::move(w);
        s.inner_word = std::move(in);
        s.measured   = {L, 0., 0.};
        return s;
    };
    old.swatches = {sw({"magenta"}, {}, 50.), sw({"white"}, {}, 93.), sw({"magenta"}, {"white"}, 57.)};
    Calibration cyan = old;
    cyan.swatches = {sw({"cyan"}, {}, 60.), sw({"white"}, {}, 92.)}; // the reprint: cyan, and white measured again
    cyan.plate_file = "reprint.json";
    int kept = 0;
    REQUIRE(merge_calibration(old, cyan, &kept));
    CHECK(kept == 2);
    CHECK(cyan.swatches.size() == 4);
    // The new measurement wins.
    for (const Swatch &s : cyan.swatches)
        if (s.word == std::vector<std::string>{"white"} && s.inner_word.empty())
            CHECK(s.measured.L == Approx(92.));
    // Other layer heights: a different print, nothing is merged.
    Calibration other = cyan;
    other.layer_schedule = {0.12, 0.1, 0.08};
    const size_t n = other.swatches.size();
    CHECK_FALSE(merge_calibration(old, other));
    CHECK(other.swatches.size() == n);
    // The calibration remembers its plate file.
    Bundle b = builtin_colormix_bundle();
    b.calibrations.push_back(cyan);
    const auto back = parse_bundle(bundle_to_json(b));
    REQUIRE(back.has_value());
    CHECK(back->calibrations.back().plate_file == "reprint.json");
}

TEST_CASE("ColorMix new filament sets: ids, checks, save and load", "[ColorMix]")
{
    CHECK(make_id("Matte Sky Blue", {}) == "matte-sky-blue");
    CHECK(make_id("  Silk  Gold!! ", {}) == "silk-gold");
    CHECK(make_id("Sky Blue", {"sky-blue"}) == "sky-blue-2");
    CHECK(make_id("Sky Blue", {"sky-blue", "sky-blue-2"}) == "sky-blue-3");
    CHECK(make_id("???", {}, "set") == "set");

    Bundle b;
    b.name = "My PLA";
    b.id   = make_id(b.name, {builtin_colormix_bundle().id}, "set");
    CHECK(check_bundle(b) != ""); // no filaments
    b.filaments = {{"white", "Matte White", "#F0F0F0", 1, MixRole::Mix, {}},
                   {"sky-blue", "Sky Blue", "#3A9AD9", 2, MixRole::Mix, {}},
                   {"gold", "Silk Gold", "#C8A040", 3, MixRole::Solid, {}}};
    CHECK(check_bundle(b).empty());
    b.filaments[1].role = MixRole::Accent;
    CHECK(check_bundle(b).find("set to \"Mix\"") != std::string::npos);
    b.filaments[1].role = MixRole::Mix;
    b.filaments[1].color   = "blue";
    CHECK(check_bundle(b).find("color") != std::string::npos);
    b.filaments[1].color = "#3A9AD9";
    b.filaments[2].name  = "Sky Blue";
    CHECK(check_bundle(b).find("called") != std::string::npos);
    b.filaments[2].name = "Silk Gold";

    const std::string path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("colormax_set_%%%%%%.json")).string();
    std::string err;
    REQUIRE(save_bundle(b, path, &err));
    const auto back = load_bundle(path, &err);
    REQUIRE(back.has_value());
    CHECK(back->name == "My PLA");
    CHECK(back->filaments.size() == 3);
    CHECK(back->filament_index("white") == 0);
    CHECK(back->mixable_indices().size() == 2);
    boost::filesystem::remove(path);
}

TEST_CASE("ColorMix filament database: parse, merge, search, store", "[ColorMix]")
{
    // The shapes of the real responses, trimmed.
    const std::string fc = R"({"count": 2, "next": "https://filamentcolors.xyz/api/swatch/?format=json&page=2&page_size=100", "results": [
        {"id": 1, "slug": "american-filament-blizzard-white-pctg", "hex_color": "E9EDED", "lab_l": 93.56, "lab_a": -1.18, "lab_b": -1.16,
         "td": 3.0, "color_name": "Blizzard White", "manufacturer": {"id": 7, "name": "American Filament"},
         "filament_type": {"id": 3, "name": "PCTG", "parent_type": {"name": "PETG"}}},
        {"id": 2, "slug": "x-silk-gold", "hex_color": "C8A040", "lab_l": null, "lab_a": null, "lab_b": null, "td": null,
         "color_name": "Gold", "manufacturer": {"name": "Brand X"}, "filament_type": {"name": "Silk PLA", "parent_type": {"name": "PLA"}}},
        {"id": 3, "hex_color": "2D80A6", "lab_l": 50.45, "lab_a": -11.92, "lab_b": -27.85, "td": 4.5,
         "color_name": "Transparent Blue", "manufacturer": {"name": "SnoLabs"}, "filament_type": {"name": "PLA"}}]})";
    std::string next;
    const auto measured = parse_filamentcolors(fc, &next);
    REQUIRE(measured.size() == 3);
    CHECK(next.find("page=2") != std::string::npos);
    CHECK(measured[0].measured);
    CHECK(measured[0].td == Approx(3.));
    CHECK(delta_e_2000(hex_to_lab(measured[0].color), Lab{93.56, -1.18, -1.16}) < 0.5); // from the measured Lab
    CHECK(measured[1].color == "#C8A040");
    CHECK(measured[1].effect);       // silk
    CHECK(measured[2].translucent);  // "Transparent"

    const std::string sp = R"([
        {"id": "a_1000", "manufacturer": "SnoLabs", "name": "Transparent Blue", "material": "PLA", "color_hex": "3366CC", "translucent": true},
        {"id": "b_1000", "manufacturer": "Polymaker", "name": "Galaxy Black", "material": "PLA", "color_hex": "1A1A1A", "pattern": "sparkle"},
        {"id": "b_750", "manufacturer": "Polymaker", "name": "Galaxy Black", "material": "PLA", "color_hex": "1A1A1A", "pattern": "sparkle"},
        {"id": "c_1000", "manufacturer": "Elegoo", "name": "Red", "material": "PLA", "color_hex": "CC2222", "transmission_distance": 1.8},
        {"id": "d_1000", "manufacturer": "Rainbow", "name": "Dual", "material": "PLA", "color_hex": null, "color_hexes": ["FF0000", "0000FF"]}])";
    const auto catalog = parse_spoolmandb(sp);
    REQUIRE(catalog.size() == 4); // the 750 g spool of Galaxy Black is the same filament
    CHECK(catalog[1].effect);
    CHECK(catalog[2].td == Approx(1.8));
    CHECK(catalog[3].effect); // multicolor
    CHECK_FALSE(catalog[0].measured);

    // The measured SnoLabs Transparent Blue replaces the catalog one.
    const auto all = merge_filament_records(measured, catalog);
    CHECK(all.size() == 3 + 3);
    size_t blues = 0;
    for (const auto &f : all)
        blues += f.name == "Transparent Blue" ? 1 : 0;
    CHECK(blues == 1);

    auto hits = search_filaments(all, "transparent blue");
    REQUIRE(hits.size() == 1);
    CHECK(all[hits.front()].source == "filamentcolors.xyz");
    hits = search_filaments(all, "pla");
    REQUIRE(hits.size() >= 2);
    CHECK(all[hits.front()].measured); // measured first
    CHECK(search_filaments(all, "nothing like this").empty());
    // Words match the start of words: "red" finds the red filament, not every "measured" one.
    hits = search_filaments(all, "red");
    REQUIRE(hits.size() == 1);
    CHECK(all[hits.front()].name == "Red");

    std::string date;
    const auto back = filament_db_from_json(filament_db_to_json(all, "2026-11-13"), &date);
    CHECK(date == "2026-11-13");
    REQUIRE(back.size() == all.size());
    CHECK(back[0].td == Approx(3.));
    CHECK(back[0].measured);

    // Filament sets keep a filament's TD.
    Bundle b = builtin_colormix_bundle();
    b.filaments[0].td = 2.4;
    const auto rb = parse_bundle(bundle_to_json(b));
    REQUIRE(rb.has_value());
    CHECK(rb->filaments[0].td == Approx(2.4));
}

TEST_CASE("ColorMix calibrations record how their swatches were printed", "[ColorMix]")
{
    Bundle b = builtin_colormix_bundle();
    Calibration c;
    c.id              = "test";
    c.white_core      = true;
    c.wall_depth      = 0.9;
    c.dual_wall_width = 0.45;
    c.layer_schedule  = {0.12, 0.1, 0.08};
    b.calibrations.push_back(c);
    const auto back = parse_bundle(bundle_to_json(b));
    REQUIRE(back.has_value());
    REQUIRE(!back->calibrations.empty());
    CHECK(back->calibrations.back().white_core);
    CHECK(back->calibrations.back().wall_depth == Approx(0.9));
    CHECK(back->calibrations.back().dual_wall_width == Approx(0.45));
}

#include <fstream>
#include <sstream>

// Diagnostic, not part of the suite: reads real calibration photos and fits a calibration.
//   COLORMIX_DIAG_DIR   output folder (plan.tsv, photo readings, fit.tsv)
//   COLORMIX_PHOTO_JOBS text file, one job per line:
//                       <ppm (P6) path> <scanner 0/1> <x0 y0 x1 y1 x2 y2 x3 y3> <board 1..n> <output name>
//   COLORMIX_FIT_IN     TSV "<swatch 1-based> L a b" (measured colors) -> fit.tsv + fit report
// Plan: Prusament ColorMix bundle, tools 1..5, 0.1 mm, longest word 4, 10 held out, full dual walls
// (the calibration dialog's defaults).
TEST_CASE("ColorMix photo diagnostics", "[.][ColorMixPhotoDiag]")
{
    const char *dir_env = std::getenv("COLORMIX_DIAG_DIR");
    REQUIRE(dir_env != nullptr);
    const std::string dir = dir_env;
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    sched.first_layer_height = 0.2;
    sched.heights            = {0.1};
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    BoardLayout L;
    const auto plan = plan_calibration_swatches(b, tools, sched, 4, 10, 2, L.swatches_per_board(), int(TRIANGLE_STATE_TYPE_COUNT) - 1 - 8);
    // COLORMIX_TIFFS: TIFF files to decode into <file>.ppm (JPEG streams are dumped to <file>.jpgstream).
    if (const char *tiffs = std::getenv("COLORMIX_TIFFS")) {
        std::istringstream ls(tiffs);
        std::string        path;
        while (std::getline(ls, path, ';')) {
            std::ifstream f(path, std::ios::binary);
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            PhotoImage  img;
            std::string err;
            int         n = 0;
            const bool  ok = decode_tiff(bytes.data(), bytes.size(), img, &err, [&](const std::vector<unsigned char> &j, PhotoImage &part) {
                std::ofstream o(path + ".jpgstream" + std::to_string(n++), std::ios::binary);
                o.write(reinterpret_cast<const char *>(j.data()), std::streamsize(j.size()));
                // No JPEG decoder here: a gray image of the frame's size (SOF marker) keeps the decoder going.
                for (size_t i = 0; i + 8 < j.size(); ++i)
                    if (j[i] == 0xFF && (j[i + 1] == 0xC0 || j[i + 1] == 0xC1 || j[i + 1] == 0xC2)) {
                        part.height = j[i + 5] << 8 | j[i + 6];
                        part.width  = j[i + 7] << 8 | j[i + 8];
                        part.rgb.assign(size_t(part.width) * size_t(part.height) * 3, 128);
                        return true;
                    }
                return false;
            });
            std::printf("%s: %s %s\n", path.c_str(), ok ? "ok" : "failed", err.c_str());
            if (ok) {
                std::ofstream o(path + ".ppm", std::ios::binary);
                o << "P6\n" << img.width << " " << img.height << "\n255\n";
                o.write(reinterpret_cast<const char *>(img.rgb.data()), std::streamsize(img.rgb.size()));
            }
        }
    }
    Predictor nominal(b, nullptr, sched);
    auto recipe_of = [&](const SwatchSpec &s) {
        Recipe r;
        r.word    = s.word;
        r.weights = word_weights(s.word, b.filaments.size(), sched);
        if (!s.inner_word.empty()) {
            r.inner_word    = s.inner_word;
            r.inner_weights = word_weights(s.inner_word, b.filaments.size(), sched);
        }
        return r;
    };
    {
        std::ofstream f(dir + "/plan.tsv");
        f << "# plan " << calibration_plan_id(plan) << "\n";
        f << "swatch\tboard\trow\tcol\theld_out\tword\tinner\tpredL\tpreda\tpredb\tlabel\n";
        for (size_t i = 0; i < plan.size(); ++i) {
            const auto pos = swatch_position(int(i), L);
            std::string w, in;
            for (int x : plan[i].word) w += b.filaments[size_t(x)].id.substr(0, 2) + " ";
            for (int x : plan[i].inner_word) in += b.filaments[size_t(x)].id.substr(0, 2) + " ";
            const Lab p = nominal.predict(recipe_of(plan[i])).color;
            f << i + 1 << '\t' << pos[0] + 1 << '\t' << pos[1] + 1 << '\t' << pos[2] + 1 << '\t' << plan[i].held_out << '\t' << w << '\t'
              << in << '\t' << p.L << '\t' << p.a << '\t' << p.b << '\t' << plan[i].label << "\n";
        }
    }
    if (const char *jobs = std::getenv("COLORMIX_PHOTO_JOBS")) {
        std::ifstream jf(jobs);
        std::string   line;
        while (std::getline(jf, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream ls(line);
            std::string        path, name;
            int                scanner = 0, board = 1;
            BoardCorners       c;
            ls >> path >> scanner;
            for (auto &p : c)
                ls >> p[0] >> p[1];
            ls >> board >> name;
            std::ifstream pf(path, std::ios::binary);
            std::string   magic;
            int           w = 0, h = 0, mx = 0;
            pf >> magic >> w >> h >> mx;
            pf.get();
            PhotoImage img;
            img.width  = w;
            img.height = h;
            img.rgb.resize(size_t(w) * size_t(h) * 3);
            pf.read(reinterpret_cast<char *>(img.rgb.data()), std::streamsize(img.rgb.size()));
            PhotoReadOptions opt;
            opt.scanner = scanner != 0;
            std::ofstream o(dir + "/" + name + ".tsv");
            try {
                const PhotoReading pr = read_board_photo(img, c, L, opt);
                o << "# paper " << pr.paper_samples << " gap " << pr.paper_gap << " clipped " << pr.paper_clipped << " falloff " << pr.light_falloff << " warning: " << pr.warning << "\n";
                o << "swatch\tL\ta\tb\tvalid\tspread\n";
                for (size_t k = 0; k < pr.cells.size(); ++k)
                    o << (board - 1) * L.swatches_per_board() + int(k) + 1 << '\t' << pr.cells[k].L << '\t' << pr.cells[k].a << '\t'
                      << pr.cells[k].b << '\t' << pr.valid[k] << '\t' << pr.spread[k] << "\n";
            } catch (const std::exception &e) {
                o << "# error: " << e.what() << "\n";
            }
        }
    }
    if (const char *fit = std::getenv("COLORMIX_FIT_IN")) {
        std::ifstream ff(fit);
        std::vector<Lab>  measured(plan.size());
        std::vector<bool> has(plan.size(), false);
        std::string line;
        while (std::getline(ff, line)) {
            std::istringstream ls(line);
            int idx = 0;
            Lab l;
            if (ls >> idx >> l.L >> l.a >> l.b && idx >= 1 && idx <= int(plan.size())) {
                measured[size_t(idx - 1)] = l;
                has[size_t(idx - 1)]      = true;
            }
        }
        for (const FilamentCheck &c : check_filaments(b, plan, measured, has, sched))
            std::printf("CHECK: %s didn't print? swatches with it %.1f dE from prediction, others %.1f (%zu swatches)\n",
                        b.filaments[size_t(c.filament)].name.c_str(), c.median_with, c.median_without, c.swatches.size());
        Bundle cb = b;
        cb.calibrations.push_back(make_calibration(b, plan, measured, has, sched, "diag", "CORE One INDX"));
        cb.calibrations.back().white_core = true;
        Predictor p(cb, &cb.calibrations.back(), sched);
        std::ofstream o(dir + "/fit.tsv");
        o << "# " << p.report().summary() << "\n";
        o << "# filament colors:";
        for (size_t i = 0; i < p.filament_colors().size(); ++i)
            o << " " << b.filaments[i].id << "=" << lab_to_hex(p.filament_colors()[i]);
        o << "\nswatch\tmeasured\tnominal\tde_nominal\n";
        for (size_t i = 0; i < plan.size(); ++i)
            if (has[i]) {
                const Lab n = nominal.predict(recipe_of(plan[i])).color;
                o << i + 1 << '\t' << lab_to_hex(measured[i]) << '\t' << lab_to_hex(n) << '\t' << delta_e_2000(measured[i], n) << "\n";
            }
        std::printf("%s\n", p.report().summary().c_str());
        // COLORMIX_GAMUT_OUT: every candidate recipe the converter would consider, predicted with this
        // calibration: with / without Cyan, single walls / with dual walls.
        if (const char *g = std::getenv("COLORMIX_GAMUT_OUT")) {
            std::ofstream go(g);
            go << "set\tdual_on\tdual\tL\ta\tb\tname\n";
            for (int with_cyan = 0; with_cyan < 2; ++with_cyan)
                for (int dual = 0; dual < 2; ++dual) {
                    std::vector<unsigned int> t = tools;
                    if (!with_cyan)
                        t[size_t(b.filament_index("cyan"))] = 0;
                    for (const char *fx : {"gold-glitter", "silver-glitter"})
                        t[size_t(b.filament_index(fx))] = 0;
                    ConvertOptions opt;
                    opt.schedule   = sched;
                    opt.dual_walls = dual != 0;
                    const ConversionResult r = convert_colors({}, cb, p, t, opt);
                    for (const auto &c : r.candidates)
                        go << (with_cyan ? "cmywk" : "mywk") << '\t' << int(dual) << '\t' << c.recipe.dual() << '\t' << c.prediction.color.L
                           << '\t' << c.prediction.color.a << '\t' << c.prediction.color.b << '\t' << c.name << "\n";
                }
        }
    }
}

// Diagnostic: the filament database parsers on real downloads.
//   COLORMIX_DB_DIR=<folder with fc_1.json ... and spoolmandb.json> libslic3r_tests "[ColorMixDbDiag]"
TEST_CASE("ColorMix filament database diagnostics", "[.][ColorMixDbDiag]")
{
    const char *dir = std::getenv("COLORMIX_DB_DIR");
    if (dir == nullptr)
        return;
    auto read = [](const std::string &p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    std::vector<FilamentRecord> measured;
    for (int i = 1; i <= 40; ++i) {
        const std::string t = read(std::string(dir) + "/fc_" + std::to_string(i) + ".json");
        if (t.empty())
            break;
        auto page = parse_filamentcolors(t);
        measured.insert(measured.end(), page.begin(), page.end());
    }
    const auto catalog = parse_spoolmandb(read(std::string(dir) + "/spoolmandb.json"));
    const auto all     = merge_filament_records(measured, catalog);
    size_t td = 0, eff = 0;
    for (const auto &f : all) {
        td += f.td > 0. ? 1 : 0;
        eff += f.effect ? 1 : 0;
    }
    std::printf("measured %zu, catalog %zu, merged %zu (with TD %zu, effect %zu), json %zu bytes\n", measured.size(), catalog.size(), all.size(),
                td, eff, filament_db_to_json(all, "x").size());
    for (const char *q : {"prusament galaxy black", "polymaker panchroma", "bambu basic white", "sunlu silk"}) {
        const auto h = search_filaments(all, q, 3);
        std::printf("'%s': %zu hits", q, search_filaments(all, q).size());
        for (size_t i : h)
            std::printf(" | %s %s %s %s td %.1f %s", all[i].manufacturer.c_str(), all[i].name.c_str(), all[i].material.c_str(), all[i].color.c_str(),
                        all[i].td, all[i].measured ? "measured" : "catalog");
        std::printf("\n");
    }
}

// Does TD (HueForge transmission distance) predict how strongly a filament shows in a layer mix?
// Prusa's measured mixes (prusa-fdm-mixer fitting / holdout sets) whose filaments all have a TD.
//   COLORMIX_TD_EVAL=<td_eval.tsv: set, L, a, b, then hex, td, ratio per filament> libslic3r_tests "[ColorMixTdDiag]"
TEST_CASE("ColorMix TD strength diagnostics", "[.][ColorMixTdDiag]")
{
    const char *path = std::getenv("COLORMIX_TD_EVAL");
    if (path == nullptr)
        return;
    struct Mix { std::string set; Lab lab; std::vector<std::tuple<Lab, double, double>> parts; };
    std::vector<Mix> mixes;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        std::istringstream ss(line);
        Mix m;
        ss >> m.set >> m.lab.L >> m.lab.a >> m.lab.b;
        std::string hex;
        double td, ratio;
        while (ss >> hex >> td >> ratio)
            m.parts.emplace_back(hex_to_lab(hex), td, ratio);
        if (m.parts.size() >= 2)
            mixes.push_back(std::move(m));
    }
    std::printf("%zu mixes\n", mixes.size());
    for (bool subtractive : {false, true})
        for (double power : {0., 0.15, 0.25, 0.35, 0.5, 0.75, 1.}) {
            std::vector<double> err[2];
            for (const Mix &m : mixes) {
                std::vector<double> w;
                double tot = 0.;
                for (const auto &[lab, td, ratio] : m.parts) {
                    w.push_back(ratio * std::clamp(std::pow(3. / std::max(td, 0.1), power), 0.25, 4.));
                    tot += w.back();
                }
                Lab pred;
                if (subtractive) {
                    LinRGB acc{0., 0., 0.};
                    for (size_t i = 0; i < w.size(); ++i) {
                        const LinRGB lin = lab_to_linear(std::get<0>(m.parts[i]));
                        for (int c = 0; c < 3; ++c)
                            acc[c] += w[i] / tot * std::log(std::max(lin[c], 1e-4));
                    }
                    pred = linear_to_lab({std::exp(acc[0]), std::exp(acc[1]), std::exp(acc[2])});
                } else {
                    std::vector<std::pair<Lab, double>> parts;
                    for (size_t i = 0; i < w.size(); ++i)
                        parts.emplace_back(std::get<0>(m.parts[i]), w[i] / tot);
                    pred = fdm_mixer_predict(parts);
                }
                err[m.set == "hol"].push_back(delta_e_2000(pred, m.lab));
            }
            auto med = [](std::vector<double> v) {
                std::sort(v.begin(), v.end());
                return v.empty() ? 0. : v[v.size() / 2];
            };
            auto mean = [](const std::vector<double> &v) { return v.empty() ? 0. : std::accumulate(v.begin(), v.end(), 0.) / double(v.size()); };
            std::printf("%-11s power %.2f  fitting set: median %.2f mean %.2f (n %zu)   holdout: median %.2f mean %.2f (n %zu)\n",
                        subtractive ? "subtractive" : "prusa", power, med(err[0]), mean(err[0]), err[0].size(), med(err[1]), mean(err[1]),
                        err[1].size());
        }
}

// The fit of a bundle file's newest calibration and every filament's strength.
//   COLORMIX_BUNDLE_FILE=<bundle .json> libslic3r_tests "[ColorMixFitDiag]"
TEST_CASE("ColorMix bundle fit diagnostics", "[.][ColorMixFitDiag]")
{
    const char *path = std::getenv("COLORMIX_BUNDLE_FILE");
    if (path == nullptr)
        return;
    std::string err;
    const std::optional<Bundle> b = load_bundle(path, &err);
    REQUIRE(b.has_value());
    LayerSchedule sched;
    const Calibration *cal = b->calibrations.empty() ? nullptr : &b->calibrations.back();
    Predictor p(*b, cal, sched);
    std::printf("%s\n", p.report().summary().c_str());
    for (size_t i = 0; i < b->filaments.size(); ++i)
        std::printf("  %-26s td %5.1f  strength %.2f\n", b->filaments[i].name.c_str(), b->filaments[i].td, p.strengths()[i]);
}

// Calibration plate size for a bundle file, as planned, and with its effect filaments made mixable.
//   COLORMIX_BUNDLE_FILE=<bundle .json> libslic3r_tests "[ColorMixPlanDiag]"
TEST_CASE("ColorMix calibration plan size diagnostics", "[.][ColorMixPlanDiag]")
{
    const char *path = std::getenv("COLORMIX_BUNDLE_FILE");
    if (path == nullptr)
        return;
    std::optional<Bundle> b = load_bundle(path);
    REQUIRE(b.has_value());
    LayerSchedule sched;
    std::vector<unsigned int> tools;
    for (size_t i = 0; i < b->filaments.size(); ++i)
        tools.push_back(unsigned(i + 1));
    auto report = [&](const Bundle &bb, const char *what) {
        for (int dual : {0, 2}) {
            const std::vector<SwatchSpec> plan = plan_calibration_swatches(bb, tools, sched, 4, 10, dual, 48, 255 - 1 - 8);
            std::map<int, int> by_effect;   // swatches by number of effect filaments in them
            for (const SwatchSpec &s : plan) {
                std::set<int> f(s.word.begin(), s.word.end());
                f.insert(s.inner_word.begin(), s.inner_word.end());
                int n = 0;
                for (int i : f)
                    n += bb.filaments[size_t(i)].id.find("glitter") != std::string::npos ? 1 : 0;
                ++by_effect[n];
            }
            std::printf("%-34s dual %d: %zu swatches (", what, dual, plan.size());
            for (auto [n, c] : by_effect)
                std::printf(" %d with %d glitter;", c, n);
            std::printf(" )\n");
        }
    };
    report(*b, "as is");
    Bundle m = *b;
    for (BundleFilament &f : m.filaments)
        f.role = MixRole::Mix;
    report(m, "every filament set to Mix");
}

// Every recipe ColorMax would consider with a bundle file's newest calibration (the five ColorMix colors in
// tools 1-5, dual walls on), as a gamut table (TSV), plus how many are distinguishable.
//   COLORMIX_BUNDLE_FILE=<bundle .json> COLORMIX_GAMUT_OUT=<tsv> libslic3r_tests "[ColorMixGamutDump]"
TEST_CASE("ColorMix gamut dump", "[.][ColorMixGamutDump]")
{
    const char *path = std::getenv("COLORMIX_BUNDLE_FILE");
    if (path == nullptr)
        return;
    const std::optional<Bundle> b = load_bundle(path);
    REQUIRE(b.has_value());
    LayerSchedule sched;
    const Calibration *cal = b->calibrations.empty() ? nullptr : &b->calibrations.back();
    Predictor p(*b, cal, sched);
    std::vector<unsigned int> tools(b->filaments.size(), 0);
    for (const char *id : {"cyan", "magenta", "yellow", "white", "black"})
        if (const int k = b->filament_index(id); k >= 0)
            tools[size_t(k)] = unsigned(k + 1);
    auto distinct = [](const std::vector<Lab> &cols, double de) {
        std::vector<Lab> kept;
        for (const Lab &c : cols)
            if (std::none_of(kept.begin(), kept.end(), [&](const Lab &k) { return delta_e_2000(c, k) < de; }))
                kept.push_back(c);
        return kept.size();
    };
    for (bool dual : {false, true}) {
        ConvertOptions opt;
        opt.schedule   = sched;
        opt.dual_walls = dual;
        const ConversionResult r = convert_colors({}, *b, p, tools, opt);
        std::vector<Lab> cols;
        size_t measured = 0;
        for (const Candidate &c : r.candidates) {
            cols.push_back(c.prediction.color);
            measured += c.prediction.measured ? 1 : 0;
        }
        std::printf("%s: %zu recipes (%zu measured exactly), distinguishable at 1 dE: %zu, at 2 dE: %zu, at 3 dE: %zu\n",
                    dual ? "single + dual walls" : "single wall only", r.candidates.size(), measured, distinct(cols, 1.), distinct(cols, 2.),
                    distinct(cols, 3.));
        if (dual)
            if (const char *out = std::getenv("COLORMIX_GAMUT_OUT")) {
                std::ofstream o(out);
                o << "set\tdual_on\tdual\tL\ta\tb\tname\n";
                for (const Candidate &c : r.candidates)
                    o << "cmywk\t1\t" << (c.recipe.dual() ? 1 : 0) << '\t' << c.prediction.color.L << '\t' << c.prediction.color.a << '\t'
                      << c.prediction.color.b << '\t' << c.name << "\n";
            }
    }
}
