///|/ ColorMax engine - slicing techniques applied to the multi-material segmentation.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "SliceTechniques.hpp"
#include "Predictor.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <numeric>

#include <tbb/parallel_for.h>

#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r::ColorMix {

namespace {
// Inset of the top patch zone from the layer outline, so walls keep their layer word.
constexpr double TOP_WALL_INSET_MM   = 0.45;
// Patch fragments smaller than this go to the recipe's main filament.
constexpr double MIN_PATCH_AREA_MM2  = 0.3;

double ex_area(const ExPolygons &e)
{
    double a = 0.;
    for (const ExPolygon &p : e)
        a += p.area();
    return a;
}
} // namespace

int bayer8(int x, int y)
{
    // B_{2n} = [[4B, 4B+2], [4B+3, 4B+1]], starting from [0].
    static const int M[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
                                {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
                                {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
                                {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};
    return M[((y % 8) + 8) % 8][((x % 8) + 8) % 8];
}

unsigned int bayer_pick(const std::vector<std::pair<unsigned int, double>> &shares, int threshold)
{
    if (shares.empty())
        return 0;
    double total = 0.;
    for (const auto &s : shares)
        total += s.second;
    // Largest-remainder rounding to 64 units.
    std::vector<int>    units(shares.size());
    std::vector<double> rem(shares.size());
    int                 sum = 0;
    for (size_t i = 0; i < shares.size(); ++i) {
        const double u = 64. * shares[i].second / total;
        units[i]       = int(std::floor(u));
        rem[i]         = u - units[i];
        sum += units[i];
    }
    while (sum < 64) {
        const size_t i = size_t(std::max_element(rem.begin(), rem.end()) - rem.begin());
        ++units[i];
        rem[i] = -1.;
        ++sum;
    }
    int cum = 0;
    for (size_t i = 0; i < shares.size(); ++i) {
        cum += units[i];
        if (threshold < cum)
            return shares[i].first;
    }
    return shares.back().first;
}

std::vector<std::pair<unsigned int, double>> virtual_extruder_shares(const FullSpectrum::VirtualExtruder &ve, const std::vector<double> &print_z)
{
    // Layer words: each tool's share of the wall height over the object's layers, as the predictor
    // weighs them (variable layer heights; dual walls: the outer wall's share of the color).
    if (!ve.gradient && !ve.sequence.empty() && print_z.size() > 1) {
        constexpr double OUTER = Predictor::DUAL_OUTER_SHARE;
        const bool dual = !ve.inner_sequence.empty();
        std::map<unsigned int, double> acc;
        for (size_t L = 1; L < print_z.size(); ++L) {
            const double h = print_z[L] - print_z[L - 1];
            acc[ve.sequence[L % ve.sequence.size()]] += h * (dual ? OUTER : 1.);
            if (dual)
                acc[ve.inner_sequence[L % ve.inner_sequence.size()]] += h * (1. - OUTER);
        }
        std::vector<std::pair<unsigned int, double>> out(acc.begin(), acc.end());
        std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        return out;
    }
    return virtual_extruder_shares(ve);
}

std::vector<std::pair<unsigned int, double>> virtual_extruder_shares(const FullSpectrum::VirtualExtruder &ve)
{
    std::map<unsigned int, double> acc;
    if (ve.gradient) {
        // Gradients vary along Z; use an even split of their stops on tops.
        for (const auto &s : ve.gradient->stops)
            acc[s.extruder_id] += 1.;
    } else if (!ve.sequence.empty()) {
        for (unsigned int e : ve.sequence)
            acc[e] += 1.;
    } else {
        for (const auto &c : ve.components)
            if (c.ratio > 0.)
                acc[c.extruder_id] += c.ratio;
    }
    std::vector<std::pair<unsigned int, double>> out(acc.begin(), acc.end());
    // Most used first, so leftovers and rounding favor the main filament.
    std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    return out;
}

void cleanup_regions(std::vector<std::vector<ExPolygons>> &seg,
                     const std::vector<ExPolygons>        &outlines,
                     const std::vector<double>            &print_z,
                     const SliceSettings                  &settings,
                     const std::function<void()>          &throw_on_cancel,
                     double                                first_layer_height)
{
    const size_t nl = seg.size();
    if (nl == 0 || outlines.size() != nl || print_z.size() != nl)
        return;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nl), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t L = range.begin(); L < range.end(); ++L)
            for (ExPolygons &r : seg[L])
                if (r.size() > 1)
                    r = union_ex(r);
    });
    auto slot_at = [&](size_t L, size_t slot) -> const ExPolygons * {
        static const ExPolygons none;
        return slot < seg[L].size() ? &seg[L][slot] : &none;
    };

    // ---- Border smoothing: majority of the 5 layers around each layer ----
    if (settings.smooth_borders && nl >= 3) {
        std::vector<std::vector<ExPolygons>> out(nl);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, nl), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t L = range.begin(); L < range.end(); ++L) {
                throw_on_cancel();
                ExPolygons painted;
                size_t     nslots = 0;
                const size_t lo = L >= 2 ? L - 2 : 0, hi = std::min(nl - 1, L + 2);
                for (size_t l = lo; l <= hi; ++l)
                    nslots = std::max(nslots, seg[l].size());
                for (const ExPolygons &r : seg[L])
                    append(painted, r);
                out[L].assign(seg[L].size(), ExPolygons());
                if (painted.empty())
                    continue;
                painted = union_ex(painted);
                const int need = int(hi - lo + 1) / 2 + 1;
                std::vector<ExPolygons> won(nslots);
                ExPolygons claimed, stable;
                for (size_t slot = 0; slot < nslots; ++slot) {
                    bool present = false;
                    for (size_t l = lo; l <= hi && !present; ++l)
                        present = !slot_at(l, slot)->empty();
                    if (!present)
                        continue;
                    // G[t]: covered by at least t of the window layers.
                    std::vector<Polygons> G(size_t(need) + 1);
                    int seen = 0;
                    for (size_t l = lo; l <= hi; ++l) {
                        const Polygons S = to_polygons(*slot_at(l, slot));
                        ++seen;
                        for (int t = std::min(need, seen); t >= 1; --t)
                            G[size_t(t)] = t == 1 ? union_(G[1], S) : union_(G[size_t(t)], intersection(G[size_t(t) - 1], S));
                    }
                    // A point of this layer keeps its color when one more layer of the window has it
                    // too (details two or more layers tall survive); only single-layer flicker goes.
                    if (slot < seg[L].size() && !seg[L][slot].empty())
                        append(stable, intersection_ex(seg[L][slot], G.size() > 2 ? G[2] : G[size_t(need)]));
                    if (G[size_t(need)].empty())
                        continue;
                    won[slot] = intersection_ex(G[size_t(need)], painted);
                }
                // Flicker (no support on a neighbouring layer) takes the majority color; points
                // without a majority, or stable ones, keep their color.
                stable = union_ex(stable);
                const ExPolygons flicker = diff_ex(painted, stable);
                for (size_t slot = 0; slot < nslots; ++slot)
                    if (!won[slot].empty()) {
                        won[slot] = intersection_ex(won[slot], flicker);
                        append(claimed, won[slot]);
                    }
                const ExPolygons rest = claimed.empty() ? painted : diff_ex(painted, union_ex(claimed));
                if (out[L].size() < nslots)
                    out[L].resize(nslots);
                for (size_t slot = 0; slot < nslots; ++slot) {
                    ExPolygons r = std::move(won[slot]);
                    if (slot < seg[L].size() && !seg[L][slot].empty() && !rest.empty())
                        append(r, intersection_ex(seg[L][slot], rest));
                    out[L][slot] = r.size() > 1 ? union_ex(r) : std::move(r);
                }
            }
        });
        seg.swap(out);
    }

    // ---- Short runs along the outline and small islands ----
    if (settings.min_run <= 0. && settings.min_island_area <= 0.)
        return;
    const float  eps     = scaled<float>(0.02);
    const double min_run = scaled<double>(std::max(0., settings.min_run));
    struct Comp
    {
        size_t L, slot, idx;
        double run;      // scaled length along the outline
        double exposed;  // mm^2 seen from above or below (not covered by the next / previous layer)
    };
    std::vector<std::vector<Comp>> comps(nl);
    std::vector<ExPolygons>        surface_band(nl);   // 0.5 mm deep band under the outline
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nl), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t L = range.begin(); L < range.end(); ++L) {
            throw_on_cancel();
            if (outlines[L].empty())
                continue;
            const ExPolygons edge = diff_ex(offset_ex(outlines[L], eps), offset_ex(outlines[L], -eps));
            surface_band[L]       = diff_ex(outlines[L], offset_ex(outlines[L], -scaled<float>(0.5)));
            for (size_t slot = 0; slot < seg[L].size(); ++slot)
                for (size_t i = 0; i < seg[L][slot].size(); ++i) {
                    double run = 0.;
                    for (const Polyline &pl : intersection_pl(to_polylines(seg[L][slot][i]), edge))
                        run += pl.length();
                    // Visible through a top or bottom face (the face on the bed is seen once printed).
                    const ExPolygons piece{seg[L][slot][i]};
                    double exposed = L + 1 < nl ? ex_area(diff_ex(piece, outlines[L + 1])) : ex_area(piece);
                    exposed += L > 0 ? ex_area(diff_ex(piece, outlines[L - 1])) : ex_area(piece);
                    comps[L].push_back({L, slot, i, run, exposed * SCALING_FACTOR * SCALING_FACTOR});
                }
        }
    });
    // Islands: components of one slot overlapping on neighbouring layers.
    std::vector<size_t> base(nl + 1, 0);
    for (size_t L = 0; L < nl; ++L)
        base[L + 1] = base[L] + comps[L].size();
    std::vector<int> parent(base[nl]);
    std::iota(parent.begin(), parent.end(), 0);
    // Iterative find with path halving; only used serially.
    auto find = [&](int x) {
        while (parent[size_t(x)] != x) {
            parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
            x                 = parent[size_t(x)];
        }
        return x;
    };
    // Overlaps between neighbouring layers, per slot, on cached bounding boxes (heavily fragmented
    // painting has thousands of pieces per layer).
    std::vector<std::vector<BoundingBox>> boxes(nl);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nl), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t L = range.begin(); L < range.end(); ++L)
            for (const Comp &c : comps[L])
                boxes[L].push_back(get_extents(seg[L][c.slot][c.idx]));
    });
    std::vector<std::vector<std::pair<int, int>>> links(nl);   // (a on L, b on L+1)
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nl > 0 ? nl - 1 : 0), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t L = range.begin(); L < range.end(); ++L) {
            throw_on_cancel();
            std::map<size_t, std::vector<int>> upper;
            for (size_t b = 0; b < comps[L + 1].size(); ++b)
                upper[comps[L + 1][b].slot].push_back(int(b));
            for (size_t a = 0; a < comps[L].size(); ++a) {
                const Comp &ca = comps[L][a];
                const auto  it = upper.find(ca.slot);
                if (it == upper.end())
                    continue;
                const ExPolygon &pa = seg[L][ca.slot][ca.idx];
                for (int b : it->second) {
                    if (!boxes[L][a].overlap(boxes[L + 1][size_t(b)]))
                        continue;
                    const Comp &cb = comps[L + 1][size_t(b)];
                    if (!intersection(pa, seg[L + 1][cb.slot][cb.idx]).empty())
                        links[L].emplace_back(int(a), b);
                }
            }
        }
    });
    for (size_t L = 0; L + 1 < nl; ++L)
        for (const auto &[a, b] : links[L])
            parent[size_t(find(int(base[L]) + a))] = find(int(base[L + 1]) + b);
    // Roots resolved serially, so the parallel pass below only reads them.
    std::vector<int>    root(parent.size());
    std::vector<double> island_area(parent.size(), 0.);   // mm^2 of visible surface
    for (size_t L = 0; L < nl; ++L) {
        const double h = L == 0 ? (first_layer_height > 0. ? first_layer_height : print_z[0]) : print_z[L] - print_z[L - 1];
        for (size_t a = 0; a < comps[L].size(); ++a) {
            const size_t g = base[L] + a;
            root[g]        = find(int(g));
            island_area[size_t(root[g])] += unscaled(comps[L][a].run) * h + comps[L][a].exposed;
        }
    }
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nl), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t L = range.begin(); L < range.end(); ++L) {
            throw_on_cancel();
            std::vector<std::pair<size_t, ExPolygon>> removed;   // (slot, piece)
            // 1 = the whole piece goes (small island), 2 = only its part near the surface (short run).
            std::vector<std::vector<uint8_t>> drop(seg[L].size());
            for (size_t slot = 0; slot < seg[L].size(); ++slot)
                drop[slot].assign(seg[L][slot].size(), 0);
            for (size_t a = 0; a < comps[L].size(); ++a) {
                const Comp &c      = comps[L][a];
                const bool  sliver = c.run > 0. && c.run < min_run;
                const bool  island = settings.min_island_area > 0. && island_area[size_t(root[base[L] + a])] < settings.min_island_area;
                if (island)
                    drop[c.slot][c.idx] = 1;
                else if (sliver)
                    drop[c.slot][c.idx] = 2;
            }
            std::vector<ExPolygons> kept(seg[L].size());
            for (size_t slot = 0; slot < seg[L].size(); ++slot)
                for (size_t i = 0; i < seg[L][slot].size(); ++i) {
                    if (drop[slot][i] == 1)
                        removed.emplace_back(slot, seg[L][slot][i]);
                    else if (drop[slot][i] == 2) {
                        const ExPolygons piece{seg[L][slot][i]};
                        for (ExPolygon &e : intersection_ex(piece, surface_band[L]))
                            removed.emplace_back(slot, std::move(e));
                        append(kept[slot], diff_ex(piece, surface_band[L]));
                    } else
                        kept[slot].push_back(seg[L][slot][i]);
                }
            if (removed.empty())
                continue;
            for (auto &[slot, piece] : removed) {
                // The neighbour sharing the longest border near the surface, else any neighbour.
                const ExPolygons grown = offset_ex(piece, scaled<float>(0.1));
                const ExPolygons near  = intersection_ex(grown, surface_band[L]);
                size_t best = slot;
                for (int pass = 0; pass < 2 && best == slot; ++pass) {
                    double best_area = 0.;
                    for (size_t t = 0; t < kept.size(); ++t) {
                        if (t == slot || kept[t].empty())
                            continue;
                        const double a = ex_area(intersection_ex(pass == 0 ? near : grown, kept[t]));
                        if (a > best_area) {
                            best_area = a;
                            best      = t;
                        }
                    }
                }
                kept[best].push_back(std::move(piece));
            }
            for (size_t slot = 0; slot < kept.size(); ++slot)
                seg[L][slot] = kept[slot].size() > 1 ? union_ex(kept[slot]) : std::move(kept[slot]);
        }
    });
}

void apply_slice_techniques(std::vector<std::vector<ExPolygons>> &segmentation,
                            const std::vector<ExPolygons>        &outlines,
                            const std::vector<double>            &print_z,
                            unsigned int                          num_physical,
                            const FullSpectrum::VirtualExtruders &ves,
                            const SliceSettings                  &settings,
                            const std::function<void()>          &throw_on_cancel,
                            int                                   top_shells,
                            int                                   bottom_shells,
                            double                                first_layer_height)
{
    const size_t num_layers = segmentation.size();
    // Area of layer L under neither a top nor a bottom shell: covered by the next `top_shells` and
    // the previous `bottom_shells` layers (the whole outline when no shells are given).
    auto shell_free = [&](size_t L, const ExPolygons &area) -> ExPolygons {
        if (area.empty() || (top_shells <= 0 && bottom_shells <= 0))
            return area;
        Polygons covered = to_polygons(area);
        for (int k = 1; k <= top_shells && !covered.empty(); ++k)
            covered = L + size_t(k) < num_layers ? intersection(covered, to_polygons(outlines[L + size_t(k)])) : Polygons();
        for (int k = 1; k <= bottom_shells && !covered.empty(); ++k)
            covered = L >= size_t(k) ? intersection(covered, to_polygons(outlines[L - size_t(k)])) : Polygons();
        return union_ex(covered);
    };

    // ---- 0. Region cleanup ----
    if (settings.region_cleanup)
        cleanup_regions(segmentation, outlines, print_z, settings, throw_on_cancel, first_layer_height);

    // ---- 1. Top-surface patches (before the virtual slots are emptied) ----
    std::vector<ExPolygons> patch_zone(num_layers);
    if (settings.top_pattern && !ves.empty()) {
        const coord_t cell    = scaled<coord_t>(settings.top_cell_size);
        const float   inset   = scaled<float>(TOP_WALL_INSET_MM);
        const double  min_area = MIN_PATCH_AREA_MM2 / (SCALING_FACTOR * SCALING_FACTOR);
        const int     depth    = std::max(1, settings.top_layers);
        // Filament shares of each virtual extruder (they depend on all layers: computed once).
        std::map<unsigned int, std::vector<std::pair<unsigned int, double>>> ve_shares;
        for (const FullSpectrum::VirtualExtruder &ve : ves)
            ve_shares[ve.id] = virtual_extruder_shares(ve, print_z);
        // Slope rule: horizontal step per mm of height at the cutoff slope (see patch_band_width).
        double step_per_mm = 0.;
        if (settings.patch_band_width > 0. && num_layers > 1) {
            std::vector<double> hs;
            for (size_t L = 1; L < num_layers; ++L)
                hs.push_back(print_z[L] - print_z[L - 1]);
            std::nth_element(hs.begin(), hs.begin() + long(hs.size() / 2), hs.end());
            const double h = std::max(1e-3, hs[hs.size() / 2]);
            const double B = std::max(settings.patch_band_width, 1.05 * h);
            step_per_mm    = std::sqrt(B * B - h * h) / h; // 1 / tan(cutoff angle)
        }
        tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t L = range.begin(); L < range.end(); ++L) {
                throw_on_cancel();
                if (outlines[L].empty())
                    continue;
                ExPolygons zone;
                if (step_per_mm > 0.) {
                    // Under a surface flatter than the cutoff slope: farther than k layers' worth of
                    // cutoff-slope step from the outline k layers up (flat tops: nothing above).
                    Polygons covered = to_polygons(outlines[L]);
                    for (int k = 1; k <= depth && !covered.empty(); ++k) {
                        if (L + size_t(k) >= num_layers) {
                            covered.clear();
                            break;
                        }
                        const double dz = print_z[L + size_t(k)] - print_z[L];
                        covered = intersection(covered, offset(to_polygons(outlines[L + size_t(k)]), scaled<float>(dz * step_per_mm)));
                    }
                    zone = diff_ex(outlines[L], covered);
                    // The outer ring (external perimeters) is the top rim of a wall seen from the
                    // side: it keeps the layer word, except where the layer below sticks out by more
                    // than the cutoff step (a real slope: the ring is part of the sloped surface).
                    if (!zone.empty()) {
                        const ExPolygons ring = diff_ex(outlines[L], offset_ex(outlines[L], -inset));
                        ExPolygons       ring_on_slope;
                        if (L > 0) {
                            const double dz0 = print_z[L] - print_z[L - 1];
                            const float  d   = scaled<float>(dz0 * step_per_mm);
                            const ExPolygons below_out = diff_ex(outlines[L - 1], offset_ex(outlines[L], d));
                            if (!below_out.empty())
                                ring_on_slope = intersection_ex(ring, offset_ex(below_out, inset + d + scaled<float>(0.05)));
                        }
                        zone = diff_ex(zone, ring);
                        if (!ring_on_slope.empty())
                            zone = union_ex(zone, intersection_ex(diff_ex(outlines[L], covered), ring_on_slope));
                    }
                } else {
                    // Covered by all of the next `depth` layers = not near a top surface.
                    Polygons covered = to_polygons(outlines[L]);
                    for (int k = 1; k <= depth && !covered.empty(); ++k)
                        covered = L + size_t(k) < num_layers ? intersection(covered, to_polygons(outlines[L + size_t(k)])) : Polygons();
                    zone = diff_ex(offset_ex(outlines[L], -inset), covered);
                }
                if (zone.empty())
                    continue;
                patch_zone[L] = zone;
                // Distance below the top: shift the pattern one cell per layer so cells don't stack.
                int below = 0;
                for (int k = 1; k <= depth; ++k)
                    if (L + size_t(k) < num_layers && !intersection_ex(zone, outlines[L + size_t(k)]).empty())
                        below = k;

                for (const FullSpectrum::VirtualExtruder &ve : ves) {
                    const size_t slot = ve.id - 1;
                    // Gradients change along Z: no single mix for their tops.
                    if (ve.gradient || slot >= segmentation[L].size() || segmentation[L][slot].empty())
                        continue;
                    ExPolygons region = intersection_ex(segmentation[L][slot], zone);
                    if (region.empty())
                        continue;
                    const auto &shares = ve_shares.at(ve.id);
                    if (shares.empty())
                        continue;
                    const BoundingBox bb = get_extents(region);
                    std::map<unsigned int, Polygons> cells;
                    const int j0 = int(std::floor(double(bb.min.y()) / cell)), j1 = int(std::floor(double(bb.max.y()) / cell));
                    for (int j = j0; j <= j1; ++j) {
                        // Only cells over the region's pieces in this row (a thin ring around a large
                        // object would otherwise fill its whole bounding box with cells).
                        const Polygon strip({{bb.min.x() - cell, coord_t(j) * cell}, {bb.max.x() + cell, coord_t(j) * cell},
                                             {bb.max.x() + cell, coord_t(j + 1) * cell}, {bb.min.x() - cell, coord_t(j + 1) * cell}});
                        std::set<int> columns;
                        for (const ExPolygon &piece : intersection_ex(region, Polygons{strip})) {
                            const BoundingBox pb = get_extents(piece);
                            for (int i = int(std::floor(double(pb.min.x()) / cell)); i <= int(std::floor(double(pb.max.x()) / cell)); ++i)
                                columns.insert(i);
                        }
                        for (int i : columns) {
                            const unsigned int e = bayer_pick(shares, bayer8(i + below, j));
                            Polygon sq({{coord_t(i) * cell, coord_t(j) * cell}, {coord_t(i + 1) * cell, coord_t(j) * cell},
                                        {coord_t(i + 1) * cell, coord_t(j + 1) * cell}, {coord_t(i) * cell, coord_t(j + 1) * cell}});
                            cells[e].emplace_back(std::move(sq));
                        }
                    }
                    const unsigned int main_e = shares.front().first;
                    ExPolygons         assigned;
                    for (auto &[e, polys] : cells) {
                        if (e == main_e || e < 1 || e > num_physical)
                            continue;
                        ExPolygons piece = intersection_ex(union_ex(polys), region);
                        piece.erase(std::remove_if(piece.begin(), piece.end(), [min_area](const ExPolygon &p) { return p.area() < min_area; }),
                                    piece.end());
                        if (piece.empty())
                            continue;
                        append(assigned, piece);
                        append(segmentation[L][e - 1], std::move(piece));
                    }
                    if (main_e >= 1 && main_e <= num_physical)
                        append(segmentation[L][main_e - 1], diff_ex(region, assigned));
                    segmentation[L][slot] = diff_ex(segmentation[L][slot], zone);
                }
            }
        });
    }

    // ---- 1b. Dual walls: outer ring follows the outer word, the rest the inner word ----
    bool any_dual = false;
    for (const FullSpectrum::VirtualExtruder &ve : ves)
        any_dual |= !ve.inner_sequence.empty() && !ve.sequence.empty();
    if (any_dual) {
        const float ring = scaled<float>(std::max(0.2, settings.dual_wall_width));
        tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t L = range.begin(); L < range.end(); ++L) {
                throw_on_cancel();
                if (outlines[L].empty())
                    continue;
                // The inner word only behind side walls: flat tops and bottoms (the shells) show the
                // outer word, like the color the recipe was predicted for.
                const ExPolygons inner_area = shell_free(L, offset_ex(outlines[L], -ring));
                for (const FullSpectrum::VirtualExtruder &ve : ves) {
                    if (ve.inner_sequence.empty() || ve.sequence.empty())
                        continue;
                    const size_t slot = ve.id - 1;
                    if (slot >= segmentation[L].size() || segmentation[L][slot].empty())
                        continue;
                    const unsigned int outer_e = ve.sequence[L % ve.sequence.size()];
                    const unsigned int inner_e = ve.inner_sequence[L % ve.inner_sequence.size()];
                    if (outer_e < 1 || outer_e > num_physical || inner_e < 1 || inner_e > num_physical)
                        continue;
                    ExPolygons inner = intersection_ex(segmentation[L][slot], inner_area);
                    ExPolygons outer = diff_ex(segmentation[L][slot], inner_area);
                    append(segmentation[L][outer_e - 1], std::move(outer));
                    append(segmentation[L][inner_e - 1], std::move(inner));
                    segmentation[L][slot].clear();
                }
            }
        });
    }

    // ---- 2. Layer words for the rest ----
    FullSpectrum::remap_virtual_extruders_to_physical(segmentation, print_z, num_physical, ves);

    // ---- 3. White core ----
    const unsigned int core = settings.core_extruder;
    if (core >= 1 && core <= num_physical) {
        // Deeper than the dual-wall rings, or the inner word would be covered by the core.
        const float depth = scaled<float>(std::max({0.2, settings.wall_depth, any_dual ? settings.dual_wall_width + 0.45 : 0.}));
        tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t L = range.begin(); L < range.end(); ++L) {
                throw_on_cancel();
                if (outlines[L].empty())
                    continue;
                // Top and bottom shells keep their color (flat tops and visible bottoms).
                ExPolygons interior = shell_free(L, offset_ex(outlines[L], -depth));
                if (!patch_zone[L].empty())
                    interior = diff_ex(interior, patch_zone[L]);
                if (interior.empty())
                    continue;
                bool any_painted = false;
                for (size_t slot = 0; slot < segmentation[L].size() && slot < num_physical; ++slot)
                    if (slot != core - 1 && !segmentation[L][slot].empty()) {
                        any_painted = true;
                        break;
                    }
                if (!any_painted)
                    continue;
                ExPolygons stolen;
                // Unpainted interior (beyond the painted depth: it would print with the object's own
                // extruder, an extra tool on the layer) goes to the core as well - but not the inside
                // of an unpainted part that reaches the surface (e.g. a separate part in its own color).
                {
                    ExPolygons painted;
                    for (const ExPolygons &r : segmentation[L])
                        append(painted, r);
                    painted              = union_ex(painted);
                    ExPolygons unpainted = diff_ex(interior, painted);
                    if (!unpainted.empty()) {
                        const ExPolygons unpainted_surface = diff_ex(diff_ex(outlines[L], painted), interior);
                        if (!unpainted_surface.empty()) {
                            const ExPolygons near = offset_ex(unpainted_surface, scaled<float>(0.05));
                            unpainted.erase(std::remove_if(unpainted.begin(), unpainted.end(),
                                                           [&near](const ExPolygon &e) { return !intersection_ex(ExPolygons{e}, near).empty(); }),
                                            unpainted.end());
                        }
                        append(stolen, std::move(unpainted));
                    }
                }
                for (size_t slot = 0; slot < segmentation[L].size() && slot < num_physical; ++slot) {
                    if (slot == core - 1 || segmentation[L][slot].empty())
                        continue;
                    ExPolygons inside = intersection_ex(segmentation[L][slot], interior);
                    if (inside.empty())
                        continue;
                    segmentation[L][slot] = diff_ex(segmentation[L][slot], interior);
                    append(stolen, std::move(inside));
                }
                if (!stolen.empty()) {
                    append(segmentation[L][core - 1], std::move(stolen));
                    segmentation[L][core - 1] = union_ex(segmentation[L][core - 1]);
                }
            }
        });
    }
}

} // namespace Slic3r::ColorMix
