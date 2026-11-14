///|/ ColorMax engine - looking up filaments in public filament databases.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "FilamentDb.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

#include <nlohmann/json.hpp>

#include "ColorSpace.hpp"

namespace Slic3r::ColorMix {

using json = nlohmann::json;

namespace {

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower((unsigned char) c));
    return s;
}

// Letters and digits only, lower case: "Bambu Lab" == "bambulab".
std::string key(const std::string &s)
{
    std::string out;
    for (unsigned char c : s)
        if (std::isalnum(c))
            out += char(std::tolower(c));
    return out;
}

std::string str(const json &j, const char *k)
{
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double num(const json &j, const char *k)
{
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : 0.;
}

bool looks_like_effect(const std::string &text)
{
    const std::string t = lower(text);
    for (const char *w : {"silk", "glitter", "sparkle", "galaxy", "starlight", "marble", "metallic", "shimmer", "rainbow", "dual color",
                          "tri color", "tricolor", "multicolor", "multi-color", "gradient", "glow"})
        if (t.find(w) != std::string::npos)
            return true;
    return false;
}

} // namespace

std::vector<FilamentRecord> parse_filamentcolors(const std::string &text, std::string *next_page)
{
    std::vector<FilamentRecord> out;
    if (next_page)
        next_page->clear();
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception &) {
        return out;
    }
    const json *list = &j;
    if (j.is_object()) {
        if (next_page)
            *next_page = str(j, "next");
        auto it = j.find("results");
        if (it == j.end() || !it->is_array())
            return out;
        list = &*it;
    }
    if (!list->is_array())
        return out;
    for (const json &r : *list) {
        if (!r.is_object())
            continue;
        FilamentRecord f;
        f.source = "filamentcolors.xyz";
        if (auto m = r.find("manufacturer"); m != r.end() && m->is_object())
            f.manufacturer = str(*m, "name");
        if (auto t = r.find("filament_type"); t != r.end() && t->is_object()) {
            f.material = str(*t, "name");
            // Specialty types ("Silk PLA", "Panchroma Starlight") under a parent type ("PLA").
            if (auto p = t->find("parent_type"); p != t->end() && p->is_object() && !str(*p, "name").empty() && looks_like_effect(f.material))
                f.effect = true;
        }
        f.name = str(r, "color_name");
        // The colorimeter's Lab when given (exact), else the hex made from it.
        const double L = num(r, "lab_l"), a = num(r, "lab_a"), b = num(r, "lab_b");
        if (r.contains("lab_l") && r["lab_l"].is_number() && L > 0.)
            f.color = lab_to_hex({L, a, b});
        else
            f.color = normalize_hex(str(r, "hex_color"));
        f.measured = !f.color.empty();
        f.td       = std::max(0., num(r, "td"));
        f.effect   = f.effect || looks_like_effect(f.name);
        f.translucent = lower(f.name).find("transparent") != std::string::npos || lower(f.name).find("translucent") != std::string::npos ||
                        f.td >= 10.;
        if (const std::string slug = str(r, "slug"); !slug.empty())
            f.url = "https://filamentcolors.xyz/swatch/" + slug + "/";
        if (!f.color.empty() && !f.name.empty())
            out.push_back(std::move(f));
    }
    return out;
}

std::vector<FilamentRecord> parse_spoolmandb(const std::string &text)
{
    std::vector<FilamentRecord> out;
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception &) {
        return out;
    }
    if (!j.is_array())
        return out;
    std::set<std::tuple<std::string, std::string, std::string, std::string>> seen;
    for (const json &r : j) {
        if (!r.is_object())
            continue;
        FilamentRecord f;
        f.source       = "SpoolmanDB";
        f.manufacturer = str(r, "manufacturer");
        f.name         = str(r, "name");
        f.material     = str(r, "material");
        std::string hex = str(r, "color_hex");
        bool multi = false;
        if (hex.empty())
            if (auto hs = r.find("color_hexes"); hs != r.end() && hs->is_array() && !hs->empty() && hs->front().is_string()) {
                hex   = hs->front().get<std::string>();
                multi = hs->size() > 1;
            }
        f.color = normalize_hex(hex.substr(0, 6));
        f.td    = std::max(0., num(r, "transmission_distance"));
        if (auto t = r.find("translucent"); t != r.end() && t->is_boolean())
            f.translucent = t->get<bool>();
        const std::string pattern = str(r, "pattern"), finish = str(r, "finish");
        f.effect = multi || !pattern.empty() || looks_like_effect(f.name) || looks_like_effect(finish);
        f.url    = "https://github.com/Donkie/SpoolmanDB";
        if (f.color.empty() || f.name.empty())
            continue;
        // One record per weight and diameter in the source: keep one.
        if (!seen.insert({key(f.manufacturer), key(f.name), key(f.material), f.color}).second)
            continue;
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<FilamentRecord> merge_filament_records(std::vector<FilamentRecord> measured, const std::vector<FilamentRecord> &catalog)
{
    std::set<std::tuple<std::string, std::string, std::string>> have;
    for (const FilamentRecord &f : measured)
        have.insert({key(f.manufacturer), key(f.name), key(f.material)});
    for (const FilamentRecord &f : catalog)
        if (!have.count({key(f.manufacturer), key(f.name), key(f.material)}))
            measured.push_back(f);
    return measured;
}

std::string filament_db_to_json(const std::vector<FilamentRecord> &records, const std::string &downloaded)
{
    json list = json::array();
    for (const FilamentRecord &f : records) {
        json r{{"source", f.source}, {"manufacturer", f.manufacturer}, {"name", f.name}, {"material", f.material}, {"color", f.color}};
        if (f.measured)    r["measured"] = true;
        if (f.td > 0.)     r["td"] = f.td;
        if (f.translucent) r["translucent"] = true;
        if (f.effect)      r["effect"] = true;
        if (!f.url.empty()) r["url"] = f.url;
        list.push_back(std::move(r));
    }
    return json{{"format", "colormax-filament-db"}, {"version", 1}, {"downloaded", downloaded}, {"records", std::move(list)}}.dump();
}

std::vector<FilamentRecord> filament_db_from_json(const std::string &text, std::string *downloaded)
{
    std::vector<FilamentRecord> out;
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception &) {
        return out;
    }
    if (!j.is_object() || str(j, "format") != "colormax-filament-db")
        return out;
    if (downloaded)
        *downloaded = str(j, "downloaded");
    auto it = j.find("records");
    if (it == j.end() || !it->is_array())
        return out;
    for (const json &r : *it) {
        FilamentRecord f;
        f.source       = str(r, "source");
        f.manufacturer = str(r, "manufacturer");
        f.name         = str(r, "name");
        f.material     = str(r, "material");
        f.color        = normalize_hex(str(r, "color"));
        f.measured     = r.value("measured", false);
        f.td           = std::max(0., num(r, "td"));
        f.translucent  = r.value("translucent", false);
        f.effect       = r.value("effect", false);
        f.url          = str(r, "url");
        if (!f.color.empty())
            out.push_back(std::move(f));
    }
    return out;
}

std::vector<size_t> search_filaments(const std::vector<FilamentRecord> &records, const std::string &query, size_t max_results)
{
    std::vector<std::string> words;
    {
        std::string w;
        for (unsigned char c : query + " ") {
            if (std::isspace(c)) {
                if (!w.empty())
                    words.push_back(lower(w));
                w.clear();
            } else
                w += char(c);
        }
    }
    auto tokens = [](const std::string &text) {
        std::vector<std::string> out;
        std::string              t;
        for (unsigned char c : text + " ") {
            if (std::isalnum(c) || c == '+')
                t += char(std::tolower(c));
            else if (!t.empty()) {
                out.push_back(t);
                t.clear();
            }
        }
        return out;
    };
    std::vector<std::pair<int, size_t>> hits;
    for (size_t i = 0; i < records.size(); ++i) {
        const FilamentRecord          &f    = records[i];
        const std::vector<std::string> hay  = tokens(f.manufacturer + " " + f.name + " " + f.material + (f.measured ? " measured" : " catalog") +
                                                     (f.effect ? " effect" : "") + (f.translucent ? " translucent" : ""));
        const std::vector<std::string> name = tokens(f.name);
        bool all = true;
        int  score = f.measured ? 0 : 1000;
        for (const std::string &w : words) {
            // Every word of the query starts a word of the record.
            if (std::none_of(hay.begin(), hay.end(), [&](const std::string &t) { return t.rfind(w, 0) == 0; })) {
                all = false;
                break;
            }
            if (std::none_of(name.begin(), name.end(), [&](const std::string &t) { return t.rfind(w, 0) == 0; }))
                score += 10; // words found in the color name rank first
        }
        if (all)
            hits.push_back({score + int(std::min<size_t>(f.name.size(), 99)), i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<size_t> out;
    for (size_t k = 0; k < hits.size() && k < max_results; ++k)
        out.push_back(hits[k].second);
    return out;
}

} // namespace Slic3r::ColorMix
