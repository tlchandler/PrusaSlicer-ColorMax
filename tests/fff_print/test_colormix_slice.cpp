// End-to-end tests of the ColorMix engine slicing techniques: explicit layer words, the white core,
// top-surface patches and the variable layer schedule, plus 3MF persistence of the project data.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/nowide/fstream.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <cstdio>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Feature/ColorMix/Bundle.hpp"
#include "libslic3r/Feature/ColorMix/ModelConvert.hpp"
#include "libslic3r/ModelProcessing.hpp"

#include "test_data.hpp"
#include <nlohmann/json.hpp>

using namespace Slic3r;
using Catch::Approx;

namespace {

DynamicPrintConfig five_tool_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.4,0.4,0.4,0.4,0.4"},
        {"extruder_colour", "#00AED5;#D25B96;#FFCE02;#F1E8DD;#544F4D"},
        {"filament_colour", "#00AED5;#D25B96;#FFCE02;#F1E8DD;#544F4D"},
        {"wipe_tower", "0"},
        {"layer_height", "0.1"},
        {"first_layer_height", "0.2"},
        {"min_layer_height", "0.05,0.05,0.05,0.05,0.05"},
        {"max_layer_height", "0.25,0.25,0.25,0.25,0.25"},
        {"perimeters", "2"},
        {"top_solid_layers", "3"},
        {"bottom_solid_layers", "3"},
        {"fill_density", "15%"},
        {"skirts", "0"},
    });
    return config;
}

// A 20 mm cube painted entirely with extruder `state`.
ModelObject *add_painted_cube(Model &model, unsigned int state, double size = 20.)
{
    ModelObject *obj = model.add_object();
    obj->name        = "cube";
    ModelVolume *vol = obj->add_volume(TriangleMesh(its_make_cube(size, size, size)));
    obj->add_instance();
    obj->center_around_origin(false);
    obj->instances.front()->set_offset(Vec3d(100., 100., 0.));
    obj->ensure_on_bed();
    TriangleSelector sel(vol->mesh());
    sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(state)}});
    vol->mm_segmentation_facets.set(sel);
    return obj;
}

// Extruders (1-based) that own slices on a layer, with their area in mm^2.
std::map<unsigned int, double> layer_extruders(const Layer &layer)
{
    std::map<unsigned int, double> out;
    for (const LayerRegion *lr : layer.regions()) {
        double area = 0.;
        for (const Surface &s : lr->slices().surfaces)
            area += s.expolygon.area() * SCALING_FACTOR * SCALING_FACTOR;
        if (area > 1e-6)
            out[unsigned(lr->region().config().perimeter_extruder.value)] += area;
    }
    return out;
}

} // namespace

TEST_CASE("ColorMix layer word, white core, top patches and layer schedule", "[ColorMix]")
{
    Model model;
    add_painted_cube(model, 6);
    FullSpectrum::VirtualExtruder ve;
    ve.id         = 6;
    ve.color      = "#40C0D8";
    ve.components = {{1, 2. / 3.}, {2, 1. / 3.}};
    ve.sequence   = {1, 1, 2};
    model.virtual_extruders                = FullSpectrum::normalize_virtual_extruders({ve});
    model.colormix.slice.core_extruder     = 4;
    model.colormix.slice.wall_depth        = 0.9;
    model.colormix.slice.top_pattern       = true;
    model.colormix.slice.top_layers        = 3;
    model.colormix.slice.top_cell_size     = 1.5;
    model.colormix.slice.layer_schedule    = {0.12, 0.10, 0.08};

    Print print;
    print.apply(model, five_tool_config());
    print.validate();
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObject &po = *print.objects().front();
    REQUIRE(po.layer_count() > 20);

    // Variable layer schedule above the 0.2 mm first layer.
    CHECK(po.get_layer(0)->height == Approx(0.2));
    CHECK(po.get_layer(1)->height == Approx(0.12));
    CHECK(po.get_layer(2)->height == Approx(0.10));
    CHECK(po.get_layer(3)->height == Approx(0.08));
    CHECK(po.get_layer(4)->height == Approx(0.12));

    // Middle layers: the wall follows the word C C M by absolute layer index, the core is White (4).
    const size_t n = po.layer_count();
    for (size_t L = 6; L + 6 < n; ++L) {
        const auto ex   = layer_extruders(*po.get_layer(int(L)));
        const unsigned want = ve.sequence[L % ve.sequence.size()];
        INFO("layer " << L);
        CHECK(ex.count(want) == 1);
        CHECK(ex.count(4) == 1);
        // Core is the bulk of a 20 mm cube cross section, the wall ring is ~0.9 mm wide.
        CHECK(ex.at(4) > 250.);
        CHECK(ex.at(want) < 120.);
        CHECK(ex.count(want == 1 ? 2u : 1u) == 0);
    }

    // Top layers: patches of both components, regardless of the word.
    for (size_t L = n - 3; L < n; ++L) {
        const auto ex = layer_extruders(*po.get_layer(int(L)));
        INFO("top layer " << L);
        CHECK(ex.count(1) == 1);
        CHECK(ex.count(2) == 1);
        // Cyan is 2/3 of the recipe, so it gets most of the patch area.
        CHECK(ex.at(1) > ex.at(2));
    }

    // The G-code switches between the tools.
    const std::string g = Test::gcode(print);
    CHECK(g.find("\nT0") != std::string::npos);
    CHECK(g.find("\nT1") != std::string::npos);
    CHECK(g.find("\nT3") != std::string::npos);
}

TEST_CASE("ColorMix conversion of a two-color model and 3MF round trip", "[ColorMix]")
{
    Model model;
    add_painted_cube(model, 1, 10.);
    ModelObject *green = add_painted_cube(model, 2, 10.);
    green->instances.front()->set_offset(Vec3d(130., 100., 0.));
    const std::vector<std::string> physical = {"#00AED5", "#D25B96", "#FFCE02", "#F1E8DD", "#544F4D"};
    model.colormix.source_colors           = {"", "#00AED5", "#3CA85A"}; // cyan, and a green that needs a blend

    using namespace ColorMix;
    const Bundle bundle = builtin_colormix_bundle();
    LayerSchedule sched;
    Predictor     predictor(bundle, nullptr, sched);
    const std::vector<SourceColor> sources = collect_source_colors(model, current_extruder_colors(model, physical));
    REQUIRE(sources.size() == 2);
    std::vector<Target> targets;
    for (const SourceColor &s : sources)
        targets.push_back({hex_to_lab(s.color), 1., s.label});
    ConvertOptions opt;
    opt.schedule = sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const ConversionResult res = convert_colors(targets, bundle, predictor, tools, opt);
    REQUIRE(res.problem.empty());
    capture_source(model, current_extruder_colors(model, physical));
    const ApplySummary sum = apply_conversion(model, sources, res, bundle, tools, 5);
    CHECK(sum.virtual_extruders == 1);
    CHECK(sum.state_to_extruder[1] == 1);
    CHECK(sum.state_to_extruder[2] == 6);
    REQUIRE(model.virtual_extruders.size() == 1);
    CHECK(!model.virtual_extruders.front().sequence.empty());
    CHECK(model.objects[1]->volumes.front()->mm_segmentation_facets.get_data().used_states[6]);
    CHECK(model.objects[1]->volumes.front()->has_colormix_source());
    model.colormix.slice.core_extruder = 4;

    // Save and load back.
    const std::string path = (boost::filesystem::temp_directory_path() / "colormix_roundtrip.3mf").string();
    DynamicPrintConfig config = five_tool_config();
    REQUIRE(store_3mf(path.c_str(), &model, &config, false));
    Model loaded;
    DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Disable};
    boost::optional<Semver> version;
    REQUIRE(load_3mf(path.c_str(), loaded_config, ctxt, &loaded, false, version));
    boost::filesystem::remove(path);
    CHECK(loaded.colormix == model.colormix);
    REQUIRE(loaded.virtual_extruders.size() == 1);
    CHECK(loaded.virtual_extruders.front().sequence == model.virtual_extruders.front().sequence);
    REQUIRE(loaded.objects.size() == 2);
    CHECK(loaded.objects[1]->volumes.front()->has_colormix_source());

    // Revert restores the source painting.
    revert_to_source(loaded, 5);
    CHECK(loaded.virtual_extruders.empty());
    CHECK(loaded.objects[1]->volumes.front()->mm_segmentation_facets.get_data().used_states[2]);
}

// Real-model test, run explicitly:
//   COLORMIX_TEST_3MF=<file.3mf> [COLORMIX_TEST_BASELINE=1] [COLORMIX_TEST_TECH=none|core|top|both]
//   [COLORMIX_TEST_SAVE=<out.3mf>] [COLORMIX_TEST_CONFIG=<project.3mf>] [COLORMIX_TEST_BUNDLE=<bundle.json>]
//   fff_print_tests "[ColorMixReal]"
// Uses the printer/print settings stored in the 3MF when it is a PrusaSlicer project, or those of
// COLORMIX_TEST_CONFIG. COLORMIX_TEST_BUNDLE: a (user) bundle file; its newest calibration is used.
TEST_CASE("ColorMix real colored 3MF import, conversion and slicing", "[.][ColorMixReal]")
{
    const char *path = std::getenv("COLORMIX_TEST_3MF");
    if (path == nullptr)
        return;
    using clk   = std::chrono::steady_clock;
    auto secs   = [](clk::time_point a) { return std::chrono::duration<double>(clk::now() - a).count(); };
    auto t0     = clk::now();
    Model model;
    DynamicPrintConfig file_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    boost::optional<Semver> version;
    REQUIRE(load_3mf(path, file_config, ctxt, &model, false, version));
    size_t tris = 0;
    for (const ModelObject *o : model.objects)
        for (const ModelVolume *v : o->volumes)
            tris += v->mesh().its.indices.size();
    std::cout << "loaded in " << secs(t0) << " s: objects " << model.objects.size() << ", triangles " << tris
              << ", virtual extruders " << model.virtual_extruders.size() << ", source colors " << int(model.colormix.source_colors.size()) - 1 << "\n";

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    if (const char *cfg = std::getenv("COLORMIX_TEST_CONFIG")) {
        Model             tmp;
        boost::optional<Semver> v2;
        ConfigSubstitutionContext ctxt2{ForwardCompatibilitySubstitutionRule::Enable};
        file_config = DynamicPrintConfig();
        REQUIRE(load_3mf(cfg, file_config, ctxt2, &tmp, false, v2));
    }
    if (file_config.has("nozzle_diameter")) {
        config.apply(file_config, true);
        // Some saved projects carry a malformed per-extruder enum list; fall back to the default.
        try {
            config.option<ConfigOptionEnumsGeneric>("cooling_slowdown_logic", false);
            DynamicPrintConfig probe;
            probe.set_deserialize_strict("cooling_slowdown_logic", config.opt_serialize("cooling_slowdown_logic"));
        } catch (const std::exception &) {
            config.set_key_value("cooling_slowdown_logic", DynamicPrintConfig::full_print_config().option("cooling_slowdown_logic")->clone());
        }
        std::cout << "printer: " << config.opt_string("printer_settings_id") << ", print: " << config.opt_string("print_settings_id") << "\n";
    } else
        config.set_deserialize_strict({{"nozzle_diameter", "0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4"}, {"wipe_tower", "0"},
                                       {"layer_height", "0.1"}, {"first_layer_height", "0.2"}, {"bed_shape", "0x0,250x0,250x220,0x220"}});
    config.set_key_value("binary_gcode", new ConfigOptionBool(false)); // text G-code so it can be inspected
    const size_t num_physical = config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    std::vector<std::string> physical = config.option<ConfigOptionStrings>("extruder_colour")->values;
    physical.resize(num_physical, "#808080");

    using namespace ColorMix;
    std::vector<std::set<unsigned int>> est_sets;
    std::vector<std::string>            est_names;
    if (std::getenv("COLORMIX_TEST_BASELINE") == nullptr) {
        t0 = clk::now();
        Bundle bundle = builtin_colormix_bundle();
        if (const char *bp = std::getenv("COLORMIX_TEST_BUNDLE")) {
            std::string err;
            auto loaded_bundle = load_bundle(bp, &err);
            REQUIRE(loaded_bundle.has_value());
            bundle = *loaded_bundle;
        }
        LayerSchedule sched;
        sched.first_layer_height = config.get_abs_value("first_layer_height", config.opt_float("layer_height"));
        sched.heights            = {config.opt_float("layer_height")};
        const Calibration *cal = bundle.calibrations.empty() ? nullptr : &bundle.calibrations.back();
        Predictor predictor(bundle, cal, sched);
        if (cal)
            std::cout << "calibration: " << cal->id << " - " << predictor.report().summary() << "\n";
        const std::vector<SourceColor> sources = collect_source_colors(model, current_extruder_colors(model, physical));
        std::vector<double> areas;
        std::vector<Target> targets;
        for (const SourceColor &s : sources) {
            areas.push_back(s.area);
            targets.push_back({hex_to_lab(s.color), 1., s.label, s.z_ranges, s.top_z_ranges});
        }
        const std::vector<double> w = area_weights(areas);
        for (size_t i = 0; i < targets.size(); ++i)
            targets[i].weight = w[i];
        ConvertOptions opt;
        opt.schedule   = sched;
        opt.dual_walls = std::getenv("COLORMIX_TEST_DUAL") != nullptr;
        const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
        opt.core_tool = 4;
        {
            const char *tech = std::getenv("COLORMIX_TEST_TECH");
            const std::string t = tech ? tech : "both";
            opt.top_layers = (t == "both" || t == "top") ? 3 : 0;
        }
        if (const char *cw = std::getenv("COLORMIX_TEST_CHANGE_WEIGHT"))
            opt.change_weight = std::atof(cw);
        const ConversionResult res = convert_colors(targets, bundle, predictor, tools, opt);
        REQUIRE(res.problem.empty());
        std::cout << "converted in " << secs(t0) << " s (" << res.candidates.size() << " candidate recipes)\n";
        for (size_t k = 0; k < sources.size(); ++k)
            std::cout << "  state " << sources[k].state << "  " << sources[k].color << "  " << int(sources[k].area) << " mm2  -> "
                      << res.candidates[size_t(res.choice[k])].name << "  predicted " << lab_to_hex(res.candidates[size_t(res.choice[k])].prediction.color)
                      << "  dE " << res.de[k] << "\n";
        std::cout << "weighted mean dE " << res.weighted_mean_de << ", worst " << res.worst_de << ", blends " << res.blends_used
                  << ", optimal " << res.optimal << ", estimated tool changes " << res.tool_changes << " (before optimizing "
                  << res.tool_changes_before << ", change weight " << opt.change_weight << ")\n";
        est_sets = estimate_tools_per_layer(targets, res.candidates, res.choice, tools, sched, 4, opt.top_layers);
        capture_source(model, current_extruder_colors(model, physical));
        apply_conversion(model, sources, res, bundle, tools, unsigned(num_physical));
        const char *tech = std::getenv("COLORMIX_TEST_TECH");
        const std::string t = tech ? tech : "both";
        model.colormix.slice.core_extruder = (t == "both" || t == "core") ? 4 : 0;
        model.colormix.slice.top_pattern   = (t == "both" || t == "top");
        model.colormix.slice.region_cleanup = std::getenv("COLORMIX_TEST_CLEANUP") != nullptr;
        if (const char *pb = std::getenv("COLORMIX_TEST_PATCH_BAND"))
            model.colormix.slice.patch_band_width = std::atof(pb);
        model.colormix.bundle_id           = bundle.id;
        model.colormix.tool_of_filament    = tools;
        if (const char *save = std::getenv("COLORMIX_TEST_SAVE")) {
            REQUIRE(store_3mf(save, &model, &config, false));
            std::cout << "saved " << save << "\n";
        }
    } else
        std::cout << "baseline: slicing the project as stored (stock virtual extruders, no ColorMix techniques)\n";

    for (ModelObject *o : model.objects)
        o->ensure_on_bed();
    Print print;
    t0 = clk::now();
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    std::cout << "sliced in " << secs(t0) << " s\n";
    if (std::getenv("COLORMIX_TEST_LAYER_DIFF")) {
        auto dump = [](const char *what, const FullSpectrum::VirtualExtruders &ves) {
            std::cout << what << ":\n";
            for (const FullSpectrum::VirtualExtruder &ve : ves) {
                std::cout << "  VE " << ve.id << " seq";
                for (unsigned int t : ve.sequence) std::cout << " " << t;
                std::cout << " | inner";
                for (unsigned int t : ve.inner_sequence) std::cout << " " << t;
                std::cout << " | comps";
                for (const auto &c : ve.components) std::cout << " " << c.extruder_id << ":" << c.ratio;
                std::cout << "\n";
            }
        };
        dump("model VEs", model.virtual_extruders);
        dump("print VEs", print.virtual_extruders());
        // Painting states used by the model's volumes.
        for (const ModelObject *o : model.objects)
            for (const ModelVolume *v : o->volumes) {
                std::cout << "  volume " << v->name << " extruder " << v->extruder_id() << " states";
                const auto &used = v->mm_segmentation_facets.get_data().used_states;
                for (size_t st = 0; st < used.size(); ++st)
                    if (used[st])
                        std::cout << " " << st;
                std::cout << "\n";
            }
    }
    t0 = clk::now();
    const std::string g = Test::gcode(print);
    std::cout << "G-code exported in " << secs(t0) << " s\n";
    size_t tool_changes = 0;
    for (size_t p = g.find("\nT"); p != std::string::npos; p = g.find("\nT", p + 1))
        ++tool_changes;
    size_t layers = 0, max_tools = 0, tool_layer_sum = 0;
    for (const PrintObject *po : print.objects()) {
        layers = std::max(layers, po->layer_count());
        for (const Layer *l : po->layers()) {
            std::set<unsigned int> ex;
            for (const LayerRegion *lr : l->regions())
                if (!lr->slices().empty())
                    ex.insert(unsigned(lr->region().config().perimeter_extruder.value));
            max_tools = std::max(max_tools, ex.size());
            tool_layer_sum += ex.size();
        }
    }
    // What the printer does: tools per print height over all objects.
    std::map<long, std::set<unsigned int>> by_z;
    for (const PrintObject *po : print.objects())
        for (const Layer *l : po->layers())
            for (const LayerRegion *lr : l->regions())
                if (!lr->slices().empty())
                    by_z[std::lround(l->print_z * 1000.)].insert(unsigned(lr->region().config().perimeter_extruder.value));
    size_t union_changes = 0;
    for (const auto &[z, ts] : by_z)
        union_changes += ts.size() - 1;
    // Tool changes in the G-code: lines "T<n>" (any leading spaces).
    size_t gcode_t = 0;
    {
        std::istringstream in(g);
        for (std::string line; std::getline(in, line);) {
            const size_t a = line.find_first_not_of(" \t");
            if (a != std::string::npos && line[a] == 'T' && a + 1 < line.size() && std::isdigit((unsigned char) line[a + 1]))
                ++gcode_t;
        }
    }
    if (std::getenv("COLORMIX_TEST_LAYER_DIFF") && !est_sets.empty()) {
        const PrintObject *po = print.objects().front();
        int shown = 0, diff = 0;
        const size_t first_shown = size_t(std::max(0, std::atoi(std::getenv("COLORMIX_TEST_LAYER_DIFF"))));
        std::map<unsigned int, int> est_count, act_count;
        for (size_t L = 0; L < po->layer_count() && L < est_sets.size(); ++L) {
            std::set<unsigned int> act;
            for (const LayerRegion *lr : po->get_layer(int(L))->regions())
                if (!lr->slices().empty())
                    act.insert(unsigned(lr->region().config().perimeter_extruder.value));
            for (unsigned int t : act)
                ++act_count[t];
            for (unsigned int t : est_sets[L])
                ++est_count[t];
            if (act == est_sets[L])
                continue;
            ++diff;
            if (L >= first_shown && shown < 25 && std::getenv("COLORMIX_TEST_AREAS")) {
                std::cout << "  layer " << L << " areas:";
                for (const auto &[e, a] : layer_extruders(*po->get_layer(int(L))))
                    std::cout << " T" << e << "=" << a;
                std::cout << "\n";
            }
            if (L >= first_shown && shown++ < 25) {
                std::cout << "  layer " << L << " z " << po->get_layer(int(L))->print_z << " estimated {";
                for (unsigned int t : est_sets[L]) std::cout << " " << t;
                std::cout << " } actual {";
                for (unsigned int t : act) std::cout << " " << t;
                std::cout << " }\n";
            }
        }
        std::cout << "layers whose tools differ from the estimate: " << diff << "\n";
        for (unsigned int t = 1; t <= 8; ++t)
            if (est_count[t] || act_count[t])
                std::cout << "  tool " << t << ": on " << est_count[t] << " layers estimated, " << act_count[t] << " actual\n";
    }
    std::cout << "objects " << print.objects().size() << ", print heights " << by_z.size() << ", tool changes over all objects per height "
              << union_changes << ", T lines in G-code " << gcode_t << "\n";
    if (const char *gout = std::getenv("COLORMIX_TEST_GCODE_OUT")) {
        boost::nowide::ofstream f(gout);
        f << g;
    }
    std::string est;
    if (size_t p = g.find("; estimated printing time (normal mode) = "); p != std::string::npos)
        est = g.substr(p + 42, g.find('\n', p) - p - 42);
    std::cout << "layers " << layers << ", tool changes ~" << (tool_layer_sum > layers ? tool_layer_sum - layers : 0) << ", max tools on a layer " << max_tools
              << ", estimated time " << est << ", G-code " << double(g.size()) / 1e6 << " MB\n";
    CHECK(tool_layer_sum > 0);
}

TEST_CASE("ColorMix dual walls: outer ring, inner ring and white core", "[ColorMix]")
{
    Model model;
    add_painted_cube(model, 6);
    FullSpectrum::VirtualExtruder ve;
    ve.id             = 6;
    ve.color          = "#2A6E86";
    ve.sequence       = {1, 1, 2};            // outer wall: cyan, cyan, magenta
    ve.inner_sequence = {5};                  // inner wall: black
    ve.components     = {{1, 1. / 3.}, {2, 1. / 6.}, {5, 0.5}};
    model.virtual_extruders                = FullSpectrum::normalize_virtual_extruders({ve});
    REQUIRE(model.virtual_extruders.front().inner_sequence.size() == 1);
    model.colormix.slice.core_extruder     = 4;
    model.colormix.slice.wall_depth        = 0.9;
    model.colormix.slice.dual_wall_width   = 0.45;

    Print print;
    print.apply(model, five_tool_config());
    print.validate();
    print.set_status_silent();
    print.process();
    const PrintObject &po = *print.objects().front();
    const size_t n = po.layer_count();
    for (size_t L = 6; L + 6 < n; ++L) {
        const auto ex   = layer_extruders(*po.get_layer(int(L)));
        const unsigned outer = ve.sequence[L % ve.sequence.size()];
        INFO("layer " << L);
        REQUIRE(ex.count(outer) == 1);
        REQUIRE(ex.count(5) == 1);
        REQUIRE(ex.count(4) == 1);
        // 20 mm cube: perimeter 80 mm; each ring ~0.45 mm wide -> ~36 mm^2.
        CHECK(ex.at(outer) > 20.);
        CHECK(ex.at(outer) < 50.);
        CHECK(ex.at(5) > 20.);
        CHECK(ex.at(5) < 50.);
        CHECK(ex.at(4) > 300.);
    }
    // Every wall extrusion of the outer ring is printed by the outer word's tool: check the
    // perimeters of the outer-word region on a middle layer.
    const Layer &mid = *po.get_layer(int(n / 2));
    bool found_outer_perimeter = false;
    for (const LayerRegion *lr : mid.regions())
        if (unsigned(lr->region().config().perimeter_extruder.value) == ve.sequence[(n / 2) % 3] && !lr->perimeters().empty())
            found_outer_perimeter = true;
    CHECK(found_outer_perimeter);
    const std::string g = Test::gcode(print);
    CHECK(g.find("\nT4") != std::string::npos); // black inner wall (T4 = tool 5)
}

#include "libslic3r/Feature/ColorMix/Calibration.hpp"

TEST_CASE("ColorMix full calibration plate slices", "[ColorMix]")
{
    using namespace ColorMix;
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const auto plan = plan_calibration_swatches(b, tools, sched, 4, 10, 2, BoardLayout().swatches_per_board());
    Model model;
    BoardLayout L;
    build_calibration_plate(model, plan, b, tools, 5, L, 5, 220.);
    model.colormix.slice.core_extruder = 4;
    DynamicPrintConfig config = five_tool_config();
    config.set_deserialize_strict({{"bed_shape", "0x0,250x0,250x220,0x220"}});
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().size() == 1);
    size_t max_tools = 0;
    for (const PrintObject *po : print.objects())
        for (const Layer *l : po->layers())
            max_tools = std::max(max_tools, layer_extruders(*l).size());
    CHECK(max_tools <= 5);
    // No windows (they printed as sagging bridges and read darker and tinted): the
    // middle row is as full as the others.
    const PrintObject &po = *print.objects().front();
    auto area_at = [&](double z) {
        double a = 0.;
        for (const Layer *l : po.layers())
            if (l->print_z >= z) {
                for (const auto &[e, ar] : layer_extruders(*l))
                    a += ar;
                break;
            }
        return a;
    };
    const double middle = area_at(L.base + 1.5 * L.cell_h), top = area_at(L.base + 2.5 * L.cell_h);
    CHECK(std::abs(middle - top) < 0.02 * top);
    // The first layer is one piece (the sheet with every board on it), no brim, and the sheet is
    // only one layer: the second layer holds the boards alone. PrusaSlicer doesn't warn about bed
    // adhesion.
    REQUIRE(po.layer_count() > 2);
    CHECK(po.layers()[0]->lslices.size() == 1);
    CHECK(print.brim().empty());
    CHECK(area(po.layers()[1]->lslices) < 0.5 * area(po.layers()[0]->lslices));
    for (const auto &w : print.step_state_with_warnings(psAlertWhenSupportsNeeded).warnings) {
        INFO(w.message);
        CHECK(w.message.find("Low bed adhesion") == std::string::npos);
    }
    CHECK(!Test::gcode(print).empty());
}

// Diagnostic: for every swatch of the full calibration plate, the tool of the front wall on every
// layer must follow the swatch's (outer) word. Prints a report of mismatching layers.
TEST_CASE("ColorMix calibration plate swatches follow their words", "[ColorMix]")
{
    using namespace ColorMix;
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const auto plan = plan_calibration_swatches(b, tools, sched, 4, 10, 2, BoardLayout().swatches_per_board());
    Model model;
    BoardLayout L;
    build_calibration_plate(model, plan, b, tools, 5, L, 5, 220.);
    model.colormix.slice.core_extruder = 4;
    // Every blended swatch keeps its virtual extruder.
    size_t blended = 0;
    for (const SwatchSpec &sw : plan)
        blended += sw.word.size() > 1 || !sw.inner_word.empty();
    CHECK(model.virtual_extruders.size() == blended);
    for (size_t i = 0; i < model.virtual_extruders.size(); ++i)
        CHECK(model.virtual_extruders[i].id == 6 + i);

    DynamicPrintConfig config = five_tool_config();
    config.set_deserialize_strict({{"bed_shape", "0x0,250x0,250x220,0x220"}});
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObject *po    = print.objects().front();
    const auto         cells = L.swatch_cells();
    const int          per   = L.swatches_per_board();
    int bad_swatches = 0;
    for (size_t i = 0; i < plan.size(); ++i) {
        const int board = int(i) / per; // board 1 at the back
        const auto r = L.cell_rect(cells[i % size_t(per)][0], cells[i % size_t(per)][1]);
        std::vector<unsigned int> word, inner;
        for (int f : plan[i].word)
            word.push_back(tools[size_t(f)]);
        for (int f : plan[i].inner_word)
            inner.push_back(tools[size_t(f)]);
        int checked = 0, bad = 0;
        std::string detail;
        for (const Layer *l : po->layers()) {
            const double z = l->print_z - 0.05;
            if (z < r[1] + 0.3 || z > r[3] - 0.3)
                continue;
            // This board's front face: boards stand `pitch` apart, board 1 at the back.
            BoundingBox bb = get_extents(l->lslices);
            bb.min.y()     = bb.max.y() - scaled<coord_t>(L.thickness + board * L.pitch());
            // Outer wall on both faces at three points across the cell; the inner wall (dual walls)
            // just behind the outer ring; the white core in the middle.
            std::vector<std::pair<Point, unsigned int>> probes;
            for (double fx : {0.2, 0.5, 0.8})
                for (double fy : {0.25, L.thickness - 0.25})
                    probes.push_back({Point(bb.min.x() + scaled<coord_t>(r[0] + fx * (r[2] - r[0])), bb.min.y() + scaled<coord_t>(fy)),
                                      word[size_t(l->id()) % word.size()]});
            if (!inner.empty())
                probes.push_back({Point(bb.min.x() + scaled<coord_t>(0.5 * (r[0] + r[2])), bb.min.y() + scaled<coord_t>(0.7)),
                                  inner[size_t(l->id()) % inner.size()]});
            probes.push_back({Point(bb.min.x() + scaled<coord_t>(0.5 * (r[0] + r[2])), bb.min.y() + scaled<coord_t>(0.5 * L.thickness)), 4u});
            for (const auto &[p, want] : probes) {
                const double fy = unscaled(p.y() - bb.min.y());
                unsigned int got = 0;
                std::string  all;
                for (const LayerRegion *lr : l->regions())
                    for (const Surface &s : lr->slices().surfaces)
                        if (s.expolygon.contains(p)) {
                            got = unsigned(lr->region().config().perimeter_extruder.value);
                            all += std::to_string(got) + "/" + std::to_string(lr->region().config().infill_extruder.value) + "+";
                        }
                ++checked;
                if (got != want) {
                    ++bad;
                    if (detail.size() < 300)
                        detail += " z" + std::to_string(int(std::round((z - r[1]) * 10))) + (fy < 0.5 ? "f" : fy < 1.5 ? "i" : fy < 3 ? "c" : "b") + ":" + all + "!=" + std::to_string(want);
                }
            }
        }
        if (bad > 0) {
            ++bad_swatches;
            std::cout << "swatch " << i << " board " << i / per + 1 << " " << plan[i].label << ": " << bad << "/" << checked << detail << "\n";
        }
    }
    CHECK(bad_swatches == 0);
}

TEST_CASE("ColorMix slope test builds and slices", "[ColorMix]")
{
    using namespace ColorMix;
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    sched.heights = {0.1};
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    Model model;
    const std::string legend = build_slope_test(model, b, tools, 5, 5, sched, 220.);
    REQUIRE(model.objects.size() == 2);
    CHECK(model.virtual_extruders.size() == 2);
    CHECK(model.objects.front()->volumes.size() == 1 + slope_test_angles().size());
    CHECK(legend.find("5 deg: 1.15 mm") != std::string::npos);
    model.colormix.slice.core_extruder = 4;
    DynamicPrintConfig config = five_tool_config();
    config.set_deserialize_strict({{"bed_shape", "0x0,250x0,250x220,0x220"}});
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().size() == 2);
    // The 40 degree wedge is 16.8 mm tall above a 1 mm base.
    CHECK(print.objects().front()->layer_count() > 160);
    CHECK(!Test::gcode(print).empty());
}

TEST_CASE("ColorMix region cleanup and slope patches slice a converted plate", "[ColorMix]")
{
    using namespace ColorMix;
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    Model model;
    build_slope_test(model, b, tools, 5, 5, sched, 220.);
    model.colormix.slice.core_extruder    = 4;
    model.colormix.slice.top_pattern      = true;
    model.colormix.slice.patch_band_width = 0.3;
    model.colormix.slice.region_cleanup   = true;
    DynamicPrintConfig config = five_tool_config();
    config.set_deserialize_strict({{"bed_shape", "0x0,250x0,250x220,0x220"}});
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    // On the 5 degree wedge (right end of the row) the middle layers carry patches: more than the
    // two filaments of the word's layer plus the core and frame.
    const PrintObject &po = *print.objects().front();
    size_t max_tools = 0;
    for (const Layer *l : po.layers())
        if (l->print_z > 1.3 && l->print_z < 1.6)
            max_tools = std::max(max_tools, layer_extruders(*l).size());
    CHECK(max_tools >= 3);
    CHECK(!Test::gcode(print).empty());
}

#include "libslic3r/miniz_extension.hpp"

namespace {
// A 3MF as written by other applications: `n` cubes, cube i using basematerials entry pindex[i].
std::string write_basematerials_3mf(const std::vector<int> &pindex)
{
    std::string objects, items;
    const char *cube_mesh =
        "<mesh><vertices>"
        "<vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"10\" y=\"0\" z=\"0\"/><vertex x=\"10\" y=\"10\" z=\"0\"/><vertex x=\"0\" y=\"10\" z=\"0\"/>"
        "<vertex x=\"0\" y=\"0\" z=\"10\"/><vertex x=\"10\" y=\"0\" z=\"10\"/><vertex x=\"10\" y=\"10\" z=\"10\"/><vertex x=\"0\" y=\"10\" z=\"10\"/>"
        "</vertices><triangles>"
        "<triangle v1=\"0\" v2=\"2\" v3=\"1\"/><triangle v1=\"0\" v2=\"3\" v3=\"2\"/><triangle v1=\"4\" v2=\"5\" v3=\"6\"/><triangle v1=\"4\" v2=\"6\" v3=\"7\"/>"
        "<triangle v1=\"0\" v2=\"1\" v3=\"5\"/><triangle v1=\"0\" v2=\"5\" v3=\"4\"/><triangle v1=\"1\" v2=\"2\" v3=\"6\"/><triangle v1=\"1\" v2=\"6\" v3=\"5\"/>"
        "<triangle v1=\"2\" v2=\"3\" v3=\"7\"/><triangle v1=\"2\" v2=\"7\" v3=\"6\"/><triangle v1=\"3\" v2=\"0\" v3=\"4\"/><triangle v1=\"3\" v2=\"4\" v3=\"7\"/>"
        "</triangles></mesh>";
    for (size_t i = 0; i < pindex.size(); ++i) {
        const std::string id = std::to_string(i + 2);
        objects += "<object id=\"" + id + "\" type=\"model\" pid=\"1\" pindex=\"" + std::to_string(pindex[i]) + "\">" + cube_mesh + "</object>";
        items += "<item objectid=\"" + id + "\" transform=\"1 0 0 0 1 0 0 0 1 " + std::to_string(20 * i) + " 0 0\"/>";
    }
    const std::string model =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<model unit=\"millimeter\" xml:lang=\"en-US\" "
        "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\"><resources>"
        "<basematerials id=\"1\"><base name=\"Red\" displaycolor=\"#FF0000FF\"/><base name=\"Blue\" displaycolor=\"#0000FFFF\"/></basematerials>" +
        objects + "</resources><build>" + items + "</build></model>";
    const std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>";
    const std::string types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/></Types>";
    const std::string path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("colormix_%%%%%%.3mf")).string();
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    REQUIRE(open_zip_writer(&archive, path));
    mz_zip_writer_add_mem(&archive, "[Content_Types].xml", types.data(), types.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_add_mem(&archive, "_rels/.rels", rels.data(), rels.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_add_mem(&archive, "3D/3dmodel.model", model.data(), model.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_finalize_archive(&archive);
    close_zip_writer(&archive);
    return path;
}
} // namespace

TEST_CASE("ColorMix 3MF import paints only multi-color models from other applications", "[ColorMix]")
{
    auto load = [](const std::vector<int> &pindex) {
        const std::string path = write_basematerials_3mf(pindex);
        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
        boost::optional<Semver> version;
        const bool ok = load_3mf(path.c_str(), config, ctxt, &model, false, version);
        boost::filesystem::remove(path);
        REQUIRE(ok);
        return model;
    };
    SECTION("two materials: each object on its tool, source colors kept, nothing painted") {
        const Model m = load({0, 1});
        REQUIRE(m.objects.size() == 2);
        // The importer only records the slots (the GUI assigns the tools); nothing is painted and
        // other callers (CLI) load the objects as stock PrusaSlicer does.
        CHECK(!m.objects[0]->volumes.front()->is_mm_painted());
        CHECK(!m.objects[1]->volumes.front()->is_mm_painted());
        CHECK(m.objects[0]->volumes.front()->extruder_id() == 0);
        CHECK(m.colormix.pending_import_slots.at(m.objects[0]->id().id) == 1);
        CHECK(m.colormix.pending_import_slots.at(m.objects[1]->id().id) == 2);
        CHECK(m.colormix.import_slots.empty());
        REQUIRE(m.colormix.source_colors.size() == 3);
        CHECK(m.colormix.source_colors[1] == "#FF0000");
        CHECK(m.colormix.source_colors[2] == "#0000FF");
    }
    SECTION("one material (CAD export): loads like a plain model") {
        const Model m = load({0, 0});
        REQUIRE(m.objects.size() == 2);
        CHECK(!m.objects[0]->volumes.front()->is_mm_painted());
        CHECK(!m.objects[1]->volumes.front()->is_mm_painted());
        CHECK(m.colormix.source_colors.empty());
    }
}

namespace {
std::string write_3mf_files(const std::vector<std::pair<std::string, std::string>> &files)
{
    const std::string path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("colormix_%%%%%%.3mf")).string();
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    REQUIRE(open_zip_writer(&archive, path));
    for (const auto &[name, data] : files)
        mz_zip_writer_add_mem(&archive, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_finalize_archive(&archive);
    close_zip_writer(&archive);
    return path;
}
Model load_3mf_file(const std::string &path)
{
    Model model;
    DynamicPrintConfig config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    boost::optional<Semver> version;
    const bool ok = load_3mf(path.c_str(), config, ctxt, &model, false, version);
    boost::filesystem::remove(path);
    REQUIRE(ok);
    return model;
}
const char *CUBE_MESH =
    "<mesh><vertices>"
    "<vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"10\" y=\"0\" z=\"0\"/><vertex x=\"10\" y=\"10\" z=\"0\"/><vertex x=\"0\" y=\"10\" z=\"0\"/>"
    "<vertex x=\"0\" y=\"0\" z=\"10\"/><vertex x=\"10\" y=\"0\" z=\"10\"/><vertex x=\"10\" y=\"10\" z=\"10\"/><vertex x=\"0\" y=\"10\" z=\"10\"/>"
    "</vertices><triangles>"
    "<triangle v1=\"0\" v2=\"2\" v3=\"1\"/><triangle v1=\"0\" v2=\"3\" v3=\"2\"/><triangle v1=\"4\" v2=\"5\" v3=\"6\"/><triangle v1=\"4\" v2=\"6\" v3=\"7\"/>"
    "<triangle v1=\"0\" v2=\"1\" v3=\"5\"/><triangle v1=\"0\" v2=\"5\" v3=\"4\"/><triangle v1=\"1\" v2=\"2\" v3=\"6\"/><triangle v1=\"1\" v2=\"6\" v3=\"5\"/>"
    "<triangle v1=\"2\" v2=\"3\" v3=\"7\"/><triangle v1=\"2\" v2=\"7\" v3=\"6\"/><triangle v1=\"3\" v2=\"0\" v3=\"4\"/><triangle v1=\"3\" v2=\"4\" v3=\"7\"/>"
    "</triangles></mesh>";
const char *RELS =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>";
std::string model_xml(const std::string &resources, const std::string &build)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<model unit=\"millimeter\" xml:lang=\"en-US\" "
           "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\"><resources>" + resources + "</resources><build>" + build + "</build></model>";
}
} // namespace

TEST_CASE("ColorMix 3MF import: Bambu part filaments and separate basematerials groups", "[ColorMix]")
{
    SECTION("Bambu-style layout: an assembly of parts, filaments per part") {
        // As in Bambu Studio files: object 3 is an assembly of mesh objects 1 and 2; model_settings
        // lists the parts by the mesh objects' ids.
        const std::string res = std::string("<object id=\"1\" type=\"model\">") + CUBE_MESH + "</object>" +
                                "<object id=\"2\" type=\"model\">" + CUBE_MESH + "</object>" +
                                "<object id=\"3\" type=\"model\"><components><component objectid=\"1\"/>"
                                "<component objectid=\"2\" transform=\"1 0 0 0 1 0 0 0 1 20 0 0\"/></components></object>";
        const std::string settings =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<config><object id=\"3\"><metadata key=\"extruder\" value=\"1\"/>"
            "<part id=\"1\" subtype=\"normal_part\"><metadata key=\"extruder\" value=\"2\"/></part>"
            "<part id=\"2\" subtype=\"normal_part\"><metadata key=\"extruder\" value=\"3\"/></part></object></config>";
        const std::string project = "{\"filament_colour\": [\"#111111\", \"#FF0000\", \"#00FF00\"]}";
        const Model m = load_3mf_file(write_3mf_files({{"_rels/.rels", RELS},
                                                       {"3D/3dmodel.model", model_xml(res, "<item objectid=\"3\"/>")},
                                                       {"Metadata/model_settings.config", settings},
                                                       {"Metadata/project_settings.config", project}}));
        REQUIRE(m.colormix.source_colors.size() == 4);
        CHECK(m.colormix.source_colors[2] == "#FF0000");
        for (const ModelObject *o : m.objects)
            for (const ModelVolume *v : o->volumes) {
                CHECK(!v->is_mm_painted());
                CHECK(!v->has_colormix_source()); // captured by the first conversion, not at import
            }
        std::set<unsigned int> slots;
        for (const auto &[id, slot] : m.colormix.pending_import_slots)
            slots.insert(slot);
        CHECK(slots == std::set<unsigned int>{2, 3});
    }
    SECTION("parts on two filament slots of the same color: a single-color model, loaded as stock") {
        // E.g. a Bambu file with two black spools (the "has 1 colors" offer seen with a real file).
        const std::string res = std::string("<object id=\"1\" type=\"model\">") + CUBE_MESH + "</object>" +
                                "<object id=\"2\" type=\"model\">" + CUBE_MESH + "</object>" +
                                "<object id=\"3\" type=\"model\"><components><component objectid=\"1\"/>"
                                "<component objectid=\"2\" transform=\"1 0 0 0 1 0 0 0 1 20 0 0\"/></components></object>";
        const std::string settings =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<config><object id=\"3\"><metadata key=\"extruder\" value=\"1\"/>"
            "<part id=\"1\" subtype=\"normal_part\"><metadata key=\"extruder\" value=\"1\"/></part>"
            "<part id=\"2\" subtype=\"normal_part\"><metadata key=\"extruder\" value=\"2\"/></part></object></config>";
        const std::string project = "{\"filament_colour\": [\"#000000\", \"#000000\", \"#E4BD68\"]}";
        const Model m = load_3mf_file(write_3mf_files({{"_rels/.rels", RELS},
                                                       {"3D/3dmodel.model", model_xml(res, "<item objectid=\"3\"/>")},
                                                       {"Metadata/model_settings.config", settings},
                                                       {"Metadata/project_settings.config", project}}));
        CHECK(m.colormix.source_colors.empty());
        CHECK(m.colormix.pending_import_slots.empty());
    }
    SECTION("every object with its own one-entry basematerials group") {
        const std::string res = std::string("<basematerials id=\"10\"><base name=\"A\" displaycolor=\"#FF0000\"/></basematerials>") +
                                "<basematerials id=\"11\"><base name=\"B\" displaycolor=\"#0000FF\"/></basematerials>" +
                                "<object id=\"1\" type=\"model\" pid=\"10\" pindex=\"0\">" + CUBE_MESH + "</object>" +
                                "<object id=\"2\" type=\"model\" pid=\"11\" pindex=\"0\">" + CUBE_MESH + "</object>";
        const Model m = load_3mf_file(write_3mf_files(
            {{"_rels/.rels", RELS},
             {"3D/3dmodel.model", model_xml(res, "<item objectid=\"1\"/><item objectid=\"2\" transform=\"1 0 0 0 1 0 0 0 1 20 0 0\"/>")}}));
        REQUIRE(m.objects.size() == 2);
        REQUIRE(m.colormix.source_colors.size() == 3);
        CHECK(m.colormix.source_colors[1] == "#FF0000");
        CHECK(m.colormix.source_colors[2] == "#0000FF");
        CHECK(m.colormix.pending_import_slots.at(m.objects[0]->id().id) == 1);
        CHECK(m.colormix.pending_import_slots.at(m.objects[1]->id().id) == 2);
    }
}

TEST_CASE("ColorMix: a part added after a conversion keeps its own color", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    // A converted part: source painting in palette slot 1 (red).
    ModelObject *a = add_painted_cube(model, 6);
    {
        TriangleSelector sel(a->volumes.front()->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(1)}});
        a->volumes.front()->colormix_source_facets.set(sel);
    }
    model.colormix.source_colors = {"", "#FF0000"};
    model.colormix.bundle_id     = "prusament-pla-colormix"; // converted
    // Added later on extruder 1, which now holds cyan.
    ModelObject *b = model.add_object();
    b->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    b->add_instance();
    b->config.set("extruder", 1);
    const std::vector<std::string> extruder_colors = {"", "#00FFFF", "#FF00FF", "#FFFF00", "#FFFFFF", "#000000", "#123456"};
    const std::vector<SourceColor> sources = collect_source_colors(model, extruder_colors);
    REQUIRE(sources.size() == 2);
    CHECK(sources[0].state == 1);
    CHECK(sources[0].color == "#FF0000");
    CHECK(sources[1].state == 2);
    CHECK(sources[1].color == "#00FFFF");
    capture_source(model, extruder_colors);
    REQUIRE(model.colormix.source_colors.size() == 3);
    CHECK(model.colormix.source_colors[2] == "#00FFFF");
    CHECK(b->volumes.front()->colormix_source_facets.get_data().used_states[2]);
}

TEST_CASE("ColorMix layer schedule stays in step on a raft", "[ColorMix]")
{
    Model model;
    add_painted_cube(model, 6);
    FullSpectrum::VirtualExtruder ve;
    ve.id         = 6;
    ve.color      = "#40C0D8";
    ve.components = {{1, 2. / 3.}, {2, 1. / 3.}};
    ve.sequence   = {1, 1, 2};
    model.virtual_extruders = {ve};
    model.colormix.slice.layer_schedule = {0.12, 0.10, 0.08};
    DynamicPrintConfig config = five_tool_config();
    config.set_deserialize_strict({{"raft_layers", "3"}, {"support_material_contact_distance", "0"}});
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    const PrintObject &po = *print.objects().front();
    REQUIRE(po.layer_count() > 10);
    // Object layer L >= 1 has height schedule[(L - 1) % 3], as the predictor assumes.
    const double sched[3] = {0.12, 0.10, 0.08};
    for (size_t L = 1; L < 10; ++L)
        CHECK(po.get_layer(int(L))->height == Approx(sched[(L - 1) % 3]));
}

TEST_CASE("ColorMix notices paint touch-ups after a conversion and can keep them", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    // A converted part: source slot 1 printed as blend 9.
    ModelObject *a = add_painted_cube(model, 1);
    ModelVolume *v = a->volumes.front();
    {
        TriangleSelector sel(v->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(1)}});
        v->colormix_source_facets.set(sel);
    }
    model.colormix.source_colors = {"", "#FF0000"};
    model.colormix.bundle_id     = "prusament-pla-colormix";
    const std::vector<unsigned int> applied = {0, 9};
    v->mm_segmentation_facets.set(converted_painting(*v, applied, 8));
    CHECK(count_repainted_volumes(model, applied, 8) == 0);

    // Touch-up: the whole part repainted with tool 5.
    {
        TriangleSelector sel(v->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(5)}});
        v->mm_segmentation_facets.set(sel);
    }
    REQUIRE(count_repainted_volumes(model, applied, 8) == 1);

    // Keep it: the touch-up's color becomes a source color.
    std::vector<std::string> colors(10, "#808080");
    colors[5] = "#00FF00";
    fold_repainted_into_source(model, applied, 8, colors);
    REQUIRE(model.colormix.source_colors.size() == 3);
    CHECK(model.colormix.source_colors[2] == "#00FF00");
    CHECK(v->colormix_source_facets.get_data().used_states[2]);
    CHECK(!v->colormix_source_facets.get_data().used_states[1]);
}

TEST_CASE("ColorMix calibration plan id identifies the plan", "[ColorMix]")
{
    using namespace ColorMix;
    const Bundle b = builtin_colormix_bundle();
    LayerSchedule sched;
    const std::vector<unsigned int> tools = {1, 2, 3, 4, 5, 0, 0};
    const BoardLayout L;
    const auto p1 = plan_calibration_swatches(b, tools, sched, 4, 10, 2, L.swatches_per_board(), 247);
    const auto p2 = plan_calibration_swatches(b, tools, sched, 4, 10, 2, L.swatches_per_board(), 247);
    const auto p3 = plan_calibration_swatches(b, tools, sched, 4, 12, 2, L.swatches_per_board(), 247);
    CHECK(calibration_plan_id(p1) == calibration_plan_id(p2));
    CHECK(calibration_plan_id(p1) != calibration_plan_id(p3));
}

TEST_CASE("ColorMix warns about variable layer height without a layer schedule", "[ColorMix]")
{
    Model model;
    ModelObject *o = add_painted_cube(model, 6);
    FullSpectrum::VirtualExtruder ve;
    ve.id         = 6;
    ve.color      = "#40C0D8";
    ve.components = {{1, 2. / 3.}, {2, 1. / 3.}};
    ve.sequence   = {1, 1, 2};
    model.virtual_extruders = {ve};
    o->layer_config_ranges[{5., 10.}].assign_config(DynamicPrintConfig());
    o->layer_config_ranges[{5., 10.}].set("layer_height", 0.2);
    DynamicPrintConfig config = five_tool_config();
    Print print;
    print.apply(model, config);
    std::vector<std::string> warnings;
    print.validate(&warnings);
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const std::string &w) { return w.find("variable layer height") != std::string::npos; }));
}

TEST_CASE("ColorMix: parts already on the plater keep their colors when a palette arrives", "[ColorMix]")
{
    using namespace ColorMix;
    // A stock project: a part on extruder 2 (red on this printer), no ColorMix data yet.
    Model model;
    ModelObject *o = model.add_object();
    o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    o->add_instance();
    o->config.set("extruder", 2);
    // A colored file arrives whose slot 2 is blue.
    model.colormix.source_colors = {"", "#FFFFFF", "#0000FF"};
    capture_source_by_color(model, {"", "#FFFFFF", "#FF0000"});
    // The part keeps red as its own source color, in a new slot.
    REQUIRE(model.colormix.source_colors.size() == 4);
    CHECK(model.colormix.source_colors[3] == "#FF0000");
    CHECK(o->volumes.front()->colormix_source_facets.get_data().used_states[3]);
}

TEST_CASE("ColorMix project data survives the undo/redo JSON round trip", "[ColorMix]")
{
    using namespace ColorMix;
    ModelData d;
    d.slice.core_extruder    = 4;
    d.slice.layer_schedule   = {0.12, 0.1, 0.08};
    d.slice.region_cleanup   = true;
    d.slice.patch_band_width = 0.3;
    d.source_colors          = {"", "#FF0000", "#00FF00"};
    d.bundle_id              = "prusament-pla-colormix";
    d.tool_of_filament       = {1, 2, 3, 4, 5, 0, 0};
    d.options_json           = "{\"applied_states\":[0,9,10]}";
    d.plate_json             = "{\"plan_id\":\"3F2A-225\"}";
    ModelData back;
    REQUIRE(model_data_from_json(model_data_to_json(d), back));
    CHECK(back == d);

    FullSpectrum::VirtualExtruders ves(3);
    ves[0].id = 9;  ves[0].color = "#112233"; ves[0].components = {{1, 0.5}, {2, 0.5}}; ves[0].sequence = {1, 2};
    ves[1].id = 10; ves[1].color = "#445566"; ves[1].components = {{1, 0.625}, {3, 0.375}}; ves[1].sequence = {1};
    ves[1].inner_sequence = {3, 3, 3, 1};
    ves[2].id = 11; ves[2].color = "#778899"; ves[2].components = {{2, 0.75}, {4, 0.25}};
    const auto loaded = FullSpectrum::deserialize_virtual_extruders_from_json(FullSpectrum::serialize_virtual_extruders_to_json({}, ves));
    REQUIRE(loaded.virtual_extruders.size() == 3);
    CHECK(loaded.virtual_extruders[0].sequence == ves[0].sequence);
    CHECK(loaded.virtual_extruders[1].inner_sequence == ves[1].inner_sequence);
    CHECK(loaded.virtual_extruders[2].components.size() == 2);
}

TEST_CASE("ColorMix keeps only real touch-ups when folding them into the source", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    ModelObject *a = add_painted_cube(model, 1);
    ModelVolume *v = a->volumes.front();
    // Source: the first 6 triangles in slot 1 (red), the others in slot 2 (blue).
    {
        TriangleSelector sel(v->mesh());
        for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
            sel.set_facet(f, static_cast<TriangleStateType>(f < 6 ? 1 : 2));
        v->colormix_source_facets.set(sel);
    }
    model.colormix.source_colors = {"", "#FF0000", "#0000FF"};
    model.colormix.bundle_id     = "prusament-pla-colormix";
    const std::vector<unsigned int> applied = {0, 9, 10}; // slot 1 -> blend 9, slot 2 -> blend 10
    v->mm_segmentation_facets.set(converted_painting(*v, applied, 8));
    // Touch-up: one triangle repainted with tool 5 (green).
    {
        TriangleSelector sel(v->mesh());
        sel.deserialize(v->mm_segmentation_facets.get_data(), false);
        sel.set_facet(0, static_cast<TriangleStateType>(5));
        v->mm_segmentation_facets.set(sel);
    }
    REQUIRE(count_repainted_volumes(model, applied, 8) == 1);
    std::vector<std::string> colors(12, "#808080");
    colors[5]  = "#00FF00";
    colors[9]  = "#EE1111"; // the blends' predictions: must not replace the design colors
    colors[10] = "#1111EE";
    fold_repainted_into_source(model, applied, 8, colors);
    // Untouched areas keep slots 1 and 2; only the touch-up gets a new slot.
    REQUIRE(model.colormix.source_colors.size() == 4);
    CHECK(model.colormix.source_colors[3] == "#00FF00");
    const auto &used = v->colormix_source_facets.get_data().used_states;
    CHECK(used[1]);
    CHECK(used[2]);
    CHECK(used[3]);
}

TEST_CASE("ColorMix data this build can't read is kept when saving", "[ColorMix]")
{
    const std::string future = "{\"version\": 99, \"something\": \"new\"}";
    const std::string res = std::string("<object id=\"1\" type=\"model\">") + CUBE_MESH + "</object>";
    Model m = load_3mf_file(write_3mf_files({{"_rels/.rels", RELS},
                                             {"3D/3dmodel.model", model_xml(res, "<item objectid=\"1\"/>")},
                                             {"Metadata/Prusa_Slicer_colormix.json", future}}));
    CHECK(m.colormix.unreadable_json == future);
    CHECK(m.colormix.empty());
    const std::string path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("colormix_%%%%%%.3mf")).string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(store_3mf(path.c_str(), &m, &config, false));
    const Model back = load_3mf_file(path);
    CHECK(back.colormix.unreadable_json == future);
}

TEST_CASE("ColorMix revert paints parts added after a conversion with their own tool", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    ModelObject *a = add_painted_cube(model, 6);
    {
        TriangleSelector sel(a->volumes.front()->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(1)}});
        a->volumes.front()->colormix_source_facets.set(sel);
    }
    model.colormix.source_colors = {"", "#FF0000"};
    model.colormix.bundle_id     = "prusament-pla-colormix";
    ModelObject *b = model.add_object();
    b->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    b->add_instance();
    b->config.set("extruder", 1);
    capture_source(model, {"", "#00FFFF", "#FF00FF"}); // b: cyan tool 1 -> new slot 2
    REQUIRE(b->volumes.front()->colormix_source_facets.get_data().used_states[2]);
    revert_to_source(model, 8);
    // Back on tool 1 (not on tool 2, the slot number): all in its own extruder, so unpainted again.
    CHECK(!b->volumes.front()->is_mm_painted());
    CHECK(b->volumes.front()->extruder_id() == 1);
}

TEST_CASE("ColorMix: a second colored import keeps its painting and tools, colors go to ColorMix data", "[ColorMix]")
{
    using namespace ColorMix;
    // Plater: an imported file A (white = slot 1, black = slot 2), parts on tools 1 and 2.
    Model plater;
    for (int t : {1, 2}) {
        ModelObject *o = plater.add_object();
        o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
        o->add_instance();
        o->config.set("extruder", t);
    }
    plater.colormix.source_colors = {"", "#FFFFFF", "#000000"};
    // File B: red (slot 1) and white (slot 2), a part painted with both.
    Model b;
    ModelObject *bo = b.add_object();
    ModelVolume *bv = bo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    bo->add_instance();
    bo->config.set("extruder", 1);
    {
        TriangleSelector sel(bv->mesh());
        for (int f = 0; f < int(bv->mesh().its.indices.size()); ++f)
            sel.set_facet(f, static_cast<TriangleStateType>(f < 6 ? 1 : 2));
        bv->mm_segmentation_facets.set(sel);
    }
    b.colormix.source_colors = {"", "#FF0000", "#FFFFFF"};
    const auto painting_before = bv->mm_segmentation_facets.get_data();
    REQUIRE(imported_color_count(b) == 2);
    REQUIRE(merge_imported_palette(plater, b, {"", "#FFFFFF", "#000000"}));
    // B prints exactly as its file says (no states beyond the tools, extruders unchanged) ...
    CHECK(bv->mm_segmentation_facets.get_data() == painting_before);
    CHECK(bv->extruder_id() == 1);
    // ... and its colors are ColorMix data: new slots (white is on tool 2 here, slot 1 is tool 1), with
    // the tools they print with.
    REQUIRE(plater.colormix.source_colors.size() == 5);
    CHECK(plater.colormix.source_colors[3] == "#FF0000");
    CHECK(plater.colormix.source_colors[4] == "#FFFFFF");
    const auto &used = bv->colormix_source_facets.get_data().used_states;
    CHECK(used[3]);
    CHECK(used[4]);
    CHECK(!used[1]);
    CHECK(!used[2]);
    REQUIRE(plater.colormix.slot_tools.size() == 5);
    CHECK(plater.colormix.slot_tools[3] == 1);
    CHECK(plater.colormix.slot_tools[4] == 2);
    // Not converted yet: nothing counts as a touch-up; a repaint of the imported part does.
    const std::vector<unsigned int> expected = expected_states(plater, 2);
    plater.add_object(*bo);
    CHECK(count_repainted_volumes(plater, expected, 2) == 0);
    {
        ModelVolume     *pv = plater.objects.back()->volumes.front();
        TriangleSelector sel(pv->mesh());
        sel.deserialize(pv->mm_segmentation_facets.get_data(), false);
        sel.set_facet(0, static_cast<TriangleStateType>(2));
        pv->mm_segmentation_facets.set(sel);
    }
    CHECK(count_repainted_volumes(plater, expected, 2) == 1);
}

TEST_CASE("ColorMix: the first colored import keeps its slots as they are", "[ColorMix]")
{
    using namespace ColorMix;
    Model plater; // empty
    Model a;
    ModelObject *o = a.add_object();
    o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    o->add_instance();
    o->config.set("extruder", 2);
    a.colormix.source_colors = {"", "#FF0000", "#00FF00"};
    REQUIRE(merge_imported_palette(plater, a, {"", "#FFFFFF"}));
    CHECK(plater.colormix.source_colors == a.colormix.source_colors);
    CHECK(!o->volumes.front()->has_colormix_source()); // its painting / extruder are the slots
}

TEST_CASE("ColorMix revert restores the project's own blends and blend extruders", "[ColorMix]")
{
    using namespace ColorMix;
    const Bundle b = builtin_colormix_bundle();
    Model model;
    // A Full Spectrum blend 6 painted on a part, and a second part using it as its extruder.
    add_painted_cube(model, 6);
    ModelObject *other = model.add_object();
    other->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    other->add_instance();
    other->config.set("extruder", 6);
    FullSpectrum::VirtualExtruder fs;
    fs.id         = 6;
    fs.color      = "#808080";
    fs.components = {{1, 0.5}, {2, 0.5}};
    model.virtual_extruders = {fs};
    const std::vector<std::string> colors = current_extruder_colors(model, {"#00AED5", "#D25B96", "#FFCE02", "#F1E8DD", "#544F4D"});
    const auto sources = collect_source_colors(model, colors);
    LayerSchedule sched;
    Predictor p(b, nullptr, sched);
    std::vector<Target> targets;
    for (const SourceColor &sc : sources)
        targets.push_back({hex_to_lab(sc.color), 1., sc.label});
    ConvertOptions opt;
    opt.schedule = sched;
    const ConversionResult res = convert_colors(targets, b, p, {1, 2, 3, 4, 5, 0, 0}, opt);
    capture_source(model, colors);
    apply_conversion(model, sources, res, b, {1, 2, 3, 4, 5, 0, 0}, 5);
    CHECK(other->config.extruder() == 1); // the replaced blend id no longer names the user's blend
    revert_to_source(model, 8);
    REQUIRE(model.virtual_extruders.size() == 1);
    CHECK(model.virtual_extruders.front().id == 6);
    CHECK(other->config.extruder() == 6);
}

TEST_CASE("ColorMix techniques leave ordinary painted objects alone", "[ColorMix]")
{
    // A hand-painted object (tool 1, no ColorMix source) in a project with a white core set.
    Model model;
    add_painted_cube(model, 1);
    model.colormix.slice.core_extruder  = 4;
    model.colormix.slice.region_cleanup = true;
    DynamicPrintConfig config = five_tool_config();
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    for (const Layer *l : print.objects().front()->layers())
        CHECK(layer_extruders(*l).count(4) == 0);
}

TEST_CASE("ColorMix: shell settings re-slice objects with a white core", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    ModelObject *o = add_painted_cube(model, 1);
    {
        TriangleSelector sel(o->volumes.front()->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(1)}});
        o->volumes.front()->colormix_source_facets.set(sel); // a converted object
    }
    model.colormix.slice.core_extruder = 4;
    DynamicPrintConfig config = five_tool_config();
    Print print;
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    auto core_layers = [&]() {
        size_t n = 0;
        for (const Layer *l : print.objects().front()->layers())
            n += layer_extruders(*l).count(4);
        return n;
    };
    const size_t before = core_layers();
    config.set_deserialize_strict({{"top_solid_layers", "8"}});
    print.apply(model, config);
    print.process();
    CHECK(core_layers() < before); // 5 more layers under the top keep their color
}

TEST_CASE("ColorMix: the record of the last conversion follows renumbered blends", "[ColorMix]")
{
    using namespace ColorMix;
    ModelData d;
    d.options_json = "{\"applied_states\":[0,6,7,9],\"num_physical\":5}";
    remap_conversion_record(d, {{6, 9}, {7, 10}, {9, 12}}, 8);
    const auto j = nlohmann::json::parse(d.options_json);
    CHECK(j["applied_states"] == nlohmann::json::array({0, 9, 10, 12}));
    CHECK(j["num_physical"] == 8);
}

TEST_CASE("ColorMix: an object that becomes a ColorMix object is sliced again", "[ColorMix]")
{
    Model model;
    ModelObject *o = add_painted_cube(model, 1);
    model.colormix.slice.core_extruder = 4;
    DynamicPrintConfig config = five_tool_config();
    Print print;
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    auto core_layers = [&]() {
        size_t n = 0;
        for (const Layer *l : print.objects().front()->layers())
            n += layer_extruders(*l).count(4);
        return n;
    };
    CHECK(core_layers() == 0); // an ordinary painted object: no core
    // A conversion gives it a source painting, nothing else changes.
    TriangleSelector sel(o->volumes.front()->mesh());
    sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(1)}});
    o->volumes.front()->colormix_source_facets.set(sel);
    print.apply(model, config);
    print.process();
    CHECK(core_layers() > 0);
}

TEST_CASE("ColorMix keeps source colors apart that printed alike when folding touch-ups", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    ModelObject *a = add_painted_cube(model, 1);
    ModelVolume *v = a->volumes.front();
    {
        TriangleSelector sel(v->mesh());
        for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
            sel.set_facet(f, static_cast<TriangleStateType>(f < 6 ? 1 : 2));
        v->colormix_source_facets.set(sel);
    }
    model.colormix.source_colors = {"", "#00AED5", "#00B4DC"};
    model.colormix.bundle_id     = "prusament-pla-colormix";
    const std::vector<unsigned int> applied = {0, 1, 1}; // both printed with pure tool 1
    v->mm_segmentation_facets.set(converted_painting(*v, applied, 8));
    {
        TriangleSelector sel(v->mesh());
        sel.deserialize(v->mm_segmentation_facets.get_data(), false);
        sel.set_facet(0, static_cast<TriangleStateType>(5));
        v->mm_segmentation_facets.set(sel);
    }
    std::vector<std::string> colors(10, "#808080");
    colors[1] = "#00AED5";
    colors[5] = "#00FF00";
    fold_repainted_into_source(model, applied, 8, colors);
    const auto &used = v->colormix_source_facets.get_data().used_states;
    CHECK(used[1]); // untouched triangles of slot 1
    CHECK(used[2]); // ... and of slot 2: not merged into slot 1
    REQUIRE(model.colormix.source_colors.size() == 4);
    CHECK(used[3]); // the touch-up
    CHECK(model.colormix.source_colors[3] == "#00FF00");
}

TEST_CASE("ColorMix import slots stay pending until applied, and survive saving", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    for (int i = 0; i < 2; ++i) {
        ModelObject *o = model.add_object();
        o->name = "part" + std::to_string(i);
        o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
        o->add_instance();
    }
    model.colormix.source_colors                                   = {"", "#FF0000", "#00FF00"};
    model.colormix.pending_import_slots[model.objects[0]->id().id] = 2;
    model.colormix.pending_import_slots[model.objects[1]->id().id] = 9;
    // Saved and loaded: the slots come back by object order.
    const std::string path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("colormix_%%%%%%.3mf")).string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(store_3mf(path.c_str(), &model, &config, false));
    const Model back = load_3mf_file(path);
    REQUIRE(back.objects.size() == 2);
    CHECK(back.colormix.pending_import_slots.at(back.objects[0]->id().id) == 2);
    CHECK(back.colormix.pending_import_slots.at(back.objects[1]->id().id) == 9);
    // Applied (opening the ColorMix dialog): tool 2; slot 9 is beyond 5 tools -> kept as source data.
    CHECK(apply_pending_import_slots(model, 5) == 1);
    CHECK(model.objects[0]->volumes.front()->extruder_id() == 2);
    CHECK(model.objects[1]->volumes.front()->extruder_id() == 0);
    CHECK(model.objects[1]->volumes.front()->colormix_source_facets.get_data().used_states[9]);
    CHECK(model.colormix.pending_import_slots.empty());
}

TEST_CASE("ColorMix import slots follow their objects: undo, dropped objects, multi-part loading", "[ColorMix]")
{
    using namespace ColorMix;
    auto make = [](Model &model, int n) {
        for (int i = 0; i < n; ++i) {
            ModelObject *o = model.add_object();
            o->name = "part" + std::to_string(i);
            o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
            o->add_instance();
        }
    };
    SECTION("an object the loader drops doesn't shift the others") {
        Model from;
        make(from, 3);
        from.colormix.pending_import_slots[from.objects[0]->id().id] = 1;
        from.colormix.pending_import_slots[from.objects[1]->id().id] = 2;
        from.colormix.pending_import_slots[from.objects[2]->id().id] = 3;
        from.delete_object(size_t(1)); // e.g. a zero-volume object
        Model to;
        make(to, 1); // already on the plater
        std::vector<size_t> idxs;
        for (const ModelObject *o : from.objects) {
            to.add_object(*o);
            idxs.push_back(to.objects.size() - 1);
        }
        copy_pending_slots(from, to, idxs);
        CHECK(to.colormix.pending_import_slots.size() == 2);
        CHECK(apply_pending_import_slots(to, 5) == 0);
        CHECK(to.objects[0]->volumes.front()->extruder_id() == 0);
        CHECK(to.objects[1]->volumes.front()->extruder_id() == 1);
        CHECK(to.objects[2]->volumes.front()->extruder_id() == 3);
    }
    SECTION("loaded as one multi-part object: each part keeps its object's slot, also through a project") {
        Model model;
        make(model, 2);
        model.colormix.source_colors = {"", "#FF0000", "#00FF00", "#0000FF"};
        model.colormix.pending_import_slots[model.objects[0]->id().id] = 3;
        model.colormix.pending_import_slots[model.objects[1]->id().id] = 9;
        const std::vector<unsigned int> part_slots = multipart_pending_slots(model);
        ModelProcessing::convert_to_multipart_object(model, 5);
        set_multipart_pending_slots(model, part_slots);
        REQUIRE(model.objects.size() == 1);
        REQUIRE(model.objects.front()->volumes.size() == 2);
        model.objects.front()->add_instance(); // as the plater adds one
        const std::string path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("colormix_%%%%%%.3mf")).string();
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        REQUIRE(store_3mf(path.c_str(), &model, &config, false));
        Model back = load_3mf_file(path);
        REQUIRE(back.objects.size() == 1);
        REQUIRE(back.colormix.pending_import_slots.size() == 2);
        CHECK(apply_pending_import_slots(back, 5) == 1);
        const ModelObject *o = back.objects.front();
        CHECK(o->volumes[0]->extruder_id() == 3);
        CHECK(o->volumes[1]->colormix_source_facets.get_data().used_states[9]); // beyond the tools: kept as its color
    }
    SECTION("a saved list that doesn't match the objects is dropped") {
        Model model;
        make(model, 2);
        model.colormix.import_slots = {1, 2, 3};
        pending_slots_from_file(model);
        CHECK(model.colormix.pending_import_slots.empty());
        CHECK(model.colormix.import_slots.empty());
    }
}

TEST_CASE("ColorMix: a colored import leaves the parts already on the plater ordinary", "[ColorMix]")
{
    using namespace ColorMix;
    Model plater;
    ModelObject *mine = add_painted_cube(plater, 2); // a hand-painted part on tool 2 (magenta)
    Model a;
    ModelObject *o = a.add_object();
    o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    o->add_instance();
    o->config.set("extruder", 1);
    a.colormix.source_colors = {"", "#FF0000", "#D25B96"};
    REQUIRE(merge_imported_palette(plater, a, {"#00AED5", "#D25B96"})); // per tool, as the plater gives them
    // Untouched: no ColorMix data on it, sliced as stock.
    CHECK(!mine->volumes.front()->has_colormix_source());
    CHECK(!is_colormix_object(*mine, plater.colormix.slice, plater.virtual_extruders));
    // Its tools are the first slots (their colors: the extruders' until a conversion), the import's
    // colors follow.
    REQUIRE(plater.colormix.source_colors.size() >= 4);
    CHECK(plater.colormix.source_colors[2].empty());
    CHECK(plater.colormix.source_colors[3] == "#FF0000");
    const auto &used = o->volumes.front()->colormix_source_facets.get_data().used_states;
    CHECK(used[3]); // the import's red
    // The first conversion captures the plater's part as its tool, in its color then.
    capture_source(plater, {"", "#00AED5", "#A0A0A0"});
    CHECK(mine->volumes.front()->colormix_source_facets.get_data().used_states[2]);
    CHECK(plater.colormix.source_colors[2] == "#A0A0A0");
}

TEST_CASE("ColorMix revert: a part all in its own extruder's color is unpainted again", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    ModelObject *o = model.add_object();
    ModelVolume *v = o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    o->add_instance();
    o->config.set("extruder", 2);
    {
        TriangleSelector sel(v->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(2)}});
        v->colormix_source_facets.set(sel);
        sel.remap_states({{static_cast<TriangleStateType>(2), static_cast<TriangleStateType>(7)}});
        v->mm_segmentation_facets.set(sel); // converted: a blend
    }
    model.colormix.source_colors = {"", "#00FF00", "#FF0000"};
    model.colormix.bundle_id     = "prusament-pla-colormix";
    revert_to_source(model, 8);
    CHECK(!v->is_mm_painted());
    CHECK(v->extruder_id() == 2);
}

TEST_CASE("ColorMix: a model imported into a converted project isn't a touch-up", "[ColorMix]")
{
    using namespace ColorMix;
    Model plater;
    ModelObject *conv = add_painted_cube(plater, 6);
    {
        TriangleSelector sel(conv->volumes.front()->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(1)}});
        conv->volumes.front()->colormix_source_facets.set(sel);
    }
    plater.colormix.source_colors = {"", "#FF0000", "#00FF00"}; // slot 2: another part's color (deleted since)
    plater.colormix.bundle_id     = "prusament-pla-colormix";
    plater.colormix.options_json  = R"({"applied_states": [0, 6, 3]})";
    // Import a painted model: filament 1 red (the project has it), filament 2 blue (new).
    Model b;
    ModelObject *bo = b.add_object();
    ModelVolume *bv = bo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    bo->add_instance();
    {
        TriangleSelector sel(bv->mesh());
        for (int f = 0; f < int(bv->mesh().its.indices.size()); ++f)
            sel.set_facet(f, static_cast<TriangleStateType>(f < 6 ? 1 : 2));
        bv->mm_segmentation_facets.set(sel);
    }
    b.colormix.source_colors = {"", "#FF0000", "#0000FF"};
    REQUIRE(merge_imported_palette(plater, b, {"#00AED5", "#D25B96", "#FFCE02"}));
    // Red prints with the project's blend 6, blue (new slot 3) with the file's tool 2.
    const auto &painted = bv->mm_segmentation_facets.get_data().used_states;
    CHECK(painted[6]);
    CHECK(painted[2]);
    CHECK(!painted[1]);
    // Copies included: not a touch-up.
    plater.add_object(*bo);
    plater.add_object(*bo);
    const nlohmann::json o       = nlohmann::json::parse(plater.colormix.options_json);
    const auto           applied = o["applied_states"].get<std::vector<unsigned int>>();
    CHECK(count_repainted_volumes(plater, applied, 3) == 0);
    // A touch-up of an imported part is noticed.
    {
        ModelVolume     *cv = plater.objects[1]->volumes.front();
        TriangleSelector sel(cv->mesh());
        sel.deserialize(cv->mm_segmentation_facets.get_data(), false);
        sel.set_facet(0, static_cast<TriangleStateType>(3));
        cv->mm_segmentation_facets.set(sel);
    }
    CHECK(count_repainted_volumes(plater, applied, 3) == 1);
    // Revert: blue goes back to the file's tool 2 (not to its palette slot number).
    revert_to_source(plater, 3);
    const auto &reverted = plater.objects[2]->volumes.front()->mm_segmentation_facets.get_data().used_states;
    CHECK(reverted[1]);
    CHECK(reverted[2]);
    CHECK(!reverted[3]);
}

TEST_CASE("ColorMix: tools the plater doesn't use yet keep their slots when a colored file is imported", "[ColorMix]")
{
    using namespace ColorMix;
    Model plater;
    ModelObject *mine = plater.add_object(); // unpainted on tool 1
    mine->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    mine->add_instance();
    Model a;
    ModelObject *o = a.add_object();
    o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    o->add_instance();
    o->config.set("extruder", 1);
    a.colormix.source_colors = {"", "#FFFF00"};
    REQUIRE(merge_imported_palette(plater, a, {"#FFFFFF", "#000000", "#FF0000"}));
    REQUIRE(plater.colormix.source_colors.size() == 5);
    CHECK(plater.colormix.source_colors[3].empty()); // tool 3 keeps slot 3
    CHECK(plater.colormix.source_colors[4] == "#FFFF00");
    // A part added later on tool 3 is captured as red.
    ModelObject *later = plater.add_object();
    later->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    later->add_instance();
    later->config.set("extruder", 3);
    capture_source(plater, {"", "#FFFFFF", "#000000", "#FF0000"});
    CHECK(later->volumes.front()->colormix_source_facets.get_data().used_states[3]);
}

TEST_CASE("ColorMix: Delete All brings back the project's own blends", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    FullSpectrum::VirtualExtruder fs;
    fs.id         = 6;
    fs.components = {{1, 0.5}, {2, 0.5}};
    model.colormix.options_json = nlohmann::json{{"original_virtual_extruders", FullSpectrum::serialize_virtual_extruders_to_json({}, {fs})}}.dump();
    FullSpectrum::VirtualExtruder word;
    word.id       = 6;
    word.sequence = {1, 1, 2};
    model.virtual_extruders = {word};
    const FullSpectrum::VirtualExtruders kept = blends_after_delete_all(model);
    REQUIRE(kept.size() == 1);
    CHECK(kept.front().sequence.empty());
    CHECK(kept.front().components.size() == 2);
}

TEST_CASE("ColorMix: Delete All keeps the printer's original colors and unreadable data", "[ColorMix]")
{
    using namespace ColorMix;
    ModelData d;
    d.source_colors   = {"", "#FF0000"};
    d.bundle_id       = "prusament-pla-colormix";
    d.unreadable_json = "{\"future\": 1}";
    d.options_json    = R"({"original_extruder_colors": ["#00FF00"], "extruder_colors": ["#FF0000"], "applied_states": [0, 6]})";
    const ModelData out = data_after_delete_all(d);
    CHECK(out.source_colors.empty());
    CHECK(out.bundle_id.empty());
    CHECK(out.unreadable_json == d.unreadable_json);
    const nlohmann::json o = nlohmann::json::parse(out.options_json);
    CHECK(o.contains("original_extruder_colors"));
    CHECK(o.contains("extruder_colors"));
    CHECK(!o.contains("applied_states"));
    CHECK(data_after_delete_all(ModelData()).options_json.empty());
}

TEST_CASE("ColorMix: an imported slot beyond the printer's tools isn't a touch-up in a converted project", "[ColorMix]")
{
    using namespace ColorMix;
    Model plater;
    ModelObject *conv = add_painted_cube(plater, 6);
    {
        TriangleSelector sel(conv->volumes.front()->mesh());
        sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(1)}});
        conv->volumes.front()->colormix_source_facets.set(sel);
    }
    plater.colormix.source_colors = {"", "#FF0000"};
    plater.colormix.bundle_id     = "prusament-pla-colormix";
    plater.colormix.options_json  = R"({"applied_states": [0, 6], "num_physical": 4})";
    // File slot 6 on a 4-tool printer: the plater sets it as the extruder for the merge only.
    Model b;
    ModelObject *bo = b.add_object();
    bo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    bo->add_instance();
    b.colormix.source_colors = {"", "#111111", "#222222", "#333333", "#444444", "#555555", "#00FF00"};
    bo->config.set("extruder", 6);
    REQUIRE(merge_imported_palette(plater, b, {"#00AED5", "#D25B96", "#FFCE02", "#F1E8DD"}));
    bo->config.erase("extruder");
    plater.add_object(*bo);
    CHECK(count_repainted_volumes(plater, expected_states(plater, 4), 4) == 0);
}

TEST_CASE("ColorMix: importing onto a plater whose objects were deleted starts afresh", "[ColorMix]")
{
    using namespace ColorMix;
    Model plater; // e.g. a calibration plate's data, its objects deleted one by one
    plater.colormix.bundle_id           = "prusament-pla-colormix";
    plater.colormix.slice.core_extruder = 4;
    Model a;
    ModelObject *o = a.add_object();
    o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    o->add_instance();
    a.colormix.source_colors = {"", "#FF0000", "#00FF00"};
    REQUIRE(merge_imported_palette(plater, a, {"#FFFFFF"}));
    CHECK(plater.colormix.bundle_id.empty());
    CHECK(plater.colormix.slice.core_extruder == 0);
    CHECK(plater.colormix.source_colors == a.colormix.source_colors);
}

TEST_CASE("ColorMix: a file slot beyond the printer's tools prints with the part's own extruder", "[ColorMix]")
{
    using namespace ColorMix;
    SECTION("imported into a converted project: no unrelated blend") {
        Model plater;
        ModelObject *conv = add_painted_cube(plater, 6);
        {
            TriangleSelector sel(conv->volumes.front()->mesh());
            sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(1)}});
            conv->volumes.front()->colormix_source_facets.set(sel);
        }
        plater.colormix.source_colors = {"", "#FF0000"};
        plater.colormix.bundle_id     = "prusament-pla-colormix";
        plater.colormix.options_json  = R"({"applied_states": [0, 6], "num_physical": 4})";
        Model b;
        ModelObject *bo = b.add_object();
        ModelVolume *bv = bo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
        bo->add_instance();
        b.colormix.source_colors = {"", "#111111", "#222222", "#333333", "#444444", "#555555", "#00FF00"};
        bo->config.set("extruder", 6);
        REQUIRE(merge_imported_palette(plater, b, {"#00AED5", "#D25B96", "#FFCE02", "#F1E8DD"}));
        bo->config.erase("extruder");
        CHECK(!bv->is_mm_painted()); // not blend 6 (red's), not tool 1: its own extruder
        plater.add_object(*bo);
        CHECK(count_repainted_volumes(plater, expected_states(plater, 4), 4) == 0);
        revert_to_source(plater, 4);
        CHECK(!plater.objects.back()->volumes.front()->is_mm_painted()); // on its own extruder again
    }
    SECTION("kept when the dialog opens: no touch-up, revert leaves it on its extruder (Full Spectrum blend 6 present)") {
        Model model;
        ModelObject *o = model.add_object();
        o->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
        o->add_instance();
        FullSpectrum::VirtualExtruder fs;
        fs.id         = 6;
        fs.components = {{1, 0.5}, {2, 0.5}};
        model.virtual_extruders = {fs};
        model.colormix.source_colors = {"", "#111111", "#222222", "#333333", "#444444", "#555555", "#00FF00"};
        model.colormix.pending_import_slots[o->id().id] = 6;
        CHECK(apply_pending_import_slots(model, 4) == 1);
        CHECK(count_repainted_volumes(model, expected_states(model, 4), 4) == 0);
        CHECK(count_repainted_volumes(model, expected_states(model, 8), 8) == 0); // also on an 8-tool printer
        revert_to_source(model, 4);
        CHECK(!o->volumes.front()->is_mm_painted());
    }
}

TEST_CASE("ColorMix: projects converted by builds that kept no record ask nothing", "[ColorMix]")
{
    using namespace ColorMix;
    Model model;
    ModelObject *o = add_painted_cube(model, 7);
    TriangleSelector sel(o->volumes.front()->mesh());
    sel.remap_states({{TriangleStateType::NONE, static_cast<TriangleStateType>(1)}});
    o->volumes.front()->colormix_source_facets.set(sel);
    model.colormix.source_colors = {"", "#FF0000"};
    model.colormix.bundle_id     = "prusament-pla-colormix";
    CHECK(expected_states(model, 5).empty());
}

#include "libslic3r/FileReader.hpp"
// Diagnostic: what the GUI's project-style load (FileReader::load_model_with_config) returns for a file.
//   COLORMIX_TEST_3MF=<file.3mf> fff_print_tests "[ColorMixLoadDiag]"
TEST_CASE("ColorMix load diagnostics", "[.][ColorMixLoadDiag]")
{
    const char *path = std::getenv("COLORMIX_TEST_3MF");
    if (path == nullptr)
        return;
    DynamicPrintConfig        config_loaded;
    ConfigSubstitutionContext subst{ForwardCompatibilitySubstitutionRule::Enable};
    boost::optional<Semver>   version;
    FileReader::LoadStats     stats;
    Model model = FileReader::load_model_with_config(path, &config_loaded, &subst, version, FileReader::LoadAttribute::CheckVersion, &stats);
    std::cout << "objects " << model.objects.size() << ", config keys " << config_loaded.keys().size() << ", generator version "
              << (version ? version->to_string() : std::string("none")) << ", source colors " << model.colormix.source_colors.size()
              << ", pending slots " << model.colormix.pending_import_slots.size() << ", painted "
              << (model.objects.empty() ? 0 : int(model.objects.front()->volumes.front()->is_mm_painted())) << "\n";
    for (const std::string &k : config_loaded.keys())
        std::cout << "  config: " << k << "\n";
}

TEST_CASE("ColorMix works with any number of tools", "[ColorMix]")
{
    using namespace ColorMix;
    const Bundle bundle = builtin_colormix_bundle();
    for (unsigned int n : {1u, 2u, 4u, 8u, 20u}) {
        INFO(n << " tools");
        // Spools on the first tools, the rest "not loaded" (as the dialog's default).
        std::vector<unsigned int> tools(bundle.filaments.size(), 0);
        for (size_t i = 0; i < tools.size() && i < n; ++i)
            tools[i] = unsigned(i + 1);
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        std::string nd, mn, mx, colors;
        for (unsigned int t = 0; t < n; ++t) {
            nd += std::string(t ? "," : "") + "0.4";
            mn += std::string(t ? "," : "") + "0.05";
            mx += std::string(t ? "," : "") + "0.25";
            colors += std::string(t ? ";" : "") + (t < bundle.filaments.size() ? bundle.filaments[t].color : "#808080");
        }
        config.set_deserialize_strict({{"nozzle_diameter", nd}, {"min_layer_height", mn}, {"max_layer_height", mx},
                                       {"extruder_colour", colors}, {"filament_colour", colors}, {"wipe_tower", "0"},
                                       {"layer_height", "0.1"}, {"first_layer_height", "0.2"}, {"perimeters", "2"},
                                       {"fill_density", "15%"}, {"skirts", "0"}});
        Model model;
        add_painted_cube(model, 1, 10.);
        ModelObject *b = add_painted_cube(model, 2, 10.);
        b->instances.front()->set_offset(Vec3d(130., 100., 0.));
        model.colormix.source_colors = {"", "#3CA85A", "#7A5AA8"}; // a green and a purple: blends (C+Y, C+M)
        LayerSchedule sched;
        Predictor     predictor(bundle, nullptr, sched);
        std::vector<std::string> phys;
        for (unsigned int t = 0; t < n; ++t)
            phys.push_back(t < bundle.filaments.size() ? bundle.filaments[t].color : "#808080");
        const std::vector<SourceColor> sources = collect_source_colors(model, current_extruder_colors(model, phys));
        REQUIRE(sources.size() == 2);
        std::vector<Target> targets;
        for (const SourceColor &s : sources)
            targets.push_back({hex_to_lab(s.color), 1., s.label});
        ConvertOptions opt;
        opt.schedule    = sched;
        opt.max_virtual = int(TRIANGLE_STATE_TYPE_COUNT) - 1 - std::max(16, int(n));
        const ConversionResult res = convert_colors(targets, bundle, predictor, tools, opt);
        REQUIRE(res.problem.empty());
        capture_source(model, current_extruder_colors(model, phys));
        const ApplySummary sum = apply_conversion(model, sources, res, bundle, tools, n);
        // Blends get extruder numbers after the physical tools; every recipe component is a real tool.
        for (const FullSpectrum::VirtualExtruder &ve : model.virtual_extruders) {
            CHECK(ve.id > n);
            for (const auto &c : ve.components)
                CHECK((c.extruder_id >= 1 && c.extruder_id <= n));
        }
        if (n == 1)
            CHECK(model.virtual_extruders.empty()); // one spool: nothing to mix
        else
            CHECK_FALSE(model.virtual_extruders.empty());
        Print print;
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();
        // Every layer prints with physical tools only.
        for (const PrintObject *po : print.objects())
            for (size_t L = 0; L < po->layer_count(); ++L)
                for (const auto &[e, area] : layer_extruders(*po->get_layer(int(L))))
                    CHECK((e >= 1 && e <= n));
        const std::string g = Test::gcode(print);
        CHECK_FALSE(g.empty());
        (void) sum;
    }
}
