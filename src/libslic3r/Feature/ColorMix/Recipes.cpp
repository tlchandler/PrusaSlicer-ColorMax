///|/ ColorMax engine - layer words (recipes), striping rules and layer schedules.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "Recipes.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>

namespace Slic3r::ColorMix {

bool LayerSchedule::uniform() const
{
    return heights.size() <= 1 || std::all_of(heights.begin(), heights.end(), [this](double h) { return std::abs(h - heights.front()) < 1e-9; });
}

double LayerSchedule::height(size_t layer_idx) const
{
    if (layer_idx == 0)
        return first_layer_height;
    return heights.empty() ? 0.1 : heights[(layer_idx - 1) % heights.size()];
}

double LayerSchedule::average() const
{
    return heights.empty() ? 0.1 : std::accumulate(heights.begin(), heights.end(), 0.) / double(heights.size());
}

int circular_max_run(const Word &w)
{
    const int n = int(w.size());
    if (n == 0)
        return 0;
    if (std::all_of(w.begin(), w.end(), [&](int f) { return f == w.front(); }))
        return std::numeric_limits<int>::max();
    int best = 0;
    for (int start = 0; start < n; ++start) {
        if (w[start] == w[(start + n - 1) % n])
            continue; // not the start of a run
        int len = 1;
        while (len < n && w[(start + len) % n] == w[start])
            ++len;
        best = std::max(best, len);
    }
    return best;
}

bool satisfies_rules(const Word &w, const RecipeRules &rules)
{
    const int n = int(w.size());
    if (n == 0 || n > rules.max_word_length)
        return false;
    std::vector<int> used(w);
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    if (int(used.size()) > rules.max_filaments)
        return false;
    if (used.size() == 1)
        return n == 1;
    if (circular_max_run(w) > rules.max_run)
        return false;
    const int win = std::min(rules.recurrence_window, n);
    for (int start = 0; start < n; ++start)
        for (int f : used) {
            bool found = false;
            for (int k = 0; k < win && !found; ++k)
                found = w[(start + k) % n] == f;
            if (!found)
                return false;
        }
    return true;
}

std::vector<double> word_weights(const Word &w, size_t num_bundle_filaments, const LayerSchedule &schedule)
{
    std::vector<double> out(num_bundle_filaments, 0.);
    if (w.empty())
        return out;
    const size_t P      = std::max<size_t>(1, schedule.heights.size());
    const size_t n      = w.size();
    const size_t period = n / std::gcd(n, P) * P;
    double       total  = 0.;
    for (size_t L = 1; L <= period; ++L) {
        const double h = schedule.height(L);
        out[size_t(w[L % n])] += h;
        total += h;
    }
    for (double &v : out)
        v /= total;
    return out;
}

double banding_score(const Word &w, const LayerSchedule &schedule, const std::vector<Lab> &colors)
{
    if (w.size() <= 1)
        return 0.;
    const size_t P      = std::max<size_t>(1, schedule.heights.size());
    const size_t period = w.size() / std::gcd(w.size(), P) * P;
    // Lay out 5 periods, evaluate the middle one.
    std::vector<double> zc;
    std::vector<Oklab>  col;
    double              z = 0.;
    for (size_t k = 0; k < 5 * period; ++k) {
        const size_t L = 1 + k;
        const double h = schedule.height(L);
        zc.push_back(z + 0.5 * h);
        col.push_back(lab_to_oklab(colors[size_t(w[L % w.size()])]));
        z += h;
    }
    // Mean color of one period (height weighted).
    Oklab  mean{};
    double tot = 0.;
    for (size_t k = 2 * period; k < 3 * period; ++k) {
        const double h = schedule.height(1 + k);
        mean.L += col[k].L * h; mean.a += col[k].a * h; mean.b += col[k].b * h;
        tot += h;
    }
    mean.L /= tot; mean.a /= tot; mean.b /= tot;
    double score = 0.;
    for (double sigma : {0.2, 0.4, 0.8}) {
        double dev = 0.;
        for (size_t k = 2 * period; k < 3 * period; ++k) {
            Oklab  s{};
            double ws = 0.;
            for (size_t j = 0; j < col.size(); ++j) {
                const double d  = (zc[j] - zc[k]) / sigma;
                const double wt = std::exp(-0.5 * d * d) * schedule.height(1 + j);
                s.L += col[j].L * wt; s.a += col[j].a * wt; s.b += col[j].b * wt;
                ws += wt;
            }
            s.L /= ws; s.a /= ws; s.b /= ws;
            dev = std::max(dev, 100. * std::sqrt(4. * (s.L - mean.L) * (s.L - mean.L) + (s.a - mean.a) * (s.a - mean.a)
                                                 + (s.b - mean.b) * (s.b - mean.b)));
        }
        score += dev / 3.;
    }
    return score;
}

Word even_word(const std::vector<std::pair<int, int>> &filament_counts)
{
    int total = 0;
    for (const auto &fc : filament_counts)
        total += std::max(0, fc.second);
    Word             out;
    std::vector<int> emitted(filament_counts.size(), 0);
    for (int slot = 0; slot < total; ++slot) {
        size_t best = 0;
        double best_def = -1e9;
        for (size_t i = 0; i < filament_counts.size(); ++i) {
            if (filament_counts[i].second <= 0)
                continue;
            const double def = double((slot + 1) * filament_counts[i].second) / double(total) - emitted[i];
            if (def > best_def) {
                best_def = def;
                best     = i;
            }
        }
        ++emitted[best];
        out.push_back(filament_counts[best].first);
    }
    return out;
}

namespace {

bool is_primitive(const Word &w)
{
    const size_t n = w.size();
    for (size_t p = 1; p < n; ++p) {
        if (n % p)
            continue;
        bool rep = true;
        for (size_t i = p; i < n && rep; ++i)
            rep = w[i] == w[i - p];
        if (rep)
            return false;
    }
    return true;
}

Word min_rotation(const Word &w)
{
    Word best = w, r = w;
    for (size_t i = 1; i < w.size(); ++i) {
        std::rotate(r.begin(), r.begin() + 1, r.end());
        if (r < best)
            best = r;
    }
    return best;
}

void compositions(int n, int k, std::vector<int> &cur, std::vector<std::vector<int>> &out)
{
    if (k == 1) {
        cur.push_back(n);
        out.push_back(cur);
        cur.pop_back();
        return;
    }
    for (int first = 1; first <= n - (k - 1); ++first) {
        cur.push_back(first);
        compositions(n - first, k - 1, cur, out);
        cur.pop_back();
    }
}

} // namespace

std::vector<Recipe> enumerate_recipes(const std::vector<int> &filaments, size_t nbf, const RecipeRules &rules,
                                      const LayerSchedule &schedule, const std::vector<Lab> &colors)
{
    constexpr int FULL_SEARCH_MAX_LEN = 8;
    const bool    uniform             = schedule.uniform();
    std::vector<Word> words;

    for (int f : filaments)
        words.push_back({f});

    const int kmax = std::min<int>(rules.max_filaments, int(filaments.size()));
    std::vector<int> subset;
    std::function<void(size_t)> each_subset = [&](size_t start) {
        if (subset.size() >= 2) {
            const int k = int(subset.size());
            for (int n = k; n <= rules.max_word_length; ++n) {
                if (n <= FULL_SEARCH_MAX_LEN) {
                    Word w(size_t(n), 0);
                    // DFS with a linear run pruning; circular rules checked at the end.
                    std::function<void(int, int)> dfs = [&](int pos, int run) {
                        if (pos == n) {
                            std::vector<int> u(w);
                            std::sort(u.begin(), u.end());
                            if (int(std::unique(u.begin(), u.end()) - u.begin()) == k && is_primitive(w) && satisfies_rules(w, rules))
                                words.push_back(uniform ? min_rotation(w) : w);
                            return;
                        }
                        for (int f : subset) {
                            const int r = (pos > 0 && w[pos - 1] == f) ? run + 1 : 1;
                            if (r > rules.max_run)
                                continue;
                            w[pos] = f;
                            dfs(pos + 1, r);
                        }
                    };
                    dfs(0, 0);
                } else {
                    std::vector<std::vector<int>> comps;
                    std::vector<int>              cur;
                    compositions(n, k, cur, comps);
                    for (const std::vector<int> &c : comps) {
                        std::vector<std::pair<int, int>> fc;
                        for (int i = 0; i < k; ++i)
                            fc.emplace_back(subset[i], c[i]);
                        Word w = even_word(fc);
                        if (!is_primitive(w) || !satisfies_rules(w, rules))
                            continue;
                        if (uniform)
                            words.push_back(min_rotation(w));
                        else {
                            for (size_t r = 0; r < w.size(); ++r) {
                                words.push_back(w);
                                std::rotate(w.begin(), w.begin() + 1, w.end());
                            }
                        }
                    }
                }
            }
        }
        if (int(subset.size()) == kmax)
            return;
        for (size_t i = start; i < filaments.size(); ++i) {
            subset.push_back(filaments[i]);
            each_subset(i + 1);
            subset.pop_back();
        }
    };
    each_subset(0);

    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());

    // Merge recipes with identical weights, keep the least banded (then shortest) word.
    std::map<std::vector<long>, Recipe> by_weights;
    for (const Word &w : words) {
        Recipe r;
        r.word          = w;
        r.weights       = word_weights(w, nbf, schedule);
        r.num_filaments = int(std::count_if(r.weights.begin(), r.weights.end(), [](double v) { return v > 0.; }));
        r.banding       = banding_score(w, schedule, colors);
        std::vector<long> key;
        for (double v : r.weights)
            key.push_back(std::lround(v * 1e4));
        auto it = by_weights.find(key);
        if (it == by_weights.end() || r.banding < it->second.banding - 1e-9
            || (std::abs(r.banding - it->second.banding) <= 1e-9 && r.word.size() < it->second.word.size()))
            by_weights[key] = std::move(r);
    }
    std::vector<Recipe> out;
    out.reserve(by_weights.size());
    for (auto &kv : by_weights)
        out.push_back(std::move(kv.second));
    std::sort(out.begin(), out.end(), [](const Recipe &a, const Recipe &b) {
        return a.num_filaments != b.num_filaments ? a.num_filaments < b.num_filaments : a.word < b.word;
    });
    return out;
}

} // namespace Slic3r::ColorMix
