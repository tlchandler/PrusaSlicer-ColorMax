///|/ ColorMax engine - calibration boards and measurement import.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "Calibration.hpp"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <locale>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <random>

#include "Converter.hpp"
#include "Predictor.hpp"
#include "ModelConvert.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r::ColorMix {

namespace {
// Fisher-Yates on raw mt19937 output: std::shuffle's algorithm is implementation-defined, and the
// plate must be the same on every build (the same settings always give the same swatches).
template<class T> void portable_shuffle(std::vector<T> &v, std::mt19937 &rng)
{
    for (size_t i = v.size(); i > 1; --i)
        std::swap(v[i - 1], v[size_t(rng() % i)]);
}
} // namespace


std::vector<SwatchSpec> plan_calibration_swatches(const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament,
                                                  const LayerSchedule &schedule, int max_len, int held_out, int dual_level,
                                                  int fill_to, int max_blended, PlanReport *report)
{
    std::vector<int> mixable;
    for (int f : bundle.mixable_indices())
        if (size_t(f) < tool_of_filament.size() && tool_of_filament[size_t(f)] > 0)
            mixable.push_back(f);
    std::vector<Lab> colors;
    for (const BundleFilament &f : bundle.filaments)
        colors.push_back(hex_to_lab(f.color));

    RecipeRules short_rules     = bundle.rules;
    short_rules.max_word_length = std::min(max_len, bundle.rules.max_word_length);
    std::vector<Recipe> base = enumerate_recipes(mixable, bundle.filaments.size(), short_rules, schedule, colors);
    std::vector<SwatchSpec> out;
    for (const Recipe &r : base)
        out.push_back({r.word, {}, false, recipe_name(r, bundle)});

    if (held_out > 0 && bundle.rules.max_word_length > short_rules.max_word_length) {
        std::vector<Recipe> all = enumerate_recipes(mixable, bundle.filaments.size(), bundle.rules, schedule, colors);
        std::vector<Recipe> longer;
        for (const Recipe &r : all)
            if (int(r.word.size()) > short_rules.max_word_length)
                longer.push_back(r);
        std::mt19937 rng(12345); // fixed seed: the same bundle always gives the same plate
        portable_shuffle(longer, rng);
        for (int i = 0; i < held_out && i < int(longer.size()); ++i)
            out.push_back({longer[size_t(i)].word, {}, true, recipe_name(longer[size_t(i)], bundle) + "  (validation)"});
    }
    auto name_of = [&](const Word &w) {
        Recipe r;
        r.word    = w;
        r.weights = word_weights(w, bundle.filaments.size(), schedule);
        return recipe_name(r, bundle);
    };
    auto add = [&](const Word &o, const Word &in, bool held_out) {
        SwatchSpec sw;
        sw.word       = o;
        sw.inner_word = in;
        sw.held_out   = held_out;
        sw.label      = "outer " + name_of(o) + "  |  inner " + name_of(in) + (held_out ? "  (validation)" : "");
        out.push_back(sw);
    };
    // Inner-wall words: up to 4 layers, 2 filaments, one more layer of run allowed.
    RecipeRules inner_rules       = bundle.rules;
    inner_rules.max_run           = bundle.rules.max_run + 1;
    inner_rules.recurrence_window = bundle.rules.recurrence_window + 1;
    inner_rules.max_word_length   = std::min(4, bundle.rules.max_word_length);
    inner_rules.max_filaments     = 2;
    if (dual_level > 0) {
        auto contains = [](const Word &w, int f) { return std::find(w.begin(), w.end(), f) != w.end(); };
        // Every (outer, inner) pair of pure filaments: the pair table of the dual-wall model.
        for (int o : mixable)
            for (int in : mixable)
                if (o != in)
                    add({o}, {in}, false);
        std::vector<Word> blends11, triples;
        for (size_t i = 0; i < mixable.size(); ++i)
            for (size_t j = i + 1; j < mixable.size(); ++j) {
                blends11.push_back({mixable[i], mixable[j]});
                for (size_t k = j + 1; k < mixable.size(); ++k)
                    triples.push_back({mixable[i], mixable[j], mixable[k]});
            }
        if (dual_level == 1) {
            int added = 0;
            for (const Word &w : blends11)
                for (int in : mixable)
                    if (!contains(w, in) && added < 10) {
                        add(w, {in}, false);
                        ++added;
                        break;
                    }
        } else {
            for (const Word &w : blends11)          // 1:1 outer over every other pure inner
                for (int in : mixable)
                    if (!contains(w, in))
                        add(w, {in}, false);
            for (int o : mixable)                    // pure outer over every 1:1 inner of two others
                for (const Word &w : blends11)
                    if (!contains(w, o))
                        add({o}, w, false);
            for (const Word &w : triples)            // 1:1:1 outer over every other pure inner
                for (int in : mixable)
                    if (!contains(w, in))
                        add(w, {in}, false);
            // Held-out validation pairs: longer outer words over 1:1 or 3:1 inner words.
            std::vector<Recipe> outers = enumerate_recipes(mixable, bundle.filaments.size(), bundle.rules, schedule, colors);
            std::vector<Recipe> inners = enumerate_recipes(mixable, bundle.filaments.size(), inner_rules, schedule, colors);
            std::mt19937 rng(54321);
            portable_shuffle(outers, rng);
            portable_shuffle(inners, rng);
            // Never a pair that is also fitted: its measurement would validate nothing.
            auto in_plan = [&](const Word &o, const Word &in) {
                return std::any_of(out.begin(), out.end(), [&](const SwatchSpec &sw) { return sw.word == o && sw.inner_word == in; });
            };
            for (int i = 0, k = 0, tries = 0; i < 10 && k < int(outers.size()) && !inners.empty(); ++k)
                if (outers[size_t(k)].num_filaments >= 2) {
                    const Word &in = inners[size_t(i + tries) % inners.size()].word;
                    if (in == outers[size_t(k)].word || in_plan(outers[size_t(k)].word, in)) {
                        ++tries;
                        continue;
                    }
                    add(outers[size_t(k)].word, in, true);
                    ++i;
                }
        }
    }
    // Solid-only and accent filaments: one solid swatch each (their real color is measured). Accents
    // (glitter: clear PLA with flakes, layered for sparkle) also get a targeted set instead of every
    // combination, which would crowd the regular swatches off the plate.
    {
        auto loaded = [&](int f) { return size_t(f) < tool_of_filament.size() && tool_of_filament[size_t(f)] > 0; };
        auto single = [&](const Word &w) { out.push_back({w, {}, false, name_of(w)}); };
        std::vector<int> accents;
        for (size_t f = 0; f < bundle.filaments.size(); ++f)
            if (loaded(int(f)) && !bundle.filaments[f].mixes()) {
                single({int(f)});
                if (bundle.filaments[f].role == MixRole::Accent)
                    accents.push_back(int(f));
            }
        for (int a : accents) {
            for (int m : mixable)
                single({m, a});
            for (const char *id : {"white", "black"})
                if (const int k = bundle.filament_index(id); k >= 0 && std::find(mixable.begin(), mixable.end(), k) != mixable.end()) {
                    single({k, k, k, a});
                    single({k, a, a, a});
                }
        }
        for (size_t i = 0; i < accents.size(); ++i)
            for (size_t j = i + 1; j < accents.size(); ++j)
                single({accents[i], accents[j]});
    }
    // Fill the last board with extra validation swatches instead of empty (frame colored) cells.
    if (fill_to > 0 && out.size() % size_t(fill_to) != 0 && !mixable.empty()) {
        auto used = [&](const Word &w, const Word &in) {
            return std::any_of(out.begin(), out.end(), [&](const SwatchSpec &sw) { return sw.word == w && sw.inner_word == in; });
        };
        std::vector<Recipe> singles = enumerate_recipes(mixable, bundle.filaments.size(), bundle.rules, schedule, colors);
        singles.erase(std::remove_if(singles.begin(), singles.end(), [](const Recipe &r) { return r.num_filaments < 2; }), singles.end());
        std::mt19937 rng(777); // fixed seed: the same settings always give the same plate
        portable_shuffle(singles, rng);
        {
            // When the rules run out of new recipes, continue with words allowed one more layer of run.
            RecipeRules relaxed       = bundle.rules;
            relaxed.max_run           = bundle.rules.max_run + 1;
            relaxed.recurrence_window = bundle.rules.recurrence_window + 1;
            std::vector<Recipe> more  = enumerate_recipes(mixable, bundle.filaments.size(), relaxed, schedule, colors);
            more.erase(std::remove_if(more.begin(), more.end(), [&](const Recipe &r) {
                           return r.num_filaments < 2 || std::any_of(singles.begin(), singles.end(), [&](const Recipe &s) { return s.word == r.word; });
                       }),
                       more.end());
            portable_shuffle(more, rng);
            singles.insert(singles.end(), more.begin(), more.end());
        }
        std::vector<Recipe> outers, inners;
        if (dual_level > 0 && mixable.size() >= 2) {
            outers = singles;
            inners = enumerate_recipes(mixable, bundle.filaments.size(), inner_rules, schedule, colors);
            portable_shuffle(inners, rng);
        }
        size_t next_single = 0, next_outer = 0, next_inner = 0;
        for (int step = 0; out.size() % size_t(fill_to) != 0; ++step) {
            // Alternate single-wall and dual-wall swatches; fall back to the other kind when one runs out.
            bool added = false;
            for (int pass = 0; pass < 2 && !added; ++pass) {
                if ((pass == 0) == (step % 2 == 1)) {
                    for (int tries = 0; tries < 200 && !added && !outers.empty() && !inners.empty(); ++tries) {
                        const Word &o  = outers[next_outer++ % outers.size()].word;
                        const Word &in = inners[next_inner++ % inners.size()].word;
                        if (o != in && !used(o, in)) {
                            add(o, in, true);
                            added = true;
                        }
                    }
                } else {
                    while (!added && next_single < singles.size()) {
                        const Recipe &r = singles[next_single++];
                        if (!used(r.word, {})) {
                            out.push_back({r.word, {}, true, recipe_name(r, bundle) + "  (validation)"});
                            added = true;
                        }
                    }
                }
            }
            if (!added)
                break; // nothing left to add
        }
    }
    // Each blended swatch needs its own painting state: drop validation swatches (last first),
    // then dual-wall ones, then any, until the plan fits.
    const int budget = max_blended >= 0 ? max_blended : int(TRIANGLE_STATE_TYPE_COUNT) - 1 - 16;
    auto blended = [](const SwatchSpec &sw) { return sw.word.size() > 1 || !sw.inner_word.empty(); };
    PlanReport rep;
    rep.budget = budget;
    rep.needed = int(std::count_if(out.begin(), out.end(), blended));
    for (int n = rep.needed; n > budget; --n) {
        auto drop = out.end();
        for (auto pred : {std::function<bool(const SwatchSpec &)>([&](const SwatchSpec &sw) { return blended(sw) && sw.held_out; }),
                          std::function<bool(const SwatchSpec &)>([&](const SwatchSpec &sw) { return blended(sw) && !sw.inner_word.empty(); }),
                          std::function<bool(const SwatchSpec &)>(blended)}) {
            auto it = std::find_if(out.rbegin(), out.rend(), pred);
            if (it != out.rend()) {
                drop = std::next(it).base();
                break;
            }
        }
        if (drop == out.end())
            break;
        if (drop->held_out)
            ++rep.dropped_validation;
        else if (!drop->inner_word.empty())
            ++rep.dropped_dual;
        else
            ++rep.dropped_core;
        out.erase(drop);
    }
    if (report != nullptr)
        *report = rep;
    char buf[16];
    for (size_t i = 0; i < out.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "S%02d  ", int(i + 1));
        out[i].label = buf + out[i].label;
    }
    return out;
}

void center_on_bed(Model &model, const Vec2d &bed_center)
{
    BoundingBoxf3 bb;
    for (const ModelObject *o : model.objects)
        for (size_t i = 0; i < o->instances.size(); ++i)
            bb.merge(o->instance_bounding_box(i));
    if (!bb.defined)
        return;
    const Vec3d d(bed_center.x() - bb.center().x(), bed_center.y() - bb.center().y(), 0.);
    for (ModelObject *o : model.objects)
        for (ModelInstance *inst : o->instances)
            inst->set_offset(inst->get_offset() + d);
}

std::array<int, 3> swatch_position(int index, const BoardLayout &layout)
{
    const int  per   = layout.swatches_per_board();
    const auto cells = layout.swatch_cells();
    const auto rc    = cells[size_t(index % per)];
    return {index / per, rc[0], rc[1]};
}

int board_count(int n, const BoardLayout &layout) { return (n + layout.swatches_per_board() - 1) / layout.swatches_per_board(); }

std::string calibration_plan_id(const std::vector<SwatchSpec> &swatches)
{
    // FNV-1a over the words (not the names: they change with the language).
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {
        h ^= v;
        h *= 16777619u;
    };
    for (const SwatchSpec &sw : swatches) {
        for (int f : sw.word)
            mix(uint32_t(f + 1));
        mix(0xFEu);
        for (int f : sw.inner_word)
            mix(uint32_t(f + 1));
        mix(sw.held_out ? 0xFDu : 0xFCu);
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04X-%u", unsigned((h ^ (h >> 16)) & 0xFFFFu), unsigned(swatches.size()));
    return buf;
}

std::string build_calibration_plate(Model &model, const std::vector<SwatchSpec> &swatches, const Bundle &bundle,
                                    const std::vector<unsigned int> &tool_of_filament, unsigned int num_physical,
                                    const BoardLayout &L, unsigned int frame_tool, double bed_depth, double first_layer_height)
{
    model.clear_objects();
    model.virtual_extruders.clear();
    if (frame_tool == 0)
        for (unsigned int t : tool_of_filament)
            if (t > 0 && (frame_tool == 0 || t < frame_tool))
                frame_tool = t;
    if (frame_tool == 0)
        frame_tool = 1;

    // Extruder (painting state) of every swatch.
    FullSpectrum::VirtualExtruders ves;
    unsigned int                   next_id = num_physical + 1;
    std::vector<unsigned int>      ext(swatches.size(), 0);
    for (size_t i = 0; i < swatches.size(); ++i) {
        const SwatchSpec &sw = swatches[i];
        if (sw.word.size() == 1 && sw.inner_word.empty()) {
            const int f = sw.word.front();
            ext[i] = size_t(f) < tool_of_filament.size() ? tool_of_filament[size_t(f)] : 0;
        } else if (next_id < TRIANGLE_STATE_TYPE_COUNT) {
            Recipe r;
            r.word       = sw.word;
            r.inner_word = sw.inner_word;
            // Preview color: dual-wall swatches blend the inner wall in (uncalibrated dual-wall model), so
            // swatches that share an outer word but differ in their inner wall don't look identical.
            const double          outer_share = sw.inner_word.empty() ? 1. : Predictor::DUAL_OUTER_SHARE;
            std::map<int, double> cnt;
            for (int f : sw.word)
                cnt[f] += outer_share / double(sw.word.size());
            for (int f : sw.inner_word)
                cnt[f] += (1. - outer_share) / double(sw.inner_word.size());
            std::vector<std::pair<Lab, double>> parts;
            for (auto &[f, w] : cnt)
                parts.emplace_back(hex_to_lab(bundle.filaments[size_t(f)].color), w);
            Prediction p;
            p.color = fdm_mixer_predict(parts);
            ext[i]  = next_id++;
            ves.push_back(make_virtual_extruder(ext[i], r, p, tool_of_filament));
        }
    }

    auto paint = [](ModelVolume *v, unsigned int state) {
        TriangleSelector sel(v->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(state)}});
        v->mm_segmentation_facets.set(sel);
    };
    // Board b stands on the bed with its front face at y = front_y[b] and its left end at x = left.
    const auto   cells  = L.swatch_cells();
    const int    per    = L.swatches_per_board();
    const int    boards = board_count(int(swatches.size()), L);
    const double left   = 10.;
    std::vector<double> front_y;
    for (int b = 0; b < boards; ++b)
        front_y.push_back(bed_depth - 15. - b * L.pitch() - 0.5 * L.thickness);
    auto add_box = [&](ModelObject *o, int board, double x0, double z0, double w, double h, unsigned int state, const std::string &name) {
        if (w <= 0. || h <= 0.)
            return;
        // add_volume() centers the mesh on its own middle, so place the volume by its center.
        ModelVolume *v = o->add_volume(make_cube(w, L.thickness, h));
        v->set_offset(Vec3d(left + x0 + 0.5 * w, front_y[size_t(board)] + 0.5 * L.thickness, z0 + 0.5 * h));
        v->name = "Board " + std::to_string(board + 1) + ": " + name; // shown in the object list
        paint(v, state);
    };

    std::string  legend = "ColorMax calibration boards - " + bundle.name + " - plan " + calibration_plan_id(swatches) + "\n\n"
                          "Board k has k dots on its top edge, at the left end. Swatches are numbered left to right, top row first.\n"
                          "Colorimeter: measure the center of each swatch on the front face (the face toward the front of the printer).\n"
                          "Scanner (recommended): cut a hole the size of a board into a sheet of plain white paper, lay the sheet on\n"
                          "the glass and the board face-down in the hole, then scan with automatic corrections off.\n"
                          "Photo: lay the boards face-up on a sheet of plain white paper (at least 2 cm of paper all around), light\n"
                          "them evenly from both sides without glare, photograph straight down.\n"
                          "Then use \"Read a board (scan or photo)...\" (it shows detailed instructions) and click the corners starting\n"
                          "at the dotted corner, clockwise.\n"
                          "The boards stand on a one-layer sheet that holds them on the bed: peel it off after printing.\n\n";
    // One object: the boards and the sheet under them. The sheet is part of the boards' first layer,
    // so it ties every board to the others and gives the thin, tall boards a wide footprint (separate
    // objects with brims didn't join up, and PrusaSlicer warned about bed adhesion).
    ModelObject *o = model.add_object();
    o->name        = "ColorMax calibration boards";
    // No supports (the project's own supports would grow into the photo windows) and no brim (the
    // sheet replaces it). Both are ordinary per-object settings.
    o->config.set("support_material", false);
    o->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    {
        // Rounded rectangle around all boards, one first layer thick.
        const double margin = 5., radius = 4., h = first_layer_height > 0. ? first_layer_height : 0.2;
        const double x0 = left - margin, x1 = left + L.width() + margin;
        const double y0 = front_y.back() - margin, y1 = front_y.front() + L.thickness + margin;
        // Counter-clockwise from the front right corner; corner c spans the angles [(c-1)*90, c*90].
        std::vector<Vec2d> outline;
        const int          arc        = 8;
        const Vec2d        centers[4] = {{x1 - radius, y0 + radius}, {x1 - radius, y1 - radius}, {x0 + radius, y1 - radius}, {x0 + radius, y0 + radius}};
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i <= arc; ++i) {
                const double a = (c - 1) * 0.5 * PI + 0.5 * PI * i / arc;
                outline.push_back(centers[c] + radius * Vec2d(std::cos(a), std::sin(a)));
            }
        // A convex prism: fan-triangulated caps and quad sides.
        indexed_triangle_set its;
        const int            n = int(outline.size());
        for (const Vec2d &p : outline)
            its.vertices.emplace_back(float(p.x()), float(p.y()), 0.f);
        for (const Vec2d &p : outline)
            its.vertices.emplace_back(float(p.x()), float(p.y()), float(h));
        for (int i = 1; i + 1 < n; ++i) {
            its.indices.emplace_back(0, i + 1, i);         // bottom, facing down
            its.indices.emplace_back(n, n + i, n + i + 1); // top, facing up
        }
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            its.indices.emplace_back(i, j, n + j);
            its.indices.emplace_back(i, n + j, n + i);
        }
        ModelVolume *sheet = o->add_volume(TriangleMesh(std::move(its)));
        sheet->name        = "Sheet under the boards (peel off)";
        paint(sheet, frame_tool);
    }
    for (int b = 0; b < boards; ++b) {
        // Frame: base, sides, top, and the marker dots.
        add_box(o, b, 0., 0., L.width(), L.base, frame_tool, "Frame bottom");
        add_box(o, b, 0., L.base, L.margin, L.rows * L.cell_h, frame_tool, "Frame left");
        add_box(o, b, L.width() - L.margin, L.base, L.margin, L.rows * L.cell_h, frame_tool, "Frame right");
        add_box(o, b, 0., L.height() - L.margin, L.width(), L.margin, frame_tool, "Frame top");
        for (int d = 0; d <= b; ++d)
            add_box(o, b, L.margin + 2. + 5. * d, L.height(), 2.5, 2., frame_tool, "Marker " + std::to_string(d + 1));
        // Cells (windows stay empty).
        for (int k = 0; k < per; ++k) {
            const int          idx   = b * per + k;
            const auto         rect  = L.cell_rect(cells[size_t(k)][0], cells[size_t(k)][1]);
            const bool         used  = idx < int(swatches.size()) && ext[size_t(idx)] > 0;
            const unsigned int state = used ? ext[size_t(idx)] : frame_tool;
            add_box(o, b, rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1], state,
                    used ? swatches[size_t(idx)].label : std::string("Frame (unused cell)"));
        }
        legend += "Board " + std::to_string(b + 1) + ":\n";
        for (int k = 0; k < per; ++k) {
            const int idx = b * per + k;
            if (idx >= int(swatches.size()))
                break;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "  row %d col %2d: ", cells[size_t(k)][0] + 1, cells[size_t(k)][1] + 1);
            legend += buf + swatches[size_t(idx)].label + "\n";
        }
    }
    // Volumes were placed in bed coordinates: center the object and put its instance back there.
    const Vec3d where = o->raw_mesh_bounding_box().center();
    o->center_around_origin(false);
    o->add_instance()->set_offset(Vec3d(where.x(), where.y(), 0.));
    o->ensure_on_bed();
    model.virtual_extruders = FullSpectrum::normalize_virtual_extruders(ves);
    model.colormix.slice.whole_part_regions = true; // full-color cells: segmented as whole parts
    return legend;
}

std::string build_slope_test(Model &model, const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament,
                             unsigned int num_physical, unsigned int frame_tool, const LayerSchedule &schedule, double bed_depth)
{
    model.clear_objects();
    model.virtual_extruders.clear();
    std::vector<int> mixable;
    for (int f : bundle.mixable_indices())
        if (size_t(f) < tool_of_filament.size() && tool_of_filament[size_t(f)] > 0)
            mixable.push_back(f);
    if (mixable.size() < 2)
        return "Load at least two mixable filaments of the bundle.";
    if (frame_tool == 0)
        frame_tool = tool_of_filament[size_t(mixable.front())];
    std::vector<Lab> lab;
    for (const BundleFilament &f : bundle.filaments)
        lab.push_back(hex_to_lab(f.color));
    // Rows: the highest-contrast pair, and the three most colorful filaments.
    std::vector<Word> words;
    {
        Word   best;
        double best_de = -1.;
        for (size_t i = 0; i < mixable.size(); ++i)
            for (size_t j = i + 1; j < mixable.size(); ++j)
                if (const double d = delta_e_2000(lab[size_t(mixable[i])], lab[size_t(mixable[j])]); d > best_de) {
                    best_de = d;
                    best    = {mixable[i], mixable[j]};
                }
        words.push_back(best);
    }
    if (mixable.size() >= 3) {
        std::vector<int> by_chroma = mixable;
        std::sort(by_chroma.begin(), by_chroma.end(), [&](int a, int b) {
            return std::hypot(lab[size_t(a)].a, lab[size_t(a)].b) > std::hypot(lab[size_t(b)].a, lab[size_t(b)].b);
        });
        words.push_back({by_chroma[0], by_chroma[1], by_chroma[2]});
    }
    FullSpectrum::VirtualExtruders ves;
    std::vector<unsigned int>      states;
    for (const Word &w : words) {
        Recipe r;
        r.word          = w;
        r.weights       = word_weights(w, bundle.filaments.size(), schedule);
        r.num_filaments = int(w.size());
        std::vector<std::pair<Lab, double>> parts;
        for (size_t f = 0; f < r.weights.size(); ++f)
            if (r.weights[f] > 0.)
                parts.emplace_back(lab[f], r.weights[f]);
        Prediction p;
        p.color = fdm_mixer_predict(parts);
        const unsigned int id = num_physical + 1 + unsigned(ves.size());
        ves.push_back(make_virtual_extruder(id, r, p, tool_of_filament));
        states.push_back(id);
    }

    const double run = 20., width = 12., gap = 3., base_h = 1., margin = 3.;
    const auto  &angles = slope_test_angles();
    const double plate_w = angles.size() * (width + gap) - gap + 2. * margin, plate_d = run + 2. * margin;
    auto paint = [](ModelVolume *v, unsigned int state) {
        TriangleSelector sel(v->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(state)}});
        v->mm_segmentation_facets.set(sel);
    };
    // Shown in a message box, which wraps the text itself: paragraphs are single lines.
    std::string legend = "ColorMax slope test - " + bundle.name + "\n\n"
                         "Each row has wedges from 40 degrees (left, tallest) down to 5 degrees (right), printed with layer "
                         "mixing only. The steeper the slope, the narrower each layer's band on it.\n\n"
                         "From the distance you look at your prints, find the shallowest wedge that still looks like one even "
                         "color (no visible stripes). Enter its band width as \"Top-surface patches: slopes with bands wider than\" "
                         "in the ColorMax Engine dialog; flatter surfaces then get top patches instead.\n\nBand width per wedge at an average layer height of ";
    char buf[96];
    const double h = schedule.average();
    std::snprintf(buf, sizeof(buf), "%.3f mm:\n", h);
    legend += buf;
    for (double a : angles) {
        std::snprintf(buf, sizeof(buf), "  %2.0f deg: %.2f mm\n", a, h / std::sin(a * PI / 180.));
        legend += buf;
    }
    legend += "\nRows: ";
    for (size_t row = 0; row < words.size(); ++row) {
        Recipe r;
        r.word    = words[row];
        r.weights = word_weights(words[row], bundle.filaments.size(), schedule);
        legend += (row ? ", back: " : "front: ") + recipe_name(r, bundle);

        ModelObject *o = model.add_object();
        o->name        = "ColorMax slope test " + std::to_string(row + 1);
        ModelVolume *base = o->add_volume(make_cube(plate_w, plate_d, base_h));
        base->set_offset(Vec3d(0.5 * plate_w, 0.5 * plate_d, 0.5 * base_h));
        base->name = "Base";
        paint(base, frame_tool);
        for (size_t i = 0; i < angles.size(); ++i) {
            const double H = run * std::tan(angles[i] * PI / 180.);
            indexed_triangle_set its;
            const float w = float(width), L = float(run), hh = float(H);
            its.vertices = {{0.f, 0.f, 0.f}, {w, 0.f, 0.f}, {w, L, 0.f}, {0.f, L, 0.f}, {0.f, L, hh}, {w, L, hh}};
            its.indices  = {{0, 2, 1}, {0, 3, 2}, {3, 5, 2}, {3, 4, 5}, {0, 1, 5}, {0, 5, 4}, {0, 4, 3}, {1, 2, 5}};
            ModelVolume *v = o->add_volume(TriangleMesh(std::move(its)));
            // add_volume() centers the mesh on its bounding box center.
            v->set_offset(Vec3d(margin + i * (width + gap) + 0.5 * width, margin + 0.5 * run, base_h + 0.5 * H));
            v->name = std::to_string(int(angles[i])) + " deg";
            paint(v, states[row]);
        }
        o->center_around_origin(false);
        ModelInstance *inst = o->add_instance();
        inst->set_offset(Vec3d(10. + plate_w / 2., bed_depth - 20. - plate_d / 2. - double(row) * (plate_d + 10.), 0.));
        o->ensure_on_bed();
    }
    legend += "\n";
    model.virtual_extruders = FullSpectrum::normalize_virtual_extruders(ves);
    model.colormix.slice.whole_part_regions = true; // full-color cells: segmented as whole parts
    return legend;
}

// One number with a '.' decimal point; the whole token must be the number.
static bool parse_number(const std::string &token, double &out)
{
    if (token.empty())
        return false;
    std::istringstream in(token);
    in.imbue(std::locale::classic());
    double v = 0.;
    if (!(in >> v) || !in.eof() || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

// "L a b" or #RRGGBB. The three numbers are separated by spaces, tabs or ';' (a comma inside a number
// is then a decimal comma: "62,1 -21,4 -30,2"), or by commas alone with '.' decimals ("62.1,-21.4,-30.2").
// Anything ambiguous is rejected rather than misread.
bool parse_lab_measurement(const std::string &text_in, Lab &out)
{
    const std::string text = boost::algorithm::trim_copy(text_in);
    if (text.empty())
        return false;
    if (!normalize_hex(text).empty() && text[0] == '#') {
        out = hex_to_lab(text);
        return true;
    }
    std::vector<std::string> fields;
    if (text.find_first_of(" \t;") == std::string::npos) {
        boost::split(fields, text, boost::is_any_of(","));
    } else {
        std::vector<std::string> tokens;
        boost::split(tokens, text, boost::is_any_of(" \t;"), boost::token_compress_on);
        for (std::string t : tokens) {
            // A comma next to a separator separates too ("62.1, -21.4, -30.2").
            boost::algorithm::trim_if(t, boost::is_any_of(","));
            if (t.empty())
                continue;
            if (const auto commas = std::count(t.begin(), t.end(), ','); commas > 0) {
                if (commas > 1 || t.find('.') != std::string::npos)
                    return false; // "62.1,-21.4" or "1,2,3" inside one field: ambiguous
                std::replace(t.begin(), t.end(), ',', '.');
            }
            fields.push_back(t);
        }
    }
    if (fields.size() != 3)
        return false;
    double v[3];
    for (size_t i = 0; i < 3; ++i)
        if (!parse_number(boost::algorithm::trim_copy(fields[i]), v[i]))
            return false;
    if (!(v[0] >= 0. && v[0] <= 100.))
        return false;
    out = {v[0], v[1], v[2]};
    return true;
}

CsvMeasurements parse_measurement_csv(const std::string &text, size_t rows)
{
    CsvMeasurements out;
    out.values.assign(rows, std::string());
    auto format = [](const Lab &l) {
        std::ostringstream o;
        o.imbue(std::locale::classic());
        o << std::fixed << std::setprecision(2) << l.L << ' ' << l.a << ' ' << l.b;
        return o.str();
    };
    std::istringstream in(text);
    std::string        line;
    int                line_no = 0, numbered = 0, plain = 0;
    size_t             next_row = 0;
    bool               any_data = false;
    std::vector<std::pair<int, std::string>> plain_values; // (line, value)
    while (std::getline(in, line)) {
        ++line_no;
        boost::algorithm::trim(line);
        if (line.empty() || (line[0] == '#' && normalize_hex(line.substr(0, 7)).empty()))
            continue;
        Lab lab;
        if (parse_lab_measurement(line, lab)) {
            const std::string t = boost::algorithm::trim_copy(line);
            plain_values.emplace_back(line_no, !normalize_hex(t).empty() && t[0] == '#' ? normalize_hex(t) : format(lab));
            ++plain;
            any_data = true;
            continue;
        }
        // A swatch number, then the measurement. Fields are separated by ';' or tabs if there are any
        // (a comma is then a decimal comma), else by commas (decimal points), else by spaces.
        std::vector<std::string> cells;
        if (line.find_first_of(";\t") != std::string::npos)
            boost::split(cells, line, boost::is_any_of(";\t"));
        else if (line.find(',') != std::string::npos)
            boost::split(cells, line, boost::is_any_of(","));
        else
            boost::split(cells, line, boost::is_any_of(" "), boost::token_compress_on);
        for (std::string &c : cells)
            boost::algorithm::trim(c);
        while (!cells.empty() && cells.back().empty())
            cells.pop_back();
        int index = -1;
        if (cells.size() == 4 || cells.size() == 2) {
            std::string idx = cells.front();
            if (!idx.empty() && (idx[0] == 'S' || idx[0] == 's'))
                idx = idx.substr(1);
            if (!idx.empty() && std::all_of(idx.begin(), idx.end(), [](char c) { return c >= '0' && c <= '9'; }))
                index = std::atoi(idx.c_str()) - 1;
        }
        std::string value;
        if (index >= 0) {
            cells.erase(cells.begin());
            value = boost::algorithm::join(cells, "; ");
        }
        if (index < 0 || !parse_lab_measurement(value, lab)) {
            if (any_data)
                out.bad_lines.push_back(line_no);
            continue; // a header before the data is fine
        }
        any_data = true;
        if (size_t(index) >= rows) {
            out.bad_lines.push_back(line_no);
            continue;
        }
        const std::string t = boost::algorithm::trim_copy(value);
        out.values[size_t(index)] = !normalize_hex(t).empty() && t[0] == '#' ? normalize_hex(t) : format(lab);
        ++numbered;
    }
    if (numbered > 0 && plain > 0) {
        out.error = "The file mixes lines with and without swatch numbers.";
        return out;
    }
    if (plain > 0) {
        if (!out.bad_lines.empty()) {
            out.error = "Line " + std::to_string(out.bad_lines.front()) +
                        " could not be read; without swatch numbers every value after it would land on the wrong swatch.";
            return out;
        }
        if (size_t(plain) != rows) {
            out.error = "A file without swatch numbers must have one line per swatch (" + std::to_string(rows) + "); this one has " +
                        std::to_string(plain) + ". Put the swatch number first on each line to load part of the list.";
            return out;
        }
        for (const auto &[ln, v] : plain_values)
            out.values[next_row++] = v;
        out.loaded = plain;
        return out;
    }
    out.loaded = numbered;
    return out;
}

std::vector<FilamentCheck> check_filaments(const Bundle &bundle, const std::vector<SwatchSpec> &swatches, const std::vector<Lab> &measured,
                                           const std::vector<bool> &has_value, const LayerSchedule &schedule)
{
    const Predictor     nominal(bundle, nullptr, schedule);
    std::vector<double> err(swatches.size(), -1.);
    for (size_t i = 0; i < swatches.size() && i < measured.size(); ++i) {
        if (i >= has_value.size() || !has_value[i])
            continue;
        Recipe r;
        r.word    = swatches[i].word;
        r.weights = word_weights(r.word, bundle.filaments.size(), schedule);
        if (!swatches[i].inner_word.empty()) {
            r.inner_word    = swatches[i].inner_word;
            r.inner_weights = word_weights(r.inner_word, bundle.filaments.size(), schedule);
        }
        err[i] = delta_e_2000(measured[i], nominal.predict(r).color);
    }
    auto median = [](std::vector<double> v) {
        if (v.empty())
            return 0.;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::vector<FilamentCheck> out;
    for (int f = 0; f < int(bundle.filaments.size()); ++f) {
        FilamentCheck       c;
        std::vector<double> with, without;
        c.filament = f;
        for (size_t i = 0; i < swatches.size(); ++i) {
            if (err[i] < 0.)
                continue;
            const bool has = std::find(swatches[i].word.begin(), swatches[i].word.end(), f) != swatches[i].word.end() ||
                             std::find(swatches[i].inner_word.begin(), swatches[i].inner_word.end(), f) != swatches[i].inner_word.end();
            (has ? with : without).push_back(err[i]);
            if (has)
                c.swatches.push_back(int(i));
        }
        c.median_with    = median(with);
        c.median_without = median(without);
        // Uncalibrated predictions are typically 5-10 dE off; a missing filament shifts its swatches
        // much further, and only its swatches.
        if (with.size() >= 3 && without.size() >= 3 && c.median_with > 15. && c.median_with > 2. * c.median_without)
            out.push_back(std::move(c));
    }
    return out;
}

Calibration make_calibration(const Bundle &bundle, const std::vector<SwatchSpec> &swatches, const std::vector<Lab> &measured,
                             const std::vector<bool> &has_value, const LayerSchedule &schedule, const std::string &id,
                             const std::string &printer)
{
    Calibration cal;
    cal.id                 = id;
    cal.printer            = printer;
    cal.first_layer_height = schedule.first_layer_height;
    cal.layer_schedule     = schedule.heights;
    for (size_t i = 0; i < swatches.size() && i < measured.size(); ++i) {
        if (i < has_value.size() && !has_value[i])
            continue;
        Swatch s;
        for (int f : swatches[i].word)
            s.word.push_back(bundle.filaments[size_t(f)].id);
        for (int f : swatches[i].inner_word)
            s.inner_word.push_back(bundle.filaments[size_t(f)].id);
        s.measured = measured[i];
        s.held_out = swatches[i].held_out;
        s.note     = swatches[i].label;
        cal.swatches.push_back(std::move(s));
    }
    return cal;
}

bool merge_calibration(const Calibration &old, Calibration &updated, int *kept)
{
    if (kept)
        *kept = 0;
    auto same_heights = [](const std::vector<double> &a, const std::vector<double> &b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(a[i] - b[i]) > 1e-6)
                return false;
        return true;
    };
    if (!same_heights(old.layer_schedule, updated.layer_schedule) || std::abs(old.first_layer_height - updated.first_layer_height) > 1e-6)
        return false;
    auto same = [](const Swatch &a, const Swatch &b) { return a.word == b.word && a.inner_word == b.inner_word && a.ratios == b.ratios; };
    for (const Swatch &o : old.swatches)
        if (std::none_of(updated.swatches.begin(), updated.swatches.end(), [&](const Swatch &n) { return same(o, n); })) {
            updated.swatches.push_back(o);
            if (kept)
                ++*kept;
        }
    return true;
}

std::string user_bundle_file_name(const Bundle &bundle)
{
    std::string n;
    for (char c : bundle.id)
        n += std::isalnum((unsigned char) c) || c == '-' || c == '_' ? c : '_';
    return n + ".json";
}

} // namespace Slic3r::ColorMix
