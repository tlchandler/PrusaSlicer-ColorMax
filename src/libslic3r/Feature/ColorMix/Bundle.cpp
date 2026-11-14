///|/ ColorMax engine - filament bundles.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "Bundle.hpp"

#include <cctype>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r::ColorMix {

namespace {
constexpr const char *FORMAT_TAG     = "prusaslicer-colormix-bundle";
constexpr int         FORMAT_VERSION = 1;

using json = nlohmann::json;

Lab lab_from_json(const json &j)
{
    if (j.is_array() && j.size() == 3)
        return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
    if (j.is_object())
        return {j.value("L", 0.), j.value("a", 0.), j.value("b", 0.)};
    if (j.is_string()) {
        const std::string hex = normalize_hex(j.get<std::string>());
        if (hex.empty())
            throw std::runtime_error("invalid color \"" + j.get<std::string>() + "\" (expected #RRGGBB)");
        return hex_to_lab(hex);
    }
    throw std::runtime_error("color must be [L,a,b], {\"L\",\"a\",\"b\"} or \"#RRGGBB\"");
}
} // namespace

unsigned int white_tool(const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament)
{
    const int w = bundle.filament_index("white");
    return w >= 0 && size_t(w) < tool_of_filament.size() ? tool_of_filament[size_t(w)] : 0;
}

std::vector<unsigned int> allowed_core_tools(const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament,
                                             unsigned int num_physical)
{
    const unsigned int        white = white_tool(bundle, tool_of_filament);
    std::vector<unsigned int> out;
    for (unsigned int t = 1; t <= num_physical; ++t) {
        bool holds_bundle_filament = false;
        for (size_t f = 0; f < tool_of_filament.size() && f < bundle.filaments.size(); ++f)
            holds_bundle_filament |= tool_of_filament[f] == t;
        if (t == white || !holds_bundle_filament)
            out.push_back(t);
    }
    return out;
}

std::string core_tool_color(const Bundle &bundle, const std::vector<unsigned int> &tool_of_filament, unsigned int core_tool)
{
    const int w = bundle.filament_index("white");
    if (w >= 0 && core_tool > 0 && white_tool(bundle, tool_of_filament) == core_tool)
        return bundle.filaments[size_t(w)].color;
    return "#FFFFFF";
}

std::string make_id(const std::string &name, const std::vector<std::string> &taken, const std::string &fallback)
{
    std::string id;
    for (unsigned char c : name) {
        if (std::isalnum(c))
            id += char(std::tolower(c));
        else if (!id.empty() && id.back() != '-')
            id += '-';
    }
    while (!id.empty() && id.back() == '-')
        id.pop_back();
    if (id.empty())
        id = fallback;
    const auto used = [&](const std::string &v) { return std::find(taken.begin(), taken.end(), v) != taken.end(); };
    if (!used(id))
        return id;
    for (int k = 2;; ++k)
        if (const std::string v = id + "-" + std::to_string(k); !used(v))
            return v;
}

std::string check_bundle(const Bundle &b)
{
    if (b.name.find_first_not_of(" \t") == std::string::npos)
        return "Give the filament set a name.";
    if (b.id.empty())
        return "The filament set has no id.";
    int mixable = 0;
    for (size_t i = 0; i < b.filaments.size(); ++i) {
        const BundleFilament &f = b.filaments[i];
        if (f.name.find_first_not_of(" \t") == std::string::npos)
            return "Filament " + std::to_string(i + 1) + " has no name.";
        if (normalize_hex(f.color).empty())
            return "\"" + f.name + "\" has no valid color (#RRGGBB).";
        if (f.id.empty())
            return "\"" + f.name + "\" has no id.";
        for (size_t j = 0; j < i; ++j) {
            if (b.filaments[j].id == f.id)
                return "Two filaments have the id \"" + f.id + "\".";
            if (b.filaments[j].name == f.name)
                return "Two filaments are called \"" + f.name + "\".";
        }
        mixable += f.mixes() ? 1 : 0;
    }
    if (mixable < 2)
        return "A filament set needs at least two filaments set to \"Mix\" (accents and solid-only filaments don't count).";
    return std::string();
}

int Bundle::filament_index(const std::string &fid) const
{
    for (size_t i = 0; i < filaments.size(); ++i)
        if (filaments[i].id == fid)
            return int(i);
    return -1;
}

std::vector<int> Bundle::mixable_indices() const
{
    std::vector<int> out;
    for (size_t i = 0; i < filaments.size(); ++i)
        if (filaments[i].mixes())
            out.push_back(int(i));
    return out;
}

const char *mix_role_key(MixRole role)
{
    switch (role) {
    case MixRole::Accent: return "accent";
    case MixRole::Solid: return "solid";
    default: return "mix";
    }
}

MixRole mix_role_from_key(const std::string &key)
{
    if (key == "accent")
        return MixRole::Accent;
    if (key == "solid")
        return MixRole::Solid;
    return MixRole::Mix;
}

std::optional<Bundle> parse_bundle(const std::string &text, std::string *error)
{
    try {
        const json root = json::parse(text);
        if (root.value("format", std::string()) != FORMAT_TAG)
            throw std::runtime_error(std::string("missing \"format\": \"") + FORMAT_TAG + "\"");
        if (root.value("version", 0) > FORMAT_VERSION)
            throw std::runtime_error("bundle format version is newer than this PrusaSlicer supports");
        Bundle b;
        b.id          = root.at("id").get<std::string>();
        b.name        = root.value("name", b.id);
        b.material    = root.value("material", std::string("PLA"));
        b.description = root.value("description", std::string());
        for (const json &f : root.at("filaments")) {
            BundleFilament bf;
            bf.id    = f.at("id").get<std::string>();
            bf.name  = f.value("name", bf.id);
            bf.color = normalize_hex(f.at("color").get<std::string>());
            if (bf.color.empty())
                throw std::runtime_error("filament \"" + bf.id + "\" has an invalid color");
            {
                const int t = f.value("default_tool", 0);
                if (t < 0 || t > 64)
                    throw std::runtime_error("filament \"" + bf.id + "\": default_tool out of range");
                bf.default_tool = unsigned(t);
            }
            // "role"; files of earlier builds: "mixable" and "effect" (an effect filament never mixed).
            if (f.contains("role") && f["role"].is_string())
                bf.role = mix_role_from_key(f["role"].get<std::string>());
            else
                bf.role = f.value("effect", false) || !f.value("mixable", true) ? MixRole::Solid : MixRole::Mix;
            bf.filament_preset = f.value("filament_preset", std::string());
            bf.td              = std::max(0., f.value("td", 0.));
            if (b.filament_index(bf.id) >= 0)
                throw std::runtime_error("duplicate filament id \"" + bf.id + "\"");
            b.filaments.push_back(std::move(bf));
        }
        if (b.filaments.empty())
            throw std::runtime_error("bundle has no filaments");
        if (root.contains("rules")) {
            const json &r            = root["rules"];
            b.rules.max_run          = std::clamp(r.value("max_run", b.rules.max_run), 1, 16);
            b.rules.recurrence_window = std::clamp(r.value("recurrence_window", b.rules.recurrence_window), 1, 64);
            // Longer words band visibly anyway, and the number of words explodes beyond this.
            b.rules.max_word_length  = std::clamp(r.value("max_word_length", b.rules.max_word_length), 1, 12);
            b.rules.max_filaments    = std::clamp(r.value("max_filaments", b.rules.max_filaments), 1, 4);
        }
        if (root.contains("calibrations")) {
            for (const json &c : root["calibrations"]) {
                Calibration cal;
                cal.id                 = c.value("id", std::string("calibration"));
                cal.date               = c.value("date", std::string());
                cal.printer            = c.value("printer", std::string());
                cal.notes              = c.value("notes", std::string());
                cal.first_layer_height = c.value("first_layer_height", 0.2);
                cal.white_core         = c.value("white_core", false);
                cal.wall_depth         = c.value("wall_depth", 0.);
                cal.dual_wall_width    = c.value("dual_wall_width", 0.);
                cal.plate_file         = c.value("plate_file", std::string());
                if (!(cal.first_layer_height >= 0.02 && cal.first_layer_height <= 1.))
                    throw std::runtime_error("calibration \"" + cal.id + "\": first_layer_height out of range");
                if (c.contains("layer_schedule"))
                    cal.layer_schedule = c["layer_schedule"].get<std::vector<double>>();
                if (cal.layer_schedule.empty())
                    cal.layer_schedule = {c.value("layer_height", 0.1)};
                for (double h : cal.layer_schedule)
                    if (!(h > 0.01 && h < 1.))
                        throw std::runtime_error("calibration \"" + cal.id + "\" has an invalid layer height");
                if (c.contains("swatches"))
                    for (const json &s : c["swatches"]) {
                        Swatch sw;
                        if (s.contains("word"))
                            sw.word = s["word"].get<std::vector<std::string>>();
                        if (s.contains("inner_word"))
                            sw.inner_word = s["inner_word"].get<std::vector<std::string>>();
                        for (const std::string &fid : sw.inner_word)
                            if (b.filament_index(fid) < 0)
                                throw std::runtime_error("swatch uses unknown filament \"" + fid + "\"");
                        if (s.contains("ratios"))
                            for (auto it = s["ratios"].begin(); it != s["ratios"].end(); ++it)
                                sw.ratios.emplace_back(it.key(), it.value().get<double>());
                        if (sw.word.empty() && sw.ratios.empty())
                            throw std::runtime_error("swatch without \"word\" or \"ratios\"");
                        for (const std::string &fid : sw.word)
                            if (b.filament_index(fid) < 0)
                                throw std::runtime_error("swatch uses unknown filament \"" + fid + "\"");
                        for (const auto &[fid, r] : sw.ratios)
                            if (b.filament_index(fid) < 0)
                                throw std::runtime_error("swatch uses unknown filament \"" + fid + "\"");
                        sw.measured = lab_from_json(s.at("measured"));
                        sw.held_out = s.value("held_out", false);
                        sw.note     = s.value("note", std::string());
                        cal.swatches.push_back(std::move(sw));
                    }
                b.calibrations.push_back(std::move(cal));
            }
        }
        return b;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    }
}

std::string bundle_to_json(const Bundle &b)
{
    json root;
    root["format"]      = FORMAT_TAG;
    root["version"]     = FORMAT_VERSION;
    root["id"]          = b.id;
    root["name"]        = b.name;
    root["material"]    = b.material;
    if (!b.description.empty())
        root["description"] = b.description;
    json fil = json::array();
    for (const BundleFilament &f : b.filaments) {
        // "mixable" / "effect" too, so earlier builds read the file the way they can.
        json j{{"id", f.id}, {"name", f.name}, {"color", f.color}, {"role", mix_role_key(f.role)}, {"mixable", f.mixes()}, {"effect", !f.mixes()}};
        if (f.default_tool > 0)
            j["default_tool"] = f.default_tool;
        if (!f.filament_preset.empty())
            j["filament_preset"] = f.filament_preset;
        if (f.td > 0.)
            j["td"] = f.td;
        fil.push_back(std::move(j));
    }
    root["filaments"] = std::move(fil);
    root["rules"]     = {{"max_run", b.rules.max_run},
                         {"recurrence_window", b.rules.recurrence_window},
                         {"max_word_length", b.rules.max_word_length},
                         {"max_filaments", b.rules.max_filaments}};
    json cals = json::array();
    for (const Calibration &c : b.calibrations) {
        json jc{{"id", c.id}, {"first_layer_height", c.first_layer_height}, {"layer_schedule", c.layer_schedule}};
        if (c.white_core)
            jc["white_core"] = true;
        if (c.wall_depth > 0.)
            jc["wall_depth"] = c.wall_depth;
        if (c.dual_wall_width > 0.)
            jc["dual_wall_width"] = c.dual_wall_width;
        if (!c.date.empty())    jc["date"] = c.date;
        if (!c.printer.empty()) jc["printer"] = c.printer;
        if (!c.notes.empty())   jc["notes"] = c.notes;
        if (!c.plate_file.empty()) jc["plate_file"] = c.plate_file;
        json sw = json::array();
        for (const Swatch &s : c.swatches) {
            json js;
            if (!s.word.empty())
                js["word"] = s.word;
            if (!s.inner_word.empty())
                js["inner_word"] = s.inner_word;
            if (!s.ratios.empty()) {
                json r = json::object();
                for (const auto &[fid, v] : s.ratios)
                    r[fid] = v;
                js["ratios"] = r;
            }
            js["measured"] = {s.measured.L, s.measured.a, s.measured.b};
            if (s.held_out)
                js["held_out"] = true;
            if (!s.note.empty())
                js["note"] = s.note;
            sw.push_back(std::move(js));
        }
        jc["swatches"] = std::move(sw);
        cals.push_back(std::move(jc));
    }
    root["calibrations"] = std::move(cals);
    return root.dump(2);
}

std::optional<Bundle> load_bundle(const std::string &path, std::string *error)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error)
            *error = "cannot open " + path;
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::optional<Bundle> b = parse_bundle(ss.str(), error);
    if (b)
        b->source_path = path;
    return b;
}

bool save_bundle(const Bundle &bundle, const std::string &path, std::string *error)
{
    try {
        boost::filesystem::create_directories(boost::filesystem::path(path).parent_path());
        // Written next to the target and renamed over it: a failed write never loses the old file.
        const std::string tmp = path + ".tmp";
        {
            boost::nowide::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
                throw std::runtime_error("cannot write " + tmp);
            out << bundle_to_json(bundle);
            out.close();
            if (!out)
                throw std::runtime_error("cannot write " + tmp);
        }
        boost::filesystem::rename(tmp, path);
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

std::vector<Bundle> load_bundles(const std::string &system_dir, const std::string &user_dir, std::vector<std::string> *errors)
{
    std::map<std::string, Bundle> by_id;
    std::vector<std::string>      order;
    auto scan = [&](const std::string &dir, bool user) {
        namespace fs = boost::filesystem;
        boost::system::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec))
            return;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (fs::is_regular_file(it->path(), ec) && it->path().extension() == ".json")
                files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path &p : files) {
            std::string err;
            std::optional<Bundle> b = load_bundle(p.string(), &err);
            if (!b) {
                BOOST_LOG_TRIVIAL(error) << "ColorMax filament set " << p.string() << ": " << err;
                if (errors)
                    errors->push_back(p.filename().string() + ": " + err);
                continue;
            }
            b->user_bundle = user;
            if (by_id.find(b->id) == by_id.end())
                order.push_back(b->id);
            by_id[b->id] = std::move(*b);
        }
    };
    scan(system_dir, false);
    scan(user_dir, true);
    if (by_id.find("prusament-pla-colormix") == by_id.end()) {
        order.insert(order.begin(), "prusament-pla-colormix");
        by_id["prusament-pla-colormix"] = builtin_colormix_bundle();
    }
    std::vector<Bundle> out;
    for (const std::string &id : order)
        out.push_back(std::move(by_id[id]));
    return out;
}

Bundle builtin_colormix_bundle()
{
    Bundle b;
    b.id          = "prusament-pla-colormix";
    b.name        = "Prusament PLA ColorMix bundle";
    b.material    = "PLA";
    b.description = "Prusament PLA ColorMix bundle 7x1 kg. Nominal colors from the OpenPrintTag database; "
                    "uncalibrated until you add measured swatches.";
    b.filaments = {
        {"cyan", "ColorMix Cyan", "#00AED5", 1, MixRole::Mix, {}},
        {"magenta", "ColorMix Magenta", "#D25B96", 2, MixRole::Mix, {}},
        {"yellow", "ColorMix Yellow", "#FFCE02", 3, MixRole::Mix, {}},
        {"white", "ColorMix White", "#F1E8DD", 4, MixRole::Mix, {}},
        {"black", "ColorMix Black", "#544F4D", 5, MixRole::Mix, {}},
        // Clear PLA with glitter, made to be layered with the other colors for sparkle (Prusa).
        {"gold-glitter", "ColorMix Gold Glitter", "#DDCAA9", 6, MixRole::Accent, {}},
        {"silver-glitter", "ColorMix Silver Glitter", "#A4A09F", 7, MixRole::Accent, {}},
    };
    b.rules = RecipeRules{3, 4, 6, 3};
    return b;
}

} // namespace Slic3r::ColorMix
