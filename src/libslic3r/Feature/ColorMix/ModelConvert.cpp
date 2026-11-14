///|/ ColorMax engine - apply a color conversion to a Model (painting + virtual extruders).
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "ModelConvert.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

#include "libslic3r/Model.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <nlohmann/json.hpp>
#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r::ColorMix {

namespace {

unsigned int base_extruder(const ModelVolume &v)
{
    const int e = v.extruder_id();
    return e > 0 ? unsigned(e) : 1u;
}

// Source slots of volumes without a source painting. Their current painting holds extruder ids
// (physical tools and virtual extruders). Before any volume has a source painting those ids are the
// source slots themselves. Afterwards (e.g. a part added after a conversion) each id maps to the
// palette slot of its current color, or to a new slot appended to the palette.
struct UnsourcedSlots
{
    std::map<unsigned int, unsigned int> slot;       // current state -> source slot
    std::vector<std::string>             new_colors; // colors of the appended slots, in order
};

// Painting states a volume uses now (unpainted triangles count as its extruder).
std::set<unsigned int> current_states(const ModelVolume &v)
{
    std::set<unsigned int> out;
    const std::vector<bool> &used = v.mm_segmentation_facets.get_data().used_states;
    for (size_t s = 1; s < used.size(); ++s)
        if (used[s])
            out.insert(unsigned(s));
    const bool all_painted = v.is_mm_painted() && v.mm_segmentation_facets.get_data().triangles_to_split.size() == v.mesh().its.indices.size() &&
                             !(used.empty() || used[0]);
    if (!all_painted)
        out.insert(std::min<unsigned>(base_extruder(v), TRIANGLE_STATE_TYPE_COUNT - 1));
    return out;
}

UnsourcedSlots unsourced_slots(const Model &model, const std::vector<std::string> &extruder_colors, bool force_by_color = false)
{
    UnsourcedSlots out;
    bool                   any_source = false;
    std::set<unsigned int> states;
    for (const ModelObject *o : model.objects)
        for (const ModelVolume *v : o->volumes)
            if (v->is_model_part()) {
                if (v->has_colormix_source())
                    any_source = true;
                else
                    for (unsigned int st : current_states(*v))
                        states.insert(st);
            }
    const std::vector<std::string> &palette = model.colormix.source_colors;
    // Before any conversion, a volume's states are the source slots themselves (e.g. an imported
    // file's material slots). After one (the bundle is recorded) they are tool / blend ids: map
    // them by color.
    (void) any_source;
    const bool by_color = force_by_color || !model.colormix.bundle_id.empty();
    for (unsigned int st : states) {
        if (!by_color) {
            out.slot[st] = st;
            continue;
        }
        std::string hex = st < extruder_colors.size() ? normalize_hex(extruder_colors[st]) : std::string();
        if (hex.empty())
            hex = "#808080";
        unsigned int slot = 0;
        for (size_t k = 1; k < palette.size() && slot == 0; ++k)
            if (normalize_hex(palette[k]) == hex)
                slot = unsigned(k);
        for (size_t k = 0; k < out.new_colors.size() && slot == 0; ++k)
            if (out.new_colors[k] == hex)
                slot = unsigned(std::max<size_t>(palette.size(), 1) + k);
        if (slot == 0) {
            slot = unsigned(std::max<size_t>(palette.size(), 1) + out.new_colors.size());
            if (slot >= TRIANGLE_STATE_TYPE_COUNT)
                slot = st; // out of painting states: keep the state as it is
            else
                out.new_colors.push_back(hex);
        }
        out.slot[st] = slot;
    }
    return out;
}

// Full source painting of a volume: the stored source, or the current painting with unpainted
// triangles set to the volume extruder, its states moved to their source slots.
TriangleSelector source_selector(const ModelVolume &v, const UnsourcedSlots *slots = nullptr)
{
    TriangleSelector sel(v.mesh());
    if (v.has_colormix_source()) {
        sel.deserialize(v.colormix_source_facets.get_data(), false);
        return sel;
    }
    if (v.is_mm_painted())
        sel.deserialize(v.mm_segmentation_facets.get_data(), false);
    std::map<TriangleStateType, TriangleStateType> remap{
        {TriangleStateType::NONE, static_cast<TriangleStateType>(std::min<unsigned>(base_extruder(v), TRIANGLE_STATE_TYPE_COUNT - 1))}};
    sel.remap_states(remap);
    if (slots != nullptr) {
        // remap_states() looks every triangle up once, so sources and targets may overlap.
        std::map<TriangleStateType, TriangleStateType> to_slots;
        for (const auto &[from, to] : slots->slot)
            if (from != to)
                to_slots[static_cast<TriangleStateType>(from)] = static_cast<TriangleStateType>(to);
        if (!to_slots.empty())
            sel.remap_states(to_slots);
    }
    return sel;
}

// A blend's own color, kept next to the JSON form of the blends (which stores a computed color for
// gradients): set back by position.
void restore_virtual_extruder_colors(FullSpectrum::VirtualExtruders &ves, const nlohmann::json &options)
{
    if (!options.is_object() || !options.contains("original_virtual_extruder_colors"))
        return;
    const nlohmann::json &c = options["original_virtual_extruder_colors"];
    if (!c.is_array() || c.size() != ves.size())
        return;
    for (size_t i = 0; i < ves.size(); ++i)
        ves[i].color = c[i].is_string() ? std::optional<std::string>(c[i].get<std::string>()) : std::nullopt;
}

} // namespace

std::vector<std::string> current_extruder_colors(const Model &model, const std::vector<std::string> &physical_colors)
{
    std::vector<std::string> out(1, std::string());
    for (const std::string &c : physical_colors)
        out.push_back(normalize_hex(c).empty() ? std::string("#808080") : normalize_hex(c));
    for (const FullSpectrum::VirtualExtruder &ve : model.virtual_extruders) {
        if (ve.id >= out.size())
            out.resize(ve.id + 1, "#808080");
        const std::string c = ve.effective_color(physical_colors);
        out[ve.id] = normalize_hex(c).empty() ? std::string("#808080") : normalize_hex(c);
    }
    return out;
}

bool has_convertible_colors(const Model &model)
{
    for (const ModelObject *o : model.objects)
        for (const ModelVolume *v : o->volumes)
            if (v->is_model_part())
                return true;
    return false;
}

namespace {
void capture_source_impl(Model &model, const std::vector<std::string> &extruder_colors, bool force_by_color)
{
    std::set<unsigned int> used;
    const UnsourcedSlots   slots = unsourced_slots(model, extruder_colors, force_by_color);
    if (!slots.new_colors.empty()) {
        std::vector<std::string> &palette = model.colormix.source_colors;
        if (palette.empty())
            palette.push_back(std::string());
        const size_t first_new = palette.size();
        palette.insert(palette.end(), slots.new_colors.begin(), slots.new_colors.end());
        // Remember the tool each new slot came from (for revert_to_source()).
        std::vector<unsigned int> &tools = model.colormix.slot_tools;
        for (const auto &[state, slot] : slots.slot)
            if (slot >= first_new && slot != state) {
                if (tools.size() <= slot)
                    tools.resize(slot + 1, 0);
                if (tools[slot] == 0)
                    tools[slot] = state;
            }
    }
    for (ModelObject *o : model.objects)
        for (ModelVolume *v : o->volumes) {
            if (!v->is_model_part())
                continue;
            if (!v->has_colormix_source()) {
                TriangleSelector sel = source_selector(*v, &slots);
                v->colormix_source_facets.set(sel);
            }
            const std::vector<bool> &states = v->colormix_source_facets.get_data().used_states;
            for (size_t s = 1; s < states.size(); ++s)
                if (states[s])
                    used.insert(unsigned(s));
        }
    std::vector<std::string> &colors = model.colormix.source_colors;
    if (colors.empty())
        colors.push_back(std::string());
    for (unsigned int s : used) {
        if (colors.size() <= s)
            colors.resize(s + 1);
        if (normalize_hex(colors[s]).empty())
            colors[s] = s < extruder_colors.size() && !normalize_hex(extruder_colors[s]).empty() ? normalize_hex(extruder_colors[s]) : std::string("#808080");
    }
}

} // namespace

void capture_source(Model &model, const std::vector<std::string> &extruder_colors) { capture_source_impl(model, extruder_colors, false); }

void capture_source_by_color(Model &model, const std::vector<std::string> &extruder_colors)
{
    capture_source_impl(model, extruder_colors, true);
}

std::vector<SourceColor> collect_source_colors(const Model &model, const std::vector<std::string> &fallback_colors)
{
    const UnsourcedSlots slots = unsourced_slots(model, fallback_colors);
    const size_t         palette_end = std::max<size_t>(model.colormix.source_colors.size(), 1);
    std::map<unsigned int, SourceColor>        by_state;
    std::map<unsigned int, std::set<std::string>> names;
    std::map<unsigned int, std::vector<std::pair<double, double>>> spans, top_spans;
    for (const ModelObject *o : model.objects) {
        const Transform3d inst = o->instances.empty() ? Transform3d::Identity() : o->instances.front()->get_matrix();
        // Heights are measured from the object's lowest point (it sits on the bed when sliced).
        double z_bed = std::numeric_limits<double>::max();
        for (const ModelVolume *v : o->volumes)
            if (v->is_model_part())
                for (const stl_vertex &p : v->mesh().its.vertices)
                    z_bed = std::min(z_bed, (inst * v->get_matrix() * p.cast<double>()).z());
        if (z_bed == std::numeric_limits<double>::max())
            z_bed = 0.;
        for (const ModelVolume *v : o->volumes) {
            if (!v->is_model_part())
                continue;
            const TriangleSelector sel = source_selector(*v, &slots);
            const Transform3d      trafo = inst * v->get_matrix();
            // Only the states the volume uses (a full scan of 255 states is slow on large meshes).
            std::vector<bool> used(TRIANGLE_STATE_TYPE_COUNT, false);
            if (v->has_colormix_source()) {
                const std::vector<bool> &u = v->colormix_source_facets.get_data().used_states;
                for (size_t s = 1; s < u.size() && s < used.size(); ++s)
                    used[s] = u[s];
            } else
                for (unsigned int st : current_states(*v))
                    if (auto it = slots.slot.find(st); it != slots.slot.end() && it->second < used.size())
                        used[it->second] = true;
            for (size_t s = 1; s < TRIANGLE_STATE_TYPE_COUNT; ++s) {
                if (!used[s])
                    continue;
                const indexed_triangle_set its = sel.get_facets_strict(static_cast<TriangleStateType>(s));
                if (its.indices.empty())
                    continue;
                double area = 0.;
                std::vector<std::pair<double, double>> &zs = spans[unsigned(s)];
                for (const stl_triangle_vertex_indices &f : its.indices) {
                    const Vec3d a = trafo * its.vertices[size_t(f[0])].cast<double>();
                    const Vec3d b = trafo * its.vertices[size_t(f[1])].cast<double>();
                    const Vec3d c = trafo * its.vertices[size_t(f[2])].cast<double>();
                    zs.emplace_back(std::min({a.z(), b.z(), c.z()}) - z_bed, std::max({a.z(), b.z(), c.z()}) - z_bed);
                    const Vec3d n = (b - a).cross(c - a);
                    const double len = n.norm();
                    if (len <= 0.)
                        continue;
                    if (n.z() / len > 0.8)
                        top_spans[unsigned(s)].emplace_back(std::min({a.z(), b.z(), c.z()}) - z_bed, std::max({a.z(), b.z(), c.z()}) - z_bed);
                    if (n.z() / len < -0.1)
                        continue; // facing down: hidden on the bed or under overhangs
                    area += 0.5 * len;
                }
                SourceColor &sc = by_state[unsigned(s)];
                sc.state = unsigned(s);
                sc.area += area;
                names[unsigned(s)].insert(v->name.empty() ? o->name : v->name);
            }
        }
    }
    std::vector<SourceColor> out;
    for (auto &[s, sc] : by_state) {
        sc.color = s < model.colormix.source_colors.size() ? normalize_hex(model.colormix.source_colors[s]) : std::string();
        if (sc.color.empty() && s >= palette_end && s - palette_end < slots.new_colors.size())
            sc.color = slots.new_colors[s - palette_end];
        if (sc.color.empty() && s < fallback_colors.size())
            sc.color = normalize_hex(fallback_colors[s]);
        if (sc.color.empty())
            sc.color = "#808080";
        std::string label;
        for (const std::string &n : names[s]) {
            if (label.size() > 60) {
                label += ", ...";
                break;
            }
            label += (label.empty() ? "" : ", ") + n;
        }
        sc.label = label;
        // Merge the triangle spans into disjoint ranges.
        auto merge = [](std::vector<std::pair<double, double>> &zs, std::vector<std::pair<double, double>> &out) {
            std::sort(zs.begin(), zs.end());
            for (const auto &r : zs) {
                if (!out.empty() && r.first <= out.back().second + 0.05)
                    out.back().second = std::max(out.back().second, r.second);
                else
                    out.push_back(r);
            }
        };
        merge(spans[s], sc.z_ranges);
        merge(top_spans[s], sc.top_z_ranges);
        out.push_back(sc);
    }
    return out;
}

FullSpectrum::VirtualExtruder make_virtual_extruder(unsigned int id, const Recipe &recipe, const Prediction &prediction,
                                                    const std::vector<unsigned int> &tool_of_filament)
{
    FullSpectrum::VirtualExtruder ve;
    ve.id    = id;
    ve.color = lab_to_hex(prediction.color);
    std::map<unsigned int, double> shares;
    const double outer_w = recipe.dual() ? 0.5 : 1.;
    for (int f : recipe.word) {
        const unsigned int tool = size_t(f) < tool_of_filament.size() ? tool_of_filament[size_t(f)] : 0;
        ve.sequence.push_back(tool);
        shares[tool] += outer_w / double(recipe.word.size());
    }
    for (int f : recipe.inner_word) {
        const unsigned int tool = size_t(f) < tool_of_filament.size() ? tool_of_filament[size_t(f)] : 0;
        ve.inner_sequence.push_back(tool);
        shares[tool] += 0.5 / double(recipe.inner_word.size());
    }
    for (const auto &[tool, share] : shares)
        ve.components.push_back({tool, share});
    return ve;
}

ApplySummary apply_conversion(Model &model, const std::vector<SourceColor> &sources, const ConversionResult &result,
                              const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament, unsigned int num_physical)
{
    ApplySummary summary;
    FullSpectrum::VirtualExtruders ves;
    std::map<int, unsigned int>     ext_of_candidate;
    unsigned int                    next_id = num_physical + 1;
    for (size_t k = 0; k < sources.size() && k < result.choice.size(); ++k) {
        const int ci = result.choice[k];
        if (ci < 0) {
            ++summary.unprintable_colors;
            continue;
        }
        auto it = ext_of_candidate.find(ci);
        unsigned int ext = 0;
        if (it != ext_of_candidate.end())
            ext = it->second;
        else {
            const Candidate &c = result.candidates[size_t(ci)];
            if (c.pure()) {
                const int f = c.recipe.word.front();
                ext = size_t(f) < tool_of_filament.size() ? tool_of_filament[size_t(f)] : 0;
                ++summary.solid_colors;
            } else if (next_id < TRIANGLE_STATE_TYPE_COUNT) {
                ext = next_id++;
                ves.push_back(make_virtual_extruder(ext, c.recipe, c.prediction, tool_of_filament));
            }
            ext_of_candidate[ci] = ext;
        }
        if (ext == 0 || ext >= TRIANGLE_STATE_TYPE_COUNT) {
            ++summary.unmapped_colors;
            continue;
        }
        if (summary.state_to_extruder.size() <= sources[k].state)
            summary.state_to_extruder.resize(sources[k].state + 1, 0);
        summary.state_to_extruder[sources[k].state] = ext;
    }
    (void) bundle;

    // Before the first conversion: remember the project's own virtual extruders (e.g. Full Spectrum
    // blends) and the extruder settings naming them, which this replaces - Revert restores them.
    nlohmann::json options = nlohmann::json::parse(model.colormix.options_json.empty() ? std::string("{}") : model.colormix.options_json, nullptr, false);
    if (!options.is_object())
        options = nlohmann::json::object();
    const bool     first_conversion = !options.contains("original_virtual_extruders");
    // [object, volume (-1 object, -2 layer range), value, z0, z1, object name, object volume count]
    nlohmann::json replaced = nlohmann::json::array();
    auto replace_extruder = [&](ModelConfig &cfg, size_t oi, int vi, double z0 = 0., double z1 = 0.) {
        // An extruder setting naming a virtual extruder would name an unrelated new blend now.
        if (cfg.has("extruder") && cfg.opt_int("extruder") > int(num_physical)) {
            replaced.push_back({oi, vi, cfg.opt_int("extruder"), z0, z1, model.objects[oi]->name, model.objects[oi]->volumes.size()});
            cfg.set("extruder", 1);
        }
    };
    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        ModelObject *o = model.objects[oi];
        replace_extruder(o->config, oi, -1);
        for (size_t vi = 0; vi < o->volumes.size(); ++vi)
            replace_extruder(o->volumes[vi]->config, oi, int(vi));
        for (auto &[range, cfg] : o->layer_config_ranges)
            replace_extruder(cfg, oi, -2, range.first, range.second);
    }
    if (first_conversion) {
        options["original_virtual_extruders"] = FullSpectrum::serialize_virtual_extruders_to_json({}, model.virtual_extruders);
        nlohmann::json own_colors = nlohmann::json::array();
        for (const FullSpectrum::VirtualExtruder &ve : model.virtual_extruders)
            own_colors.push_back(ve.color ? nlohmann::json(*ve.color) : nlohmann::json());
        options["original_virtual_extruder_colors"] = own_colors;
        options["original_extruders"]         = replaced;
        model.colormix.options_json           = options.dump();
    }

    for (ModelObject *o : model.objects)
        for (ModelVolume *v : o->volumes) {
            if (!v->is_model_part())
                continue;
            if (!v->has_colormix_source())
                v->colormix_source_facets.set(source_selector(*v));
            v->mm_segmentation_facets.set(converted_painting(*v, summary.state_to_extruder, num_physical));
        }
    model.virtual_extruders = FullSpectrum::normalize_virtual_extruders(ves);
    summary.virtual_extruders = int(model.virtual_extruders.size());
    return summary;
}

TriangleSelector converted_painting(const ModelVolume &v, const std::vector<unsigned int> &state_to_extruder, unsigned int num_physical)
{
    TriangleSelector sel = source_selector(v);
    // remap_states() looks every triangle up once, so sources and targets may overlap.
    std::map<TriangleStateType, TriangleStateType> remap;
    // Unmapped slots: the volume's extruder, unless it is a virtual extruder (all replaced).
    const TriangleStateType fallback = base_extruder(v) <= num_physical ? TriangleStateType::NONE : static_cast<TriangleStateType>(1);
    const std::vector<bool> used = v.has_colormix_source() ? v.colormix_source_facets.get_data().used_states : sel.serialize().used_states;
    for (size_t s = 1; s < used.size(); ++s)
        if (used[s]) {
            const unsigned int e = s < state_to_extruder.size() ? state_to_extruder[s] : 0;
            remap[static_cast<TriangleStateType>(s)] = e > 0 && e < TRIANGLE_STATE_TYPE_COUNT ? static_cast<TriangleStateType>(e) : fallback;
        }
    sel.remap_states(remap);
    return sel;
}

namespace {
// A painting as it prints: unpainted triangles and states that are neither a tool nor a virtual
// extruder (skipped by the segmentation) print with the volume's extruder.
TriangleSelector::TriangleSplittingData printed_states(const ModelVolume &v, TriangleSelector sel, unsigned int num_physical,
                                                       const std::set<unsigned int> &virtual_ids)
{
    const TriangleSelector::TriangleSplittingData data = sel.serialize();
    const TriangleStateType base = static_cast<TriangleStateType>(std::min<unsigned>(base_extruder(v), TRIANGLE_STATE_TYPE_COUNT - 1));
    std::map<TriangleStateType, TriangleStateType> remap{{TriangleStateType::NONE, base}};
    for (size_t st = 1; st < data.used_states.size(); ++st)
        if (data.used_states[st] && st > num_physical && !virtual_ids.count(unsigned(st)) && static_cast<TriangleStateType>(st) != base)
            remap[static_cast<TriangleStateType>(st)] = base;
    sel.remap_states(remap);
    return sel.serialize();
}

bool repainted(const Model &model, const ModelVolume &v, const std::vector<unsigned int> &state_to_extruder, unsigned int num_physical)
{
    if (!v.is_model_part() || !v.has_colormix_source())
        return false;
    std::set<unsigned int> virtual_ids;
    for (const FullSpectrum::VirtualExtruder &ve : model.virtual_extruders)
        virtual_ids.insert(ve.id);
    TriangleSelector current(v.mesh());
    current.deserialize(v.mm_segmentation_facets.get_data(), false);
    return !(printed_states(v, std::move(current), num_physical, virtual_ids) ==
             printed_states(v, converted_painting(v, state_to_extruder, num_physical), num_physical, virtual_ids));
}
} // namespace

size_t count_repainted_volumes(const Model &model, const std::vector<unsigned int> &state_to_extruder, unsigned int num_physical)
{
    size_t n = 0;
    for (const ModelObject *o : model.objects)
        for (const ModelVolume *v : o->volumes)
            n += repainted(model, *v, state_to_extruder, num_physical) ? 1 : 0;
    return n;
}

std::vector<unsigned int> fold_repainted_into_source(Model &model, const std::vector<unsigned int> &state_to_extruder, unsigned int num_physical,
                                                     const std::vector<std::string> &extruder_colors, bool converted)
{
    std::vector<unsigned int> applied = state_to_extruder;
    std::vector<ModelVolume *> folded;
    std::vector<std::string> &palette = model.colormix.source_colors;
    if (palette.empty())
        palette.push_back(std::string());
    // Palette slot of a color printed with `tool`: an existing one that prints (converted: any of that
    // color; else: one printing with that tool), else a new one.
    auto slot_of = [&palette, &applied, converted](const std::string &hex_in, unsigned int tool) -> unsigned int {
        const std::string hex = normalize_hex(hex_in).empty() ? std::string("#808080") : normalize_hex(hex_in);
        for (size_t k = 1; k < palette.size(); ++k)
            if (normalize_hex(palette[k]) == hex && k < applied.size() && applied[k] != 0 && (converted || applied[k] == tool))
                return unsigned(k);
        if (palette.size() >= TRIANGLE_STATE_TYPE_COUNT)
            return 0;
        palette.push_back(hex);
        return unsigned(palette.size() - 1);
    };
    for (ModelObject *o : model.objects)
        for (ModelVolume *v : o->volumes) {
            if (!repainted(model, *v, state_to_extruder, num_physical))
                continue;
            // Decided per mesh triangle: one still painted exactly as the conversion painted it keeps
            // its source painting (so source colors that printed alike stay apart); a repainted one
            // gets the slots of its current colors.
            const TriangleStateType fallback = base_extruder(*v) <= num_physical ? TriangleStateType::NONE : static_cast<TriangleStateType>(1);
            std::map<unsigned int, unsigned int> inverse; // painted state -> source slot, where unambiguous
            std::set<unsigned int>               shared;
            const std::vector<bool> &src_used = v->colormix_source_facets.get_data().used_states;
            for (size_t s = 1; s < src_used.size(); ++s)
                if (src_used[s]) {
                    const unsigned int e  = s < state_to_extruder.size() ? state_to_extruder[s] : 0;
                    const unsigned int st = e > 0 && e < TRIANGLE_STATE_TYPE_COUNT ? e : unsigned(fallback);
                    if (!inverse.emplace(st, unsigned(s)).second)
                        shared.insert(st);
                }
            TriangleSelector current(v->mesh());
            current.deserialize(v->mm_segmentation_facets.get_data(), false);
            const TriangleSelector::TriangleSplittingData current_data = current.serialize();
            std::set<unsigned int> states;
            for (size_t st = 0; st < current_data.used_states.size(); ++st)
                if (current_data.used_states[st])
                    states.insert(unsigned(st));
            if (current_data.triangles_to_split.size() != v->mesh().its.indices.size())
                states.insert(0); // unpainted triangles
            std::map<TriangleStateType, TriangleStateType> remap;
            for (unsigned int st : states) {
                unsigned int slot = 0;
                if (auto it = inverse.find(st); it != inverse.end() && !shared.count(st))
                    slot = it->second;
                else {
                    const unsigned int tool    = st == 0 ? std::min<unsigned>(base_extruder(*v), TRIANGLE_STATE_TYPE_COUNT - 1) : st;
                    const size_t       before  = palette.size();
                    slot = slot_of(tool < extruder_colors.size() ? extruder_colors[tool] : std::string(), tool);
                    if (slot >= before && slot > 0) { // a new slot: it prints with this tool (also for revert)
                        std::vector<unsigned int> &tools = model.colormix.slot_tools;
                        if (slot != tool) {
                            if (tools.size() <= slot)
                                tools.resize(slot + 1, 0);
                            tools[slot] = tool;
                        }
                        if (applied.size() <= slot)
                            applied.resize(slot + 1, 0);
                        applied[slot] = tool;
                    }
                }
                if (slot > 0 && slot != st)
                    remap[static_cast<TriangleStateType>(st)] = static_cast<TriangleStateType>(slot);
            }
            TriangleSelector remapped(v->mesh());
            remapped.deserialize(current_data, false);
            // remap_states() looks every triangle up once, so sources and targets may overlap.
            remapped.remap_states(remap);
            // Per triangle: the strings of the current, the converted and the remapped painting.
            const size_t             n = v->mesh().its.indices.size();
            std::vector<std::string> cur(n), src(n), exp(n), rem(n);
            for (size_t i = 0; i < n; ++i) {
                cur[i] = v->mm_segmentation_facets.get_triangle_as_string(int(i));
                src[i] = v->colormix_source_facets.get_triangle_as_string(int(i));
            }
            v->mm_segmentation_facets.set(converted_painting(*v, state_to_extruder, num_physical));
            for (size_t i = 0; i < n; ++i)
                exp[i] = v->mm_segmentation_facets.get_triangle_as_string(int(i));
            v->mm_segmentation_facets.set(remapped);
            for (size_t i = 0; i < n; ++i)
                rem[i] = v->mm_segmentation_facets.get_triangle_as_string(int(i));
            v->mm_segmentation_facets.set(current); // the painting itself doesn't change
            v->colormix_source_facets.reset();
            v->colormix_source_facets.reserve(int(n));
            for (size_t i = 0; i < n; ++i)
                v->colormix_source_facets.set_triangle_from_string(int(i), cur[i] == exp[i] ? src[i] : rem[i]);
            v->colormix_source_facets.shrink_to_fit();
            // Through a selector: FacetsAnnotation::reset() keeps the old used-states summary.
            TriangleSelector rebuilt(v->mesh());
            rebuilt.deserialize(v->colormix_source_facets.get_data(), false);
            v->colormix_source_facets.set(rebuilt);
            folded.push_back(v);
        }
    // Painted from the new source: what the record says (a kept color that reuses a slot prints as
    // that slot does).
    for (ModelVolume *v : folded)
        v->mm_segmentation_facets.set(converted_painting(*v, applied, num_physical));
    return applied;
}

namespace {
// Painting states (source slots) the parts use: their source painting, else their current painting
// plus their extruder (unpainted triangles).
std::vector<bool> used_slots(const ModelObjectPtrs &objects)
{
    std::vector<bool> used(TRIANGLE_STATE_TYPE_COUNT, false);
    for (const ModelObject *o : objects)
        for (const ModelVolume *v : o->volumes) {
            if (!v->is_model_part())
                continue;
            if (v->has_colormix_source()) {
                const std::vector<bool> &states = v->colormix_source_facets.get_data().used_states;
                for (size_t i = 1; i < states.size() && i < used.size(); ++i)
                    used[i] = used[i] || states[i];
            } else
                for (unsigned int st : current_states(*v))
                    if (st > 0 && st < used.size())
                        used[st] = true;
        }
    return used;
}

// A converted project's parts are painted with tool / blend ids of their own project: paint them by
// their source colors again (slot k prints with tool k, like an unconverted import).
void paint_converted_from_source(Model &imported)
{
    if (imported.colormix.bundle_id.empty())
        return;
    // Slot k is tool k, except slots that record their tool (slot_tools).
    std::map<TriangleStateType, TriangleStateType> to_tools;
    const std::vector<unsigned int> &tools = imported.colormix.slot_tools;
    for (size_t s = 1; s < tools.size() && s < TRIANGLE_STATE_TYPE_COUNT; ++s)
        if (tools[s] == ModelData::OWN_EXTRUDER)
            to_tools[static_cast<TriangleStateType>(s)] = TriangleStateType::NONE;
        else if (tools[s] > 0 && tools[s] < TRIANGLE_STATE_TYPE_COUNT && tools[s] != s)
            to_tools[static_cast<TriangleStateType>(s)] = static_cast<TriangleStateType>(tools[s]);
    for (ModelObject *o : imported.objects)
        for (ModelVolume *v : o->volumes)
            if (v->is_model_part() && v->has_colormix_source()) {
                TriangleSelector sel(v->mesh());
                sel.deserialize(v->colormix_source_facets.get_data(), false);
                if (!to_tools.empty())
                    sel.remap_states(to_tools);
                v->mm_segmentation_facets.set(sel);
            }
}
} // namespace

size_t imported_color_count(const Model &imported)
{
    const std::vector<std::string> &palette = imported.colormix.source_colors;
    const std::vector<bool>         used    = used_slots(imported.objects);
    std::set<std::string>           colors;
    for (size_t s = 1; s < used.size() && s < palette.size(); ++s)
        if (used[s] && !normalize_hex(palette[s]).empty())
            colors.insert(normalize_hex(palette[s]));
    return colors.empty() ? (palette.empty() ? 0 : palette.size() - 1) : colors.size();
}

bool merge_imported_palette(Model &plater_model, Model &imported, const std::vector<std::string> &plater_extruder_colors)
{
    bool any_part = false;
    for (const ModelObject *o : plater_model.objects)
        for (const ModelVolume *v : o->volumes)
            any_part |= v->is_model_part();
    if (!any_part) {
        // Nothing on the plater: what is left of an earlier project's ColorMix data (e.g. a
        // calibration plate whose objects were deleted) doesn't apply to this model.
        plater_model.virtual_extruders = blends_after_delete_all(plater_model);
        plater_model.colormix          = data_after_delete_all(plater_model.colormix);
        // The imported states are the source slots themselves (slot k prints with tool k).
        paint_converted_from_source(imported);
        plater_model.colormix.source_colors = imported.colormix.source_colors;
        plater_model.colormix.slot_tools    = imported.colormix.slot_tools;
        return true;
    }
    std::vector<std::string>       &palette = plater_model.colormix.source_colors;
    const std::vector<std::string> &src     = imported.colormix.source_colors;
    // The conversion record: in a converted project the imported parts are painted as it paints.
    nlohmann::json options = nlohmann::json::parse(plater_model.colormix.options_json.empty() ? std::string("{}") : plater_model.colormix.options_json,
                                                   nullptr, false);
    std::vector<unsigned int> applied;
    if (options.is_object() && options.contains("applied_states") && options["applied_states"].is_array())
        for (const nlohmann::json &e : options["applied_states"])
            applied.push_back(e.is_number_unsigned() ? e.get<unsigned int>() : 0u);
    const bool converted = !plater_model.colormix.bundle_id.empty() && !applied.empty();
    // Slots and their colors for matching. A plater without a palette: its tools and blends keep
    // their ids as slots (their colors stay empty in the palette: the current extruder colors count
    // until a conversion; nothing is recorded on the plater's parts).
    std::vector<std::string> base       = palette;
    std::vector<std::string> base_match = palette;
    if (base.empty()) {
        const std::vector<std::string> colors = current_extruder_colors(plater_model, plater_extruder_colors);
        base.assign(std::min<size_t>(colors.size(), TRIANGLE_STATE_TYPE_COUNT), std::string());
        base_match.assign(colors.begin(), colors.begin() + base.size());
    }
    // The tool a slot prints with when not converted.
    const std::vector<unsigned int> &slot_tools = plater_model.colormix.slot_tools;
    auto tool_of = [&slot_tools](size_t k) { return k < slot_tools.size() && slot_tools[k] > 0 ? size_t(slot_tools[k]) : k; };
    // The tool a file slot prints with (a ColorMix project's own slot tools, else slot k = tool k);
    // beyond the printer's tools: the part's own extruder.
    const unsigned int num_tools = unsigned(plater_extruder_colors.size());
    auto file_tool = [&imported, num_tools](size_t s) -> unsigned int {
        const std::vector<unsigned int> &t = imported.colormix.slot_tools;
        const unsigned int tool = s < t.size() && t[s] > 0 ? t[s] : unsigned(s);
        return tool <= num_tools ? tool : ModelData::OWN_EXTRUDER;
    };
    const std::vector<bool> used_src = used_slots(imported.objects);
    const std::vector<bool> used_dst = used_slots(plater_model.objects);
    // New slots go after the palette and after every slot used on the plater.
    size_t next = base.size();
    for (size_t s = 1; s < used_dst.size(); ++s)
        if (used_dst[s])
            next = std::max(next, s + 1);
    std::vector<size_t>      to(used_src.size(), 0);
    std::vector<std::string> added;
    std::vector<size_t>      added_tool;
    for (size_t s = 1; s < used_src.size(); ++s) {
        if (!used_src[s])
            continue;
        const std::string hex = s < src.size() ? normalize_hex(src[s]) : std::string();
        size_t            slot = 0;
        if (!hex.empty()) {
            // Not converted: a slot is a tool, so only the same color on the same tool is the same slot.
            for (size_t k = 1; k < base_match.size() && slot == 0; ++k)
                if (normalize_hex(base_match[k]) == hex && (converted || tool_of(k) == file_tool(s)) &&
                    !(converted && (k >= applied.size() || applied[k] == 0)))
                    slot = k;
            for (size_t k = 0; k < added.size() && slot == 0; ++k)
                if (added[k] == hex && added_tool[k] == file_tool(s))
                    slot = next + k;
        }
        if (slot == 0) {
            slot = next + added.size();
            added.push_back(hex);
            added_tool.push_back(file_tool(s));
        }
        to[s] = slot;
    }
    if (next + added.size() > TRIANGLE_STATE_TYPE_COUNT)
        return false; // nothing changed
    paint_converted_from_source(imported);
    // A tool slot an imported color matched keeps that color.
    for (size_t s = 1; s < to.size(); ++s)
        if (to[s] > 0 && to[s] < base.size() && base[to[s]].empty() && s < src.size())
            base[to[s]] = normalize_hex(src[s]);
    if (!added.empty()) {
        base.resize(next, std::string());
        base.insert(base.end(), added.begin(), added.end());
    }
    palette = std::move(base);
    // A new slot prints with the tool of the file's slot (as the import assigns it).
    std::vector<unsigned int> &tools = plater_model.colormix.slot_tools;
    for (size_t s = 1; s < to.size(); ++s)
        if (to[s] >= next && file_tool(s) != to[s]) {
            if (tools.size() <= to[s])
                tools.resize(to[s] + 1, 0);
            tools[to[s]] = file_tool(s);
        }
    if (converted) {
        for (size_t s = 1; s < to.size(); ++s)
            if (to[s] >= next) {
                if (applied.size() <= to[s])
                    applied.resize(to[s] + 1, 0);
                // Beyond the tools: 0, the part's own extruder.
                applied[to[s]] = file_tool(s) == ModelData::OWN_EXTRUDER ? 0u : file_tool(s);
            }
        options["applied_states"]          = applied;
        plater_model.colormix.options_json = options.dump();
    }
    std::map<TriangleStateType, TriangleStateType> remap;
    for (size_t s = 1; s < to.size(); ++s)
        if (to[s] != 0 && to[s] != s)
            remap[static_cast<TriangleStateType>(s)] = static_cast<TriangleStateType>(to[s]);
    for (ModelObject *o : imported.objects)
        for (ModelVolume *v : o->volumes) {
            if (!v->is_model_part())
                continue;
            TriangleSelector source = source_selector(*v);
            // remap_states() looks every triangle up once, so sources and targets may overlap.
            if (!remap.empty())
                source.remap_states(remap);
            v->colormix_source_facets.set(source);
            if (converted) {
                // As the conversion paints; slots it doesn't map (e.g. beyond the tools) stay unpainted:
                // the part's own extruder, whatever it is set to (not the merge's temporary one).
                std::map<TriangleStateType, TriangleStateType> paint;
                const std::vector<bool> &used = v->colormix_source_facets.get_data().used_states;
                for (size_t st = 1; st < used.size(); ++st)
                    if (used[st]) {
                        const unsigned int e = st < applied.size() ? applied[st] : 0;
                        paint[static_cast<TriangleStateType>(st)] = e > 0 && e < TRIANGLE_STATE_TYPE_COUNT ? static_cast<TriangleStateType>(e) :
                                                                                                             TriangleStateType::NONE;
                    }
                source.remap_states(paint);
                v->mm_segmentation_facets.set(source);
            }
        }
    return true;
}

bool is_colormix_object(const ModelObject &object, const SliceSettings &settings, const FullSpectrum::VirtualExtruders &virtual_extruders)
{
    // Calibration plates / slope tests: their own objects only (not ones added next to them).
    if (settings.whole_part_regions &&
        (boost::algorithm::starts_with(object.name, "ColorMax calibration board") || boost::algorithm::starts_with(object.name, "ColorMax slope test") ||
         // older object names
         boost::algorithm::starts_with(object.name, "ColorMix calibration board") || boost::algorithm::starts_with(object.name, "ColorMix slope test")))
        return true;
    for (const ModelVolume *v : object.volumes) {
        if (!v->is_model_part())
            continue;
        if (v->has_colormix_source())
            return true;
        const std::vector<bool> &used = v->mm_segmentation_facets.get_data().used_states;
        for (const FullSpectrum::VirtualExtruder &ve : virtual_extruders)
            if (!ve.sequence.empty() && ve.id < used.size() && used[ve.id])
                return true;
    }
    return false;
}

size_t apply_pending_import_slots(Model &model, unsigned int num_tools)
{
    std::map<size_t, unsigned int> &pending = model.colormix.pending_import_slots;
    size_t                          beyond  = 0;
    // A slot beyond the printer's tools: kept as the part's source painting (its color as ColorMix
    // data); the part prints with its own extruder.
    std::vector<unsigned int> &slot_tools = model.colormix.slot_tools;
    auto keep_as_source = [&slot_tools](ModelVolume &v, unsigned int slot) {
        if (slot >= TRIANGLE_STATE_TYPE_COUNT || !v.is_model_part() || v.has_colormix_source())
            return;
        // The slot prints with the part's own extruder (not a tool or blend of that number).
        if (slot_tools.size() <= slot)
            slot_tools.resize(slot + 1, 0);
        if (slot_tools[slot] == 0)
            slot_tools[slot] = ModelData::OWN_EXTRUDER;
        TriangleSelector sel(v.mesh());
        if (v.is_mm_painted())
            sel.deserialize(v.mm_segmentation_facets.get_data(), false);
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(slot)}});
        v.colormix_source_facets.set(sel);
    };
    auto slot_of = [&pending](size_t id) -> unsigned int {
        const auto it = pending.find(id);
        return it == pending.end() ? 0 : it->second;
    };
    for (ModelObject *o : model.objects) {
        if (const unsigned int slot = slot_of(o->id().id); slot > 0) {
            if (slot <= num_tools)
                o->config.set("extruder", int(slot));
            else {
                ++beyond;
                for (ModelVolume *v : o->volumes)
                    if (!(v->config.has("extruder") && v->config.extruder() > 0))
                        keep_as_source(*v, slot);
            }
        }
        for (ModelVolume *v : o->volumes)
            if (const unsigned int slot = slot_of(v->id().id); slot > 0) {
                if (slot <= num_tools)
                    v->config.set("extruder", int(slot));
                else {
                    ++beyond;
                    keep_as_source(*v, slot);
                }
            }
    }
    pending.clear();
    return beyond;
}

void pending_slots_to_file(const Model &model, ModelData &out)
{
    const std::map<size_t, unsigned int> &pending = model.colormix.pending_import_slots;
    out.import_slots.clear();
    out.import_part_slots.clear();
    bool any_object = false, any_part = false;
    std::vector<unsigned int>              objects(model.objects.size(), 0);
    std::vector<std::vector<unsigned int>> parts(model.objects.size());
    for (size_t i = 0; i < model.objects.size(); ++i) {
        const ModelObject *o = model.objects[i];
        if (auto it = pending.find(o->id().id); it != pending.end() && it->second > 0) {
            objects[i] = it->second;
            any_object = true;
        }
        parts[i].assign(o->volumes.size(), 0);
        for (size_t k = 0; k < o->volumes.size(); ++k)
            if (auto it = pending.find(o->volumes[k]->id().id); it != pending.end() && it->second > 0) {
                parts[i][k] = it->second;
                any_part    = true;
            }
    }
    if (any_object)
        out.import_slots = std::move(objects);
    if (any_part)
        out.import_part_slots = std::move(parts);
}

void pending_slots_from_file(Model &model)
{
    ModelData &d = model.colormix;
    if (d.import_slots.size() == model.objects.size())
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (d.import_slots[i] > 0)
                d.pending_import_slots[model.objects[i]->id().id] = d.import_slots[i];
    if (d.import_part_slots.size() == model.objects.size())
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (const ModelObject *o = model.objects[i]; d.import_part_slots[i].size() == o->volumes.size())
                for (size_t k = 0; k < o->volumes.size(); ++k)
                    if (d.import_part_slots[i][k] > 0)
                        d.pending_import_slots[o->volumes[k]->id().id] = d.import_part_slots[i][k];
    d.import_slots.clear();
    d.import_part_slots.clear();
}

void copy_pending_slots(Model &from, Model &to, const std::vector<size_t> &to_idxs)
{
    std::map<size_t, unsigned int> &src = from.colormix.pending_import_slots;
    for (size_t i = 0; i < from.objects.size() && i < to_idxs.size() && !src.empty(); ++i) {
        if (to_idxs[i] >= to.objects.size())
            continue;
        const ModelObject *f = from.objects[i];
        const ModelObject *t = to.objects[to_idxs[i]];
        if (auto it = src.find(f->id().id); it != src.end()) {
            to.colormix.pending_import_slots[t->id().id] = it->second;
            src.erase(it);
        }
        for (size_t k = 0; k < f->volumes.size() && k < t->volumes.size(); ++k)
            if (auto it = src.find(f->volumes[k]->id().id); it != src.end()) {
                to.colormix.pending_import_slots[t->volumes[k]->id().id] = it->second;
                src.erase(it);
            }
    }
}

std::vector<unsigned int> multipart_pending_slots(const Model &model)
{
    // In the order convert_to_multipart_object() adds the parts: per object, per part, per instance.
    const std::map<size_t, unsigned int> &pending = model.colormix.pending_import_slots;
    std::vector<unsigned int>             out;
    for (const ModelObject *o : model.objects) {
        const auto         io          = pending.find(o->id().id);
        const unsigned int object_slot = io == pending.end() ? 0 : io->second;
        for (const ModelVolume *v : o->volumes) {
            const auto         iv   = pending.find(v->id().id);
            const unsigned int slot = iv == pending.end() ? object_slot : iv->second;
            out.insert(out.end(), std::max<size_t>(1, o->instances.size()), slot);
        }
    }
    return out;
}

void set_multipart_pending_slots(Model &model, const std::vector<unsigned int> &part_slots)
{
    model.colormix.pending_import_slots.clear();
    if (model.objects.size() != 1 || model.objects.front()->volumes.size() != part_slots.size())
        return;
    for (size_t k = 0; k < part_slots.size(); ++k)
        if (part_slots[k] > 0)
            model.colormix.pending_import_slots[model.objects.front()->volumes[k]->id().id] = part_slots[k];
}

void remap_conversion_record(ModelData &data, const std::map<unsigned int, unsigned int> &id_remap, unsigned int num_physical)
{
    nlohmann::json o = nlohmann::json::parse(data.options_json.empty() ? std::string("{}") : data.options_json, nullptr, false);
    if (!o.is_object())
        o = nlohmann::json::object();
    bool changed = false;
    if (auto it = o.find("applied_states"); it != o.end() && it->is_array())
        for (auto &v : *it)
            if (v.is_number_unsigned())
                if (auto r = id_remap.find(v.get<unsigned int>()); r != id_remap.end()) {
                    v       = r->second;
                    changed = true;
                }
    for (unsigned int &t : data.slot_tools)
        if (auto r = id_remap.find(t); r != id_remap.end() && t != ModelData::OWN_EXTRUDER) {
            t       = r->second;
            changed = true;
        }
    if (o.contains("num_physical") && o["num_physical"] != int(num_physical)) {
        o["num_physical"] = int(num_physical);
        changed           = true;
    }
    if (changed)
        data.options_json = o.empty() ? std::string() : o.dump();
}

namespace {
// Source slot -> the tool it prints with when not converted: slot_tools, else slot k is tool k.
// Only tools and the given (non-ColorMix) blends: anything else (a ColorMix blend that goes away
// with the conversion, a file's slot beyond the tools) maps to 0, the volume's own extruder.
std::vector<unsigned int> source_slot_tools(const Model &model, const FullSpectrum::VirtualExtruders &blends, unsigned int num_physical)
{
    std::set<unsigned int> valid;
    for (const FullSpectrum::VirtualExtruder &ve : blends)
        if (ve.sequence.empty())
            valid.insert(ve.id);
    const std::vector<unsigned int> &tools = model.colormix.slot_tools;
    const size_t n = std::min<size_t>(TRIANGLE_STATE_TYPE_COUNT, std::max(tools.size(), model.colormix.source_colors.size()));
    std::vector<unsigned int> out(n, 0);
    for (size_t s = 1; s < n; ++s) {
        if (s < tools.size() && tools[s] == ModelData::OWN_EXTRUDER)
            continue; // 0: the part's own extruder
        const unsigned int t = s < tools.size() && tools[s] > 0 && tools[s] < TRIANGLE_STATE_TYPE_COUNT ? tools[s] : unsigned(s);
        out[s] = t <= num_physical || valid.count(t) ? t : 0;
    }
    return out;
}
} // namespace

std::vector<unsigned int> expected_states(const Model &model, unsigned int num_physical)
{
    const nlohmann::json o = nlohmann::json::parse(model.colormix.options_json.empty() ? std::string("{}") : model.colormix.options_json, nullptr, false);
    if (o.is_object() && o.contains("applied_states") && o["applied_states"].is_array()) {
        std::vector<unsigned int> applied;
        for (const nlohmann::json &e : o["applied_states"])
            applied.push_back(e.is_number_unsigned() ? e.get<unsigned int>() : 0u);
        return applied;
    }
    // Converted by a build that kept no record (no "num_physical" either): nothing to compare with.
    if (!model.colormix.bundle_id.empty() && !(o.is_object() && o.contains("num_physical")))
        return {};
    return source_slot_tools(model, model.virtual_extruders, num_physical);
}

void revert_to_source(Model &model, unsigned int num_physical)
{
    nlohmann::json options = nlohmann::json::parse(model.colormix.options_json.empty() ? std::string("{}") : model.colormix.options_json, nullptr, false);
    if (!options.is_object())
        options = nlohmann::json::object();
    // Source slot -> tool: slot k is tool k (the plater's tools, an imported file's slot k), except
    // slots added later (parts added after a conversion, merged imports, kept touch-ups): those go
    // back to the tool they printed with.
    // (Checked against the blends the project has after the revert.)
    FullSpectrum::VirtualExtruders after_revert = model.virtual_extruders;
    if (options.contains("original_virtual_extruders") && options["original_virtual_extruders"].is_string())
        after_revert = FullSpectrum::deserialize_virtual_extruders_from_json(options["original_virtual_extruders"].get<std::string>()).virtual_extruders;
    const std::vector<unsigned int> to_tools = source_slot_tools(model, after_revert, num_physical);
    // The project's own virtual extruders and extruder settings from before the first conversion.
    if (options.contains("original_virtual_extruders")) {
        FullSpectrum::VirtualExtruders original;
        if (options["original_virtual_extruders"].is_string())
            original = FullSpectrum::deserialize_virtual_extruders_from_json(options["original_virtual_extruders"].get<std::string>()).virtual_extruders;
        if (options.contains("original_extruders") && options["original_extruders"].is_array())
            for (const nlohmann::json &e : options["original_extruders"]) {
                if (!e.is_array() || e.size() < 5)
                    continue;
                const size_t oi = e[0].get<size_t>();
                const int    vi = e[1].get<int>(), value = e[2].get<int>();
                if (oi >= model.objects.size())
                    continue;
                ModelObject *o = model.objects[oi];
                // Only onto the same object (objects may have been added or deleted since).
                if (e.size() >= 7 && (o->name != e[5].get<std::string>() || o->volumes.size() != e[6].get<size_t>()))
                    continue;
                ModelConfig *cfg = nullptr;
                if (vi == -1)
                    cfg = &o->config;
                else if (vi >= 0 && size_t(vi) < o->volumes.size())
                    cfg = &o->volumes[size_t(vi)]->config;
                else if (vi == -2)
                    for (auto &[range, c] : o->layer_config_ranges)
                        if (std::abs(range.first - e[3].get<double>()) < 1e-6 && std::abs(range.second - e[4].get<double>()) < 1e-6)
                            cfg = &c;
                // Only where the conversion's tool 1 is still set (not changed by the user since).
                if (cfg != nullptr && cfg->has("extruder") && cfg->opt_int("extruder") == 1)
                    cfg->set("extruder", value);
            }
        restore_virtual_extruder_colors(original, options);
        options.erase("original_virtual_extruders");
        options.erase("original_virtual_extruder_colors");
        options.erase("original_extruders");
        model.colormix.options_json = options.empty() ? std::string() : options.dump();
        model.virtual_extruders     = std::move(original);
    } else
        // No conversion record: only ColorMix's own blends go.
        model.virtual_extruders.erase(std::remove_if(model.virtual_extruders.begin(), model.virtual_extruders.end(),
                                                     [](const FullSpectrum::VirtualExtruder &ve) { return !ve.sequence.empty(); }),
                                      model.virtual_extruders.end());
    model.colormix.slice = SliceSettings();
    // The source painting, in tools. A part all in its own extruder's color is unpainted again (as it
    // was before the conversion: e.g. XY size compensation applies to it again).
    for (ModelObject *o : model.objects)
        for (ModelVolume *v : o->volumes)
            if (v->is_model_part() && v->has_colormix_source()) {
                const TriangleSelector                        sel  = converted_painting(*v, to_tools, num_physical);
                const TriangleSelector::TriangleSplittingData data = sel.serialize();
                size_t       n_states = 0;
                unsigned int only     = 0;
                for (size_t st = 0; st < data.used_states.size(); ++st)
                    if (data.used_states[st]) {
                        ++n_states;
                        only = unsigned(st);
                    }
                if (n_states == 1 && only == std::min<unsigned>(base_extruder(*v), TRIANGLE_STATE_TYPE_COUNT - 1))
                    v->mm_segmentation_facets.set(TriangleSelector(v->mesh()));
                else
                    v->mm_segmentation_facets.set(sel);
            }
}

FullSpectrum::VirtualExtruders blends_after_delete_all(const Model &model)
{
    FullSpectrum::VirtualExtruders out;
    const nlohmann::json o = nlohmann::json::parse(model.colormix.options_json.empty() ? std::string("{}") : model.colormix.options_json, nullptr, false);
    if (o.is_object() && o.contains("original_virtual_extruders") && o["original_virtual_extruders"].is_string()) {
        out = FullSpectrum::deserialize_virtual_extruders_from_json(o["original_virtual_extruders"].get<std::string>()).virtual_extruders;
        restore_virtual_extruder_colors(out, o);
    } else
        out = model.virtual_extruders;
    out.erase(std::remove_if(out.begin(), out.end(), [](const FullSpectrum::VirtualExtruder &ve) { return !ve.sequence.empty(); }), out.end());
    return out;
}

ModelData data_after_delete_all(const ModelData &data)
{
    ModelData out;
    out.unreadable_json = data.unreadable_json;
    const nlohmann::json o = nlohmann::json::parse(data.options_json.empty() ? std::string("{}") : data.options_json, nullptr, false);
    if (o.is_object()) {
        nlohmann::json kept = nlohmann::json::object();
        for (const char *key : {"original_extruder_colors", "extruder_colors"})
            if (o.contains(key))
                kept[key] = o[key];
        if (!kept.empty())
            out.options_json = kept.dump();
    }
    return out;
}

} // namespace Slic3r::ColorMix
