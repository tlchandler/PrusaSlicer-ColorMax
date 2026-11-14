///|/ ColorMax engine - choose a printable recipe for every target color.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "Converter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <numeric>
#include <set>

namespace Slic3r::ColorMix {

namespace {
constexpr double MISSING_DE     = 100.;
constexpr double DISTINCT_MIN   = 3.;
constexpr double DISTINCT_KEEP  = 6.;
constexpr double DISTINCT_W     = 1.;
constexpr double EPS            = 1e-9;
constexpr double PI             = 3.14159265358979323846;
using Clock = std::chrono::steady_clock;
} // namespace

std::string color_family(const Lab &lab)
{
    const Oklab  o = lab_to_oklab(lab);
    const double L = o.L, C = std::hypot(o.a, o.b);
    double       h = std::atan2(o.b, o.a) * 180. / PI;
    if (h < 0.)
        h += 360.;
    if (C < 0.035)
        return L < 0.3 ? "black" : L > 0.9 ? "white" : "grey";
    if (h >= 330. || h < 20.)
        return L >= 0.7 ? "pink" : "red";
    if (h < 40.)
        return L < 0.65 ? "red" : C >= 0.1 ? "orange" : "pink";
    if (h < 100.) {
        if (L < 0.6 || C < 0.08)
            return "brown";
        return h < 75. ? "orange" : "yellow";
    }
    if (h < 115.)
        return L >= 0.6 ? "yellow" : "brown";
    if (h < 195.)
        return "green";
    if (h < 275.)
        return "blue";
    return "purple";
}

double family_penalty(const std::string &t, const std::string &g, double de)
{
    static const std::set<std::pair<std::string, std::string>> related = [] {
        std::set<std::pair<std::string, std::string>> s;
        const char *pairs[][2] = {{"black", "grey"}, {"grey", "white"}, {"black", "brown"}, {"brown", "orange"},
                                  {"brown", "red"}, {"brown", "yellow"}, {"red", "orange"}, {"red", "pink"},
                                  {"orange", "yellow"}, {"yellow", "green"}, {"green", "blue"}, {"blue", "purple"},
                                  {"purple", "pink"}, {"white", "yellow"}, {"grey", "brown"}};
        for (auto &p : pairs) {
            s.insert({p[0], p[1]});
            s.insert({p[1], p[0]});
        }
        return s;
    }();
    if (t == g)
        return 0.;
    const double full = related.count({t, g}) ? 4. : 10.;
    return full * std::clamp((de - 3.) / (10. - 3.), 0., 1.);
}

std::vector<double> area_weights(const std::vector<double> &areas)
{
    const double total = std::accumulate(areas.begin(), areas.end(), 0.);
    std::vector<double> out(areas.size(), 1.);
    if (total <= 0. || areas.empty())
        return out;
    double mean = 0.;
    for (size_t i = 0; i < areas.size(); ++i) {
        out[i] = std::sqrt(std::max(areas[i] / total, 0.002));
        mean += out[i] / double(areas.size());
    }
    for (double &w : out)
        w /= mean;
    return out;
}

std::string recipe_name(const Recipe &r, const Bundle &bundle)
{
    auto short_name = [&](int f) {
        std::string n = bundle.filaments[size_t(f)].name;
        const std::string pre = "ColorMix ";
        if (n.rfind(pre, 0) == 0)
            n = n.substr(pre.size());
        return n;
    };
    if (r.dual()) {
        Recipe outer = r, inner = r;
        outer.inner_word.clear();
        inner.word    = r.inner_word;
        inner.weights = r.inner_weights;
        inner.inner_word.clear();
        return "outer " + recipe_name(outer, bundle) + "  |  inner " + recipe_name(inner, bundle);
    }
    if (r.word.size() == 1)
        return bundle.filaments[size_t(r.word.front())].name;
    std::vector<std::pair<double, int>> parts;
    for (size_t i = 0; i < r.weights.size(); ++i)
        if (r.weights[i] > 0.)
            parts.emplace_back(-r.weights[i], int(i));
    std::sort(parts.begin(), parts.end());
    std::string s;
    char        buf[64];
    for (const auto &[w, f] : parts) {
        std::snprintf(buf, sizeof(buf), "%s%s %d%%", s.empty() ? "" : " + ", short_name(f).c_str(), int(std::lround(-w * 100.)));
        s += buf;
    }
    s += "  [";
    for (size_t i = 0; i < r.word.size(); ++i) {
        const std::string n = short_name(r.word[i]);
        s += (i ? " " : "") + n.substr(0, std::min<size_t>(2, n.size()));
    }
    s += "]";
    return s;
}

// ---- Tool-change model ----
namespace {

struct LayerGrid
{
    std::vector<double> top;   // print_z of each layer
    explicit LayerGrid(const LayerSchedule &s, double zmax)
    {
        double z = s.first_layer_height;
        for (size_t L = 0; z < zmax + 1e-6 || L == 0; ++L) {
            top.push_back(z);
            z += s.height(L + 1);
            if (top.size() > 200000)
                break;
        }
    }
    size_t size() const { return top.size(); }
    // Layers whose slab (top[L-1], top[L]] overlaps [z0, z1].
    std::pair<int, int> span(double z0, double z1) const
    {
        const int l0 = int(std::upper_bound(top.begin(), top.end(), z0 + 1e-6) - top.begin());
        int       l1 = int(std::lower_bound(top.begin(), top.end(), z1 - 1e-6) - top.begin());
        l1           = std::min(l1, int(top.size()) - 1);
        return {std::min(l0, int(top.size()) - 1), l1};
    }
};

std::vector<std::vector<std::pair<int, int>>> to_layers(const std::vector<std::pair<double, double>> &ranges, const LayerGrid &grid, int extend_down)
{
    std::vector<std::pair<int, int>> spans;
    for (const auto &[z0, z1] : ranges) {
        auto sp = grid.span(z0, z1);
        sp.first = std::max(0, sp.first - extend_down);
        if (sp.first <= sp.second)
            spans.push_back(sp);
    }
    std::sort(spans.begin(), spans.end());
    std::vector<std::vector<std::pair<int, int>>> out(1);
    for (const auto &sp : spans) {
        if (!out[0].empty() && sp.first <= out[0].back().second + 1)
            out[0].back().second = std::max(out[0].back().second, sp.second);
        else
            out[0].push_back(sp);
    }
    return out;
}

// Per target: layers near its up-facing surfaces (top patches), as a mask over the grid.
std::vector<std::vector<uint8_t>> target_top_masks(const std::vector<Target> &targets, const LayerGrid &grid, int top_layers)
{
    std::vector<std::vector<uint8_t>> out(targets.size());
    if (top_layers <= 0)
        return out;
    for (size_t k = 0; k < targets.size(); ++k) {
        if (targets[k].top_z_ranges.empty())
            continue;
        out[k].assign(grid.size(), 0);
        for (const auto &[l0, l1] : to_layers(targets[k].top_z_ranges, grid, top_layers - 1)[0])
            for (int L = l0; L <= l1; ++L)
                out[k][size_t(L)] = 1;
    }
    return out;
}

std::vector<std::vector<std::pair<int, int>>> target_layers(const std::vector<Target> &targets, const LayerGrid &grid)
{
    std::vector<std::vector<std::pair<int, int>>> out(targets.size());
    for (size_t k = 0; k < targets.size(); ++k)
        for (const auto &[z0, z1] : targets[k].z_ranges) {
            const auto sp = grid.span(z0, z1);
            if (sp.first > sp.second)
                continue;
            if (!out[k].empty() && sp.first <= out[k].back().second + 1)
                out[k].back().second = std::max(out[k].back().second, sp.second);
            else
                out[k].push_back(sp);
        }
    return out;
}

// Tools (1-based) a recipe uses on layer L: its outer and inner word's tools, or (`all`, under top
// patches) every tool of the recipe. At most 8.
inline int recipe_tools(const Recipe &r, int L, const std::vector<unsigned int> &tool_of_filament, unsigned int out[8], bool all = false)
{
    auto tool = [&](int f) { return size_t(f) < tool_of_filament.size() ? tool_of_filament[size_t(f)] : 0u; };
    int n = 0;
    if (all && (r.word.size() > 1 || !r.inner_word.empty())) {
        auto put = [&](int f) {
            const unsigned int t = tool(f);
            if (t > 0 && n < 8 && std::find(out, out + n, t) == out + n)
                out[n++] = t;
        };
        for (int f : r.word)
            put(f);
        for (int f : r.inner_word)
            put(f);
        return n;
    }
    if (!r.word.empty())
        if (const unsigned int t = tool(r.word[size_t(L) % r.word.size()]); t > 0)
            out[n++] = t;
    if (!r.inner_word.empty())
        if (const unsigned int t = tool(r.inner_word[size_t(L) % r.inner_word.size()]); t > 0 && (n == 0 || out[0] != t))
            out[n++] = t;
    return n;
}

double max_z(const std::vector<Target> &targets)
{
    double z = 0.;
    for (const Target &t : targets)
        for (const auto &r : t.z_ranges)
            z = std::max(z, r.second);
    return z;
}

// Rotations of a recipe's words that keep its weights (so its predicted color).
std::vector<Recipe> weight_keeping_rotations(const Recipe &r, size_t nbf, const LayerSchedule &schedule)
{
    std::vector<Recipe> out;
    if (r.word.size() <= 1 && r.inner_word.size() <= 1)
        return out;
    auto same = [](const std::vector<double> &a, const std::vector<double> &b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(a[i] - b[i]) > 1e-9)
                return false;
        return true;
    };
    auto rotate = [](const Word &w, size_t k) {
        Word o(w.size());
        for (size_t i = 0; i < w.size(); ++i)
            o[i] = w[(i + k) % w.size()];
        return o;
    };
    std::vector<Word> outers, inners;
    for (size_t k = 0; k < std::max<size_t>(1, r.word.size()); ++k) {
        const Word w = rotate(r.word, k);
        if (std::find(outers.begin(), outers.end(), w) == outers.end() && (k == 0 || same(word_weights(w, nbf, schedule), r.weights)))
            outers.push_back(w);
    }
    if (r.dual())
        for (size_t k = 0; k < r.inner_word.size(); ++k) {
            const Word w = rotate(r.inner_word, k);
            if (std::find(inners.begin(), inners.end(), w) == inners.end() && (k == 0 || same(word_weights(w, nbf, schedule), r.inner_weights)))
                inners.push_back(w);
        }
    else
        inners.push_back({});
    for (const Word &o : outers)
        for (const Word &in : inners) {
            if (o == r.word && in == r.inner_word)
                continue;
            Recipe v     = r;
            v.word       = o;
            v.inner_word = in;
            out.push_back(std::move(v));
        }
    return out;
}

} // namespace

std::vector<std::set<unsigned int>> estimate_tools_per_layer(const std::vector<Target> &targets, const std::vector<Candidate> &candidates,
                                                             const std::vector<int> &choice, const std::vector<unsigned int> &tool_of_filament,
                                                             const LayerSchedule &schedule, unsigned int core_tool, int top_layers)
{
    const double zmax = max_z(targets);
    if (zmax <= 0.)
        return {};
    const LayerGrid grid(schedule, zmax);
    const auto      layers = target_layers(targets, grid);
    const auto      tops   = target_top_masks(targets, grid, top_layers);
    std::vector<std::set<unsigned int>> used(grid.size());
    for (size_t k = 0; k < targets.size() && k < choice.size(); ++k) {
        if (choice[k] < 0)
            continue;
        const Recipe &r = candidates[size_t(choice[k])].recipe;
        for (const auto &[l0, l1] : layers[k])
            for (int L = l0; L <= l1; ++L) {
                unsigned int t[8];
                const int    n = recipe_tools(r, L, tool_of_filament, t, !tops[k].empty() && tops[k][size_t(L)]);
                for (int i = 0; i < n; ++i)
                    used[size_t(L)].insert(t[i]);
            }
    }
    if (core_tool > 0)
        for (std::set<unsigned int> &u : used)
            if (!u.empty())
                u.insert(core_tool);
    return used;
}

int estimate_tool_changes(const std::vector<Target> &targets, const std::vector<Candidate> &candidates, const std::vector<int> &choice,
                          const std::vector<unsigned int> &tool_of_filament, const LayerSchedule &schedule, unsigned int core_tool,
                          int top_layers)
{
    const double zmax = max_z(targets);
    if (zmax <= 0.)
        return -1;
    long changes = 0;
    for (const std::set<unsigned int> &u : estimate_tools_per_layer(targets, candidates, choice, tool_of_filament, schedule, core_tool, top_layers))
        if (!u.empty())
            changes += long(u.size()) - 1;
    return int(changes);
}

ConversionResult convert_colors(const std::vector<Target> &targets, const Bundle &bundle, const Predictor &predictor,
                                const std::vector<unsigned int> &tool_of_filament, const ConvertOptions &opt)
{
    ConversionResult res;
    const size_t     nbf = bundle.filaments.size();
    const auto       loaded = [&](int f) { return size_t(f) < tool_of_filament.size() && tool_of_filament[size_t(f)] > 0; };

    // ---- Candidates ----
    std::vector<int> mixable;
    for (int f : bundle.mixable_indices())
        if (loaded(f))
            mixable.push_back(f);
    for (size_t f = 0; f < nbf; ++f) {
        if (!loaded(int(f)))
            continue;
        // Accents (glitter) are only used where asked for; solid-only filaments print on their own.
        if (bundle.filaments[f].role == MixRole::Accent || (bundle.filaments[f].role == MixRole::Solid && !opt.effect_solids))
            continue;
        if (std::find(mixable.begin(), mixable.end(), int(f)) != mixable.end())
            continue; // added by enumerate_recipes below
        Recipe r;
        r.word          = {int(f)};
        r.weights.assign(nbf, 0.);
        r.weights[f]    = 1.;
        r.num_filaments = 1;
        res.candidates.push_back({r, predictor.predict(r), recipe_name(r, bundle)});
    }
    if (!mixable.empty()) {
        RecipeRules rules = bundle.rules;
        if (!opt.use_blends)
            rules.max_filaments = 1;
        const std::vector<Recipe> singles = enumerate_recipes(mixable, nbf, rules, opt.schedule, predictor.filament_colors());
        for (const Recipe &r : singles)
            res.candidates.push_back({r, predictor.predict(r), recipe_name(r, bundle)});
        if (opt.dual_walls && opt.use_blends) {
            RecipeRules inner_rules       = bundle.rules;
            inner_rules.max_run           = bundle.rules.max_run + 1;
            inner_rules.recurrence_window = bundle.rules.recurrence_window + 1;
            inner_rules.max_word_length   = std::min(4, bundle.rules.max_word_length);
            inner_rules.max_filaments     = std::min(2, bundle.rules.max_filaments);
            const std::vector<Recipe> inners = enumerate_recipes(mixable, nbf, inner_rules, opt.schedule, predictor.filament_colors());
            // Keep one pair per predicted color (1 dE buckets): the simplest one.
            std::map<std::array<long, 3>, Candidate> best;
            for (const Recipe &o : singles)
                for (const Recipe &in : inners) {
                    if (in.weights == o.weights)
                        continue; // same as the single-wall recipe
                    Recipe r        = o;
                    r.inner_word    = in.word;
                    r.inner_weights = in.weights;
                    std::set<int> used(o.word.begin(), o.word.end());
                    used.insert(in.word.begin(), in.word.end());
                    r.num_filaments = int(used.size());
                    if (r.num_filaments > bundle.rules.max_filaments + 1)
                        continue;
                    r.banding = o.banding + 0.25 * in.banding;
                    const Prediction p   = predictor.predict(r);
                    const std::array<long, 3> key = {std::lround(p.color.L), std::lround(p.color.a), std::lround(p.color.b)};
                    auto it = best.find(key);
                    const double cost = r.num_filaments + 0.1 * (r.word.size() + r.inner_word.size()) + r.banding;
                    if (it == best.end() || cost < it->second.recipe.num_filaments + 0.1 * (it->second.recipe.word.size() + it->second.recipe.inner_word.size()) + it->second.recipe.banding)
                        best[key] = {r, p, recipe_name(r, bundle)};
                }
            for (auto &[k, c] : best)
                res.candidates.push_back(std::move(c));
        }
    }
    if (res.candidates.empty()) {
        res.problem = "No filament of the bundle is loaded in any tool.";
        return res;
    }

    const size_t nt = targets.size(), nc = res.candidates.size();
    res.choice.assign(nt, -1);
    res.de.assign(nt, MISSING_DE);
    if (nt == 0)
        return res;

    // ---- Cost matrix ----
    std::vector<std::string> cand_family(nc);
    for (size_t i = 0; i < nc; ++i)
        cand_family[i] = color_family(res.candidates[i].prediction.color);
    std::vector<std::vector<double>> de(nt, std::vector<double>(nc)), eff(nt, std::vector<double>(nc));
    std::vector<double> W(nt);
    for (size_t k = 0; k < nt; ++k) {
        W[k] = targets[k].weight > 0. ? targets[k].weight : 1.;
        const std::string tf = color_family(targets[k].color);
        for (size_t i = 0; i < nc; ++i) {
            const Candidate &c = res.candidates[i];
            de[k][i]  = delta_e_2000(targets[k].color, c.prediction.color);
            double pen = 0.;
            if (!c.pure()) {
                if (!c.prediction.measured)
                    pen += opt.blend_penalty;
                pen += opt.banding_weight * c.recipe.banding;
                if (c.recipe.dual())
                    pen += opt.dual_penalty;
            }
            if (opt.match_families)
                pen += family_penalty(tf, cand_family[i], de[k][i]);
            eff[k][i] = de[k][i] + pen;
        }
    }
    // A model color that is a spool's own color prints with that spool, pure: that is what the designer
    // meant (e.g. lettering in the spools' colors), and it is the sharpest result with the fewest tool
    // changes. The catalog color counts as well as the predicted one: after calibration the measured
    // color of a spool differs from its catalog color, and a blend could otherwise land closer.
    if (opt.spool_snap_de > 0.)
        for (size_t k = 0; k < nt; ++k) {
            int    snap = -1;
            double best = opt.spool_snap_de;
            for (size_t i = 0; i < nc; ++i) {
                const Candidate &c = res.candidates[i];
                if (!c.pure())
                    continue;
                const int    f = c.recipe.word.front();
                const double d = std::min(de[k][i], size_t(f) < bundle.filaments.size() ?
                                                        delta_e_2000(targets[k].color, hex_to_lab(bundle.filaments[size_t(f)].color)) : 1e9);
                if (d < best) {
                    best = d;
                    snap = int(i);
                }
            }
            if (snap >= 0)
                for (size_t i = 0; i < nc; ++i)
                    eff[k][i] = int(i) == snap ? 0. : eff[k][i] + 1000.;
        }

    std::vector<std::vector<int>> ranked(nt);
    for (size_t k = 0; k < nt; ++k) {
        ranked[k].resize(nc);
        std::iota(ranked[k].begin(), ranked[k].end(), 0);
        std::sort(ranked[k].begin(), ranked[k].end(), [&](int a, int b) { return eff[k][size_t(a)] < eff[k][size_t(b)]; });
    }

    // Keep-distinct pairs.
    struct PairReq { size_t a, b; double want, w; };
    std::vector<PairReq> pairs;
    if (opt.keep_distinct)
        for (size_t a = 0; a < nt; ++a)
            for (size_t b = a + 1; b < nt; ++b) {
                const double d = delta_e_2000(targets[a].color, targets[b].color);
                if (d >= DISTINCT_MIN)
                    pairs.push_back({a, b, std::min(d, DISTINCT_KEEP), std::min(W[a], W[b]) * DISTINCT_W});
            }
    std::map<std::pair<int, int>, double> dist_cache;
    auto dist = [&](int i, int j) {
        const auto key = std::make_pair(std::min(i, j), std::max(i, j));
        auto it = dist_cache.find(key);
        if (it != dist_cache.end())
            return it->second;
        const double d = delta_e_2000(res.candidates[size_t(i)].prediction.color, res.candidates[size_t(j)].prediction.color);
        dist_cache.emplace(key, d);
        return d;
    };
    std::vector<std::vector<std::pair<size_t, size_t>>> partner(nt); // (other target, pair index)
    for (size_t p = 0; p < pairs.size(); ++p) {
        partner[pairs[p].a].emplace_back(pairs[p].b, p);
        partner[pairs[p].b].emplace_back(pairs[p].a, p);
    }

    std::vector<int> pick(nt);
    for (size_t k = 0; k < nt; ++k)
        pick[k] = ranked[k].front();

    auto total_cost = [&](const std::vector<int> &pk) {
        double v = 0.;
        for (size_t k = 0; k < nt; ++k)
            v += W[k] * eff[k][size_t(pk[k])];
        for (const PairReq &p : pairs)
            v += p.w * std::max(0., p.want - dist(pk[p.a], pk[p.b]));
        return v;
    };

    if (!pairs.empty()) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(int(opt.time_limit_s * 1000.));
        constexpr size_t TOPN = 40;
        // Coordinate descent from the separable choice.
        for (int iter = 0; iter < 25; ++iter) {
            bool changed = false;
            for (size_t k = 0; k < nt; ++k) {
                auto val = [&](int i) {
                    double v = W[k] * eff[k][size_t(i)];
                    for (const auto &[o, p] : partner[k])
                        v += pairs[p].w * std::max(0., pairs[p].want - dist(i, pick[o]));
                    return v;
                };
                const double now = val(pick[k]);
                int          best = pick[k];
                double       bv   = now;
                for (size_t r = 0; r < std::min(TOPN, ranked[k].size()); ++r) {
                    const double v = val(ranked[k][r]);
                    if (v < bv - EPS) {
                        bv   = v;
                        best = ranked[k][r];
                    }
                }
                if (best != pick[k]) {
                    pick[k] = best;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
        // Exact branch and bound, heaviest colors first.
        std::vector<size_t> order(nt);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return W[a] > W[b]; });
        std::vector<double> rest(nt + 1, 0.);
        for (int x = int(nt) - 1; x >= 0; --x)
            rest[size_t(x)] = rest[size_t(x) + 1] + W[order[size_t(x)]] * eff[order[size_t(x)]][size_t(ranked[order[size_t(x)]].front())];
        double           best = total_cost(pick);
        std::vector<int> best_pick = pick, assigned(nt, -1);
        bool             timed_out = false;
        std::function<void(size_t, double)> dfs = [&](size_t x, double partial) {
            if (x == nt) {
                if (partial < best - EPS) {
                    best      = partial;
                    best_pick = assigned;
                }
                return;
            }
            const size_t k = order[x];
            for (int i : ranked[k]) {
                double v = partial + W[k] * eff[k][size_t(i)];
                if (v + rest[x + 1] >= best - EPS)
                    break;
                for (const auto &[o, p] : partner[k])
                    if (assigned[o] >= 0)
                        v += pairs[p].w * std::max(0., pairs[p].want - dist(i, assigned[o]));
                if (v + rest[x + 1] >= best - EPS)
                    continue;
                assigned[k] = i;
                dfs(x + 1, v);
                assigned[k] = -1;
                if (Clock::now() > deadline) {
                    timed_out = true;
                    return;
                }
            }
        };
        dfs(0, 0.);
        pick        = best_pick;
        res.optimal = !timed_out;
    }

    // ---- Respect the number of painting states available for virtual extruders ----
    for (;;) {
        std::map<int, double> use;
        for (size_t k = 0; k < nt; ++k)
            if (!res.candidates[size_t(pick[k])].pure())
                use[pick[k]] += W[k];
        if (int(use.size()) <= opt.max_virtual)
            break;
        // Drop the least important blend; its colors move to their next best allowed candidate.
        const int drop = std::min_element(use.begin(), use.end(), [](auto &a, auto &b) { return a.second < b.second; })->first;
        for (size_t k = 0; k < nt; ++k)
            if (pick[k] == drop)
                for (int i : ranked[k])
                    if (i != drop && (res.candidates[size_t(i)].pure() || use.count(i))) {
                        pick[k] = i;
                        break;
                    }
        res.optimal = false;
    }

    // Candidates appended below (word rotations) share the cost row of the candidate they rotate.
    std::vector<int> base_of(nc);
    std::iota(base_of.begin(), base_of.end(), 0);
    auto base_idx = [&](int i) { return base_of[size_t(i)]; };

    // ---- Merge recipes whose predictions are practically identical ----
    if (opt.merge_de > 0.) {
        std::map<int, double> use;
        for (size_t k = 0; k < nt; ++k)
            use[pick[k]] += W[k];
        std::vector<int> order;
        for (const auto &[i, w] : use)
            order.push_back(i);
        // Pure filaments first (no tool changes of their own), then the most used.
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            const bool pa = res.candidates[size_t(a)].pure(), pb = res.candidates[size_t(b)].pure();
            return pa != pb ? pa : use[a] > use[b];
        });
        std::map<int, int> into;
        for (size_t x = 0; x < order.size(); ++x) {
            if (into.count(order[x]))
                continue;
            for (size_t y = x + 1; y < order.size(); ++y)
                if (!into.count(order[y]) && dist(order[x], order[y]) < opt.merge_de)
                    into[order[y]] = order[x];
        }
        for (size_t k = 0; k < nt; ++k)
            if (auto it = into.find(pick[k]); it != into.end())
                pick[k] = it->second;
    }

    // ---- Tool changes ----
    if (const double zmax = max_z(targets); zmax > 0.) {
        const LayerGrid grid(opt.schedule, zmax);
        const auto      layers = target_layers(targets, grid);
        const auto      tops   = target_top_masks(targets, grid, opt.top_layers);
        auto is_top = [&](size_t k, int L) { return !tops[k].empty() && tops[k][size_t(L)] != 0; };
        const size_t    nl     = grid.size();
        unsigned int    max_tool = opt.core_tool;
        for (unsigned int t : tool_of_filament)
            max_tool = std::max(max_tool, t);
        const size_t stride = size_t(max_tool) + 1;
        std::vector<uint16_t> cnt(nl * stride, 0);
        std::vector<uint8_t>  any(nl, 0);
        for (size_t k = 0; k < nt; ++k)
            for (const auto &[l0, l1] : layers[k])
                for (int L = l0; L <= l1; ++L)
                    any[size_t(L)] = 1;
        auto add = [&](size_t k, const Recipe &r, int sign) {
            for (const auto &[l0, l1] : layers[k])
                for (int L = l0; L <= l1; ++L) {
                    unsigned int t[8];
                    const int    n = recipe_tools(r, L, tool_of_filament, t, is_top(k, L));
                    for (int i = 0; i < n; ++i)
                        cnt[size_t(L) * stride + t[i]] = uint16_t(int(cnt[size_t(L) * stride + t[i]]) + sign);
                }
        };
        if (opt.core_tool > 0)
            for (size_t L = 0; L < nl; ++L)
                if (any[L])
                    ++cnt[L * stride + opt.core_tool];
        for (size_t k = 0; k < nt; ++k)
            add(k, res.candidates[size_t(pick[k])].recipe, +1);
        auto changes = [&]() {
            long c = 0;
            for (size_t L = 0; L < nl; ++L) {
                int n = 0;
                for (size_t t = 1; t < stride; ++t)
                    n += cnt[L * stride + t] > 0;
                c += std::max(0, n - 1);
            }
            return int(c);
        };
        res.tool_changes_before = changes();
        // Change in the number of (layer, tool) uses when target k switches from recipe a to b.
        auto delta_uses = [&](size_t k, const Recipe &a, const Recipe &b) {
            int d = 0;
            for (const auto &[l0, l1] : layers[k])
                for (int L = l0; L <= l1; ++L) {
                    unsigned int ta[8], tb[8];
                    const bool   top = is_top(k, L);
                    const int    na = recipe_tools(a, L, tool_of_filament, ta, top), nb = recipe_tools(b, L, tool_of_filament, tb, top);
                    for (int i = 0; i < na; ++i)
                        if (std::find(tb, tb + nb, ta[i]) == tb + nb && cnt[size_t(L) * stride + ta[i]] == 1)
                            --d;
                    for (int i = 0; i < nb; ++i)
                        if (std::find(ta, ta + na, tb[i]) == ta + na && cnt[size_t(L) * stride + tb[i]] == 0)
                            ++d;
                }
            return d;
        };
        if (opt.change_weight > 0.) {
            double wsum_all = 0.;
            for (size_t k = 0; k < nt; ++k)
                wsum_all += W[k];
            size_t used_layers = 0;
            for (size_t L = 0; L < nl; ++L)
                used_layers += any[L];
            // dE (weighted mean) per tool change saved on every layer -> cost per single change.
            const double lambda = opt.change_weight * wsum_all / double(std::max<size_t>(1, used_layers));
            std::map<int, std::vector<Recipe>> rotations;
            auto rotations_of = [&](int i) -> const std::vector<Recipe> & {
                auto it = rotations.find(i);
                if (it == rotations.end())
                    it = rotations.emplace(i, weight_keeping_rotations(res.candidates[size_t(i)].recipe, nbf, opt.schedule)).first;
                return it->second;
            };
            std::map<int, int> blend_use;
            for (size_t k = 0; k < nt; ++k)
                if (!res.candidates[size_t(pick[k])].pure())
                    ++blend_use[pick[k]];
            std::vector<size_t> order(nt);
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return W[a] > W[b]; });
            constexpr size_t TOPN = 16;
            for (int sweep = 0; sweep < 8; ++sweep) {
                bool changed = false;
                for (size_t k : order) {
                    if (layers[k].empty())
                        continue;
                    const int     cur  = pick[k];
                    const Recipe  rcur = res.candidates[size_t(cur)].recipe;   // copy: candidates may grow
                    auto color_cost = [&](int i) {
                        double v = W[k] * eff[k][size_t(base_idx(i))];
                        for (const auto &[o, p] : partner[k])
                            v += pairs[p].w * std::max(0., pairs[p].want - dist(i, pick[o]));
                        return v;
                    };
                    const double now = color_cost(cur);
                    double       best_gain = 1e-9;
                    int          best_i    = -1;
                    const Recipe *best_r   = nullptr;
                    // Candidate index a recipe would get: an appended rotation with these words, or -1 if new.
                    auto index_of = [&](int base, const Recipe &r) {
                        const Recipe &b = res.candidates[size_t(base)].recipe;
                        if (r.word == b.word && r.inner_word == b.inner_word)
                            return base;
                        for (size_t c = nc; c < res.candidates.size(); ++c)
                            if (base_of[c] == base && res.candidates[c].recipe.word == r.word && res.candidates[c].recipe.inner_word == r.inner_word)
                                return int(c);
                        return -1;
                    };
                    auto consider = [&](int i, const Recipe &r) {
                        const int  idx       = index_of(i, r);
                        const bool new_blend = !res.candidates[size_t(i)].pure() && (idx < 0 || !blend_use.count(idx));
                        if (new_blend && int(blend_use.size()) >= opt.max_virtual)
                            return;
                        const double gain = now - color_cost(i) - lambda * delta_uses(k, rcur, r);
                        if (gain > best_gain) {
                            best_gain = gain;
                            best_i    = i;
                            best_r    = &r;
                        }
                    };
                    std::vector<int> options = {cur};
                    for (size_t x = 0; x < std::min(TOPN, ranked[k].size()); ++x)
                        if (ranked[k][x] != base_idx(cur))
                            options.push_back(ranked[k][x]);
                    // A target on a rotation may also go back to the base word.
                    if (cur != base_idx(cur))
                        consider(base_idx(cur), res.candidates[size_t(base_idx(cur))].recipe);
                    for (int i : options) {
                        if (i != cur)
                            consider(i, res.candidates[size_t(i)].recipe);
                        for (const Recipe &v : rotations_of(base_idx(i)))
                            if (!(v.word == rcur.word && v.inner_word == rcur.inner_word))
                                consider(base_idx(i), v);   // index of the base; the rotation is in best_r
                    }
                    if (best_i < 0)
                        continue;
                    int chosen = best_i;
                    const Recipe &rb = *best_r;
                    if (!(rb.word == res.candidates[size_t(best_i)].recipe.word && rb.inner_word == res.candidates[size_t(best_i)].recipe.inner_word)) {
                        // A rotation: reuse an appended candidate with these words, or append one.
                        chosen = -1;
                        for (size_t c = nc; c < res.candidates.size(); ++c)
                            if (base_of[c] == best_i && res.candidates[c].recipe.word == rb.word && res.candidates[c].recipe.inner_word == rb.inner_word)
                                chosen = int(c);
                        if (chosen < 0) {
                            Candidate c = res.candidates[size_t(best_i)];
                            c.recipe    = rb;
                            c.name      = recipe_name(rb, bundle);
                            res.candidates.push_back(std::move(c));
                            base_of.push_back(best_i);
                            chosen = int(res.candidates.size()) - 1;
                        }
                    }
                    add(k, rcur, -1);
                    add(k, res.candidates[size_t(chosen)].recipe, +1);
                    if (!res.candidates[size_t(cur)].pure() && --blend_use[cur] == 0)
                        blend_use.erase(cur);
                    if (!res.candidates[size_t(chosen)].pure())
                        ++blend_use[chosen];
                    pick[k] = chosen;
                    changed = true;
                }
                if (!changed)
                    break;
            }
        }
        res.tool_changes = changes();
    }

    std::set<int> blends;
    double        wsum = 0., wde = 0.;
    for (size_t k = 0; k < nt; ++k) {
        res.choice[k] = pick[k];
        res.de[k]     = de[k][size_t(base_idx(pick[k]))];
        wsum += W[k];
        wde += W[k] * res.de[k];
        res.worst_de = std::max(res.worst_de, res.de[k]);
        if (!res.candidates[size_t(pick[k])].pure())
            blends.insert(pick[k]);
    }
    res.blends_used      = int(blends.size());
    res.weighted_mean_de = wsum > 0. ? wde / wsum : 0.;
    return res;
}

} // namespace Slic3r::ColorMix
