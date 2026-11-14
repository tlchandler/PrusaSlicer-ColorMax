///|/ ColorMax engine - GUI: convert a colored model, calibrate a filament bundle.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "ColorMixDialog.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/progdlg.h>
#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/filedlg.h>
#include <wx/grid.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/mstream.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "Sidebar.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "../Utils/Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r::GUI {

using namespace Slic3r::ColorMix;

namespace {

wxBitmap swatch_bitmap(const std::string &hex, int w, int h)
{
    wxBitmap   bmp(w, h);
    wxMemoryDC dc(bmp);
    wxColour   c;
    c.Set(wxString::FromUTF8(normalize_hex(hex).empty() ? std::string("#808080") : normalize_hex(hex)));
    dc.SetBrush(wxBrush(c));
    dc.SetPen(wxPen(wxColour(90, 90, 90)));
    dc.DrawRectangle(0, 0, w, h);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

// Sets the extruder colors of the edited printer preset through the Printer tab, so the tab shows them and
// marks the preset modified. Empty entries keep the current color unless `set_empty`.
void set_tool_colors(Plater *plater, const std::vector<std::string> &colors, bool set_empty = false)
{
    Tab *printer_tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (printer_tab == nullptr)
        return;
    DynamicPrintConfig   cfg = *printer_tab->get_config();
    ConfigOptionStrings *opt = cfg.option<ConfigOptionStrings>("extruder_colour");
    if (opt == nullptr || opt->values.empty())
        return;
    for (size_t i = 0; i < std::min(colors.size(), opt->values.size()); ++i)
        if (set_empty || !colors[i].empty())
            opt->values[i] = colors[i];
    printer_tab->load_config(cfg);
    plater->on_config_change(cfg);
    plater->force_filament_colors_update();
    plater->sidebar().update_presets(Preset::TYPE_FILAMENT);
}

// The extruder colors of the edited printer preset as they are stored (empty = filament color).
std::vector<std::string> printer_extruder_colors()
{
    if (Tab *printer_tab = wxGetApp().get_tab(Preset::TYPE_PRINTER); printer_tab != nullptr)
        if (const auto *opt = printer_tab->get_config()->option<ConfigOptionStrings>("extruder_colour"))
            return opt->values;
    return {};
}

// A JSON object from text; an empty object when the text is empty or not a JSON object.
nlohmann::json json_object(const std::string &text)
{
    if (!text.empty()) {
        try {
            nlohmann::json j = nlohmann::json::parse(text);
            if (j.is_object())
                return j;
        } catch (const std::exception &) {
        }
    }
    return nlohmann::json::object();
}

// The printer's extruder colors from before ColorMix changed them: the project's record, else the
// current ones.
nlohmann::json original_extruder_colors(const Model &model)
{
    const nlohmann::json o = json_object(model.colormix.options_json);
    if (auto it = o.find("original_extruder_colors"); it != o.end() && it->is_array())
        return *it;
    return printer_extruder_colors();
}

// After ColorMix set the extruder colors: they and the colors from before are recorded with the
// project, so undo / redo across this change restores the matching colors.
void record_extruder_colors(Model &model, const nlohmann::json &original)
{
    nlohmann::json o = json_object(model.colormix.options_json);
    if (!o.contains("original_extruder_colors") && original.is_array())
        o["original_extruder_colors"] = original;
    o["extruder_colors"]        = printer_extruder_colors();
    model.colormix.options_json = o.dump();
}

wxString bundle_label(const Bundle &b)
{
    return from_u8(b.name) + (b.user_bundle ? " " + _L("(user)") : wxString());
}

// Names the bundle filaments assigned to the same tool; empty when every loaded filament has its own tool.
wxString duplicate_tools_message(const Bundle &b, const std::vector<unsigned int> &tools)
{
    std::map<unsigned int, std::vector<std::string>> by_tool;
    for (size_t f = 0; f < tools.size() && f < b.filaments.size(); ++f)
        if (tools[f] > 0)
            by_tool[tools[f]].push_back(b.filaments[f].name);
    wxString lines;
    for (const auto &[tool, names] : by_tool)
        if (names.size() > 1)
            lines += "\n" + format_wxstr(_L("Tool %1%: %2%"), tool, from_u8(boost::algorithm::join(names, ", ")));
    if (lines.empty())
        return wxString();
    return _L("Each tool can hold only one filament. These filaments are assigned to the same tool:") + lines;
}

// A swatch label without its "S12  " / "S123  " number: identifies the recipe across plan changes.
// App config: settings of the last conversion (defaults for new projects) and the last plate file.
const char *LAST_SETTINGS_KEY = "colormix_last_settings";
const char *LAST_PLATE_FILE_KEY = "colormix_last_plate_file"; // folder for the plate file dialogs

std::string swatch_key(const std::string &label)
{
    const size_t sep = label.find("  ");
    return sep == std::string::npos ? label : label.substr(sep + 2);
}

bool check_tools(wxWindow *parent, const Bundle &b, const std::vector<unsigned int> &tools)
{
    const wxString msg = duplicate_tools_message(b, tools);
    if (msg.empty())
        return true;
    MessageDialog(parent, msg, _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
    return false;
}

// Before replacing the plater content with a calibration plate: offers to save a modified project
// (like New Project does), else asks for confirmation when there are objects. False = cancelled.
bool close_current_project(wxWindow *parent, Plater *plater, const wxString &reason)
{
    if (plater->is_project_dirty())
        return plater->save_project_if_dirty(reason) != wxID_CANCEL;
    if (plater->model().objects.empty())
        return true;
    return MessageDialog(parent, reason + "\n" + _L("Continue?"), _L("ColorMax Engine"), wxYES_NO | wxICON_QUESTION).ShowModal() == wxID_YES;
}

std::string format_schedule(const std::vector<double> &s)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    for (size_t i = 0; i < s.size(); ++i)
        out << (i ? ", " : "") << s[i];
    return out.str();
}

// "L a b" with a '.' decimal point whatever the locale, so parse_measurement() reads it back.
std::string format_lab(const Lab &l, int decimals)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(decimals) << l.L << ' ' << l.a << ' ' << l.b;
    return out.str();
}

// Measurements: see ColorMix::parse_lab_measurement().
bool parse_measurement(const std::string &text, Lab &out) { return parse_lab_measurement(text, out); }

} // namespace

std::vector<double> parse_layer_schedule(const std::string &text_in)
{
    std::string text = text_in;
    std::replace(text.begin(), text.end(), ',', ' ');
    std::replace(text.begin(), text.end(), ';', ' ');
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    std::vector<double> out;
    double h;
    while (in >> h) {
        if (!(h >= 0.03 && h <= 0.6))
            return {};
        out.push_back(h);
    }
    // Stopped before the end: something that isn't a number.
    if (!in.eof() || out.size() > 16)
        return {};
    return out;
}

// ---------------------------------------------------------------- context

std::string ColorMixContext::system_bundle_dir() { return (boost::filesystem::path(resources_dir()) / "colormix" / "bundles").string(); }
std::string ColorMixContext::user_bundle_dir() { return (boost::filesystem::path(data_dir()) / "colormix" / "bundles").string(); }

ColorMixContext ColorMixContext::load(Plater *plater)
{
    ColorMixContext ctx;
    ctx.bundles = load_bundles(system_bundle_dir(), user_bundle_dir(), &ctx.load_errors);
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->full_config();
    if (const auto *nd = cfg.option<ConfigOptionFloats>("nozzle_diameter"))
        ctx.num_physical = unsigned(std::max<size_t>(1, nd->values.size()));
    ctx.physical_colors = plater->get_extruder_color_strings_from_plater_config();
    ctx.physical_colors.resize(ctx.num_physical, "#808080");
    if (const auto *lh = cfg.option<ConfigOptionFloat>("layer_height"))
        ctx.layer_height = lh->value;
    if (const auto *flh = cfg.option<ConfigOptionFloatOrPercent>("first_layer_height"))
        ctx.first_layer_height = flh->get_abs_value(ctx.layer_height);
    // As Print::validate() checks a layer schedule: per tool, max 0 = 3/4 of the nozzle.
    const auto *nd  = cfg.option<ConfigOptionFloats>("nozzle_diameter");
    const auto *mn  = cfg.option<ConfigOptionFloats>("min_layer_height");
    const auto *mx  = cfg.option<ConfigOptionFloats>("max_layer_height");
    for (unsigned int e = 0; nd != nullptr && e < nd->values.size(); ++e) {
        const double lo = mn != nullptr ? mn->get_at(e) : 0.;
        double       hi = mx != nullptr ? mx->get_at(e) : 0.;
        if (hi <= 0.)
            hi = 0.75 * nd->get_at(e);
        ctx.min_layer_height = std::max(ctx.min_layer_height, lo);
        ctx.max_layer_height = e == 0 ? hi : std::min(ctx.max_layer_height, hi);
    }
    return ctx;
}

// Layer heights outside the printer's limits make slicing fail: asks whether to keep them.
static bool schedule_within_limits(wxWindow *parent, const ColorMixContext &ctx, const std::vector<double> &heights)
{
    if (ctx.max_layer_height <= 0.)
        return true;
    for (double h : heights)
        if (h < ctx.min_layer_height - 1e-6 || h > ctx.max_layer_height + 1e-6)
            return MessageDialog(parent,
                                 format_wxstr(_L("The layer height %1% mm is outside the printer's layer height limits (%2% - %3% mm): "
                                                 "slicing will fail until the limits or the heights change. Use these heights anyway?"),
                                              wxString::Format("%.3g", h), wxString::Format("%.3g", ctx.min_layer_height),
                                              wxString::Format("%.3g", ctx.max_layer_height)),
                                 _L("ColorMax Engine"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING).ShowModal() == wxID_YES;
    return true;
}

std::vector<unsigned int> ColorMixContext::default_tools(const Bundle &b) const
{
    std::vector<unsigned int> out;
    for (const BundleFilament &f : b.filaments)
        out.push_back(f.default_tool <= num_physical ? f.default_tool : 0);
    return out;
}

// ---------------------------------------------------------------- tool panel

ColorMixToolPanel::ColorMixToolPanel(wxWindow *parent) : wxPanel(parent, wxID_ANY)
{
    m_grid = new wxFlexGridSizer(4, wxSize(8, 4));
    SetSizer(m_grid);
}

void ColorMixToolPanel::set_bundle(const Bundle &bundle, const std::vector<unsigned int> &tools, unsigned int num_physical)
{
    m_grid->Clear(true);
    m_tools.clear();
    m_grid->SetCols(8);
    const int em = wxGetApp().em_unit();
    const int sw_size = std::max(8, int(1.8 * em));
    wxArrayString choices;
    choices.Add(_L("Not loaded"));
    for (unsigned int t = 1; t <= num_physical; ++t)
        choices.Add(format_wxstr(_L("Tool %1%"), t));
    for (size_t i = 0; i < bundle.filaments.size(); ++i) {
        const BundleFilament &f = bundle.filaments[i];
        auto *sw = new wxStaticBitmap(this, wxID_ANY, swatch_bitmap(f.color, sw_size, sw_size));
        wxString label = from_u8(f.name);
        if (f.role == MixRole::Solid)
            label += " " + _L("(solid only)");
        else if (f.role == MixRole::Accent)
            label += " " + _L("(accent)");
        auto *txt  = new wxStaticText(this, wxID_ANY, label);
        const unsigned int t = i < tools.size() && tools[i] <= num_physical ? tools[i] : 0; // a tool the printer lacks: not loaded
        auto *choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
        choice->SetSelection(int(t));
        choice->SetToolTip(_L("The tool (toolhead) this spool is loaded in. \"Not loaded\": ColorMax works without it, e.g. when a spool ran "
                              "out - the colors are made from the other spools."));
        choice->Bind(wxEVT_CHOICE, [this, txt, choice](wxCommandEvent &) {
            txt->Enable(choice->GetSelection() > 0);
            if (on_change)
                on_change();
        });
        txt->Enable(t > 0);
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(sw, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(txt, 0, wxALIGN_CENTER_VERTICAL);
        m_grid->Add(row, 0, wxALIGN_CENTER_VERTICAL);
        m_grid->Add(choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        m_tools.push_back(choice);
    }
    Layout();
}

std::vector<unsigned int> ColorMixToolPanel::tool_map() const
{
    std::vector<unsigned int> out;
    for (const wxChoice *c : m_tools)
        out.push_back(unsigned(std::max(0, c->GetSelection())));
    return out;
}

// ---------------------------------------------------------------- core tool picker

void ColorMixCorePicker::create(wxWindow *parent, wxSizer *sizer, const wxString &label, const wxString &tooltip, int em)
{
    m_check = new wxCheckBox(parent, wxID_ANY, label);
    m_check->SetValue(true);
    m_check->SetToolTip(tooltip);
    sizer->Add(m_check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_choice = new wxChoice(parent, wxID_ANY);
    m_choice->SetToolTip(_L("The core prints from the tool holding the bundle's White, or from a tool with no filament of the bundle "
                            "(an extra spool such as an opaque white)."));
    sizer->Add(m_choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_check->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        m_choice->Enable(m_check->GetValue());
        if (on_change)
            on_change();
    });
    m_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        m_white_selected = m_white_tool > 0 && selected_tool() == m_white_tool;
        if (on_change)
            on_change();
    });
}

void ColorMixCorePicker::refill(const Bundle &bundle, const std::vector<unsigned int> &tools, unsigned int num_physical,
                                unsigned int preferred, bool init_check)
{
    const unsigned int prev       = selected_tool();
    const bool         prev_white = m_white_selected;
    m_allowed                     = allowed_core_tools(bundle, tools, num_physical);
    m_white_tool                  = white_tool(bundle, tools);
    auto allowed = [this](unsigned int t) { return t > 0 && std::find(m_allowed.begin(), m_allowed.end(), t) != m_allowed.end(); };
    unsigned int target = 0;
    if (allowed(preferred))
        target = preferred;
    else if (prev_white && allowed(m_white_tool))
        target = m_white_tool;
    else if (allowed(prev))
        target = prev;
    else if (allowed(m_white_tool))
        target = m_white_tool;
    else if (!m_allowed.empty())
        target = m_allowed.front();

    const int w = bundle.filament_index("white");
    m_choice->Clear();
    int sel = wxNOT_FOUND;
    for (size_t i = 0; i < m_allowed.size(); ++i) {
        const unsigned int t = m_allowed[i];
        m_choice->Append(t == m_white_tool && w >= 0 ? format_wxstr(_L("Tool %1% (%2%)"), t, from_u8(bundle.filaments[size_t(w)].name))
                                                     : format_wxstr(_L("Tool %1% (extra spool)"), t));
        if (t == target)
            sel = int(i);
    }
    m_choice->SetSelection(sel);
    m_white_selected = m_white_tool > 0 && target == m_white_tool;
    if (m_allowed.empty()) {
        m_check->SetValue(false);
        m_check->Enable(false);
    } else {
        m_check->Enable(true);
        if (init_check)
            m_check->SetValue(allowed(preferred));
    }
    m_choice->Enable(m_check->GetValue() && !m_allowed.empty());
}

void ColorMixCorePicker::set_checked(bool on)
{
    m_check->SetValue(on && !m_allowed.empty());
    m_choice->Enable(m_check->GetValue());
}

unsigned int ColorMixCorePicker::selected_tool() const
{
    const int i = m_choice != nullptr ? m_choice->GetSelection() : wxNOT_FOUND;
    return i >= 0 && size_t(i) < m_allowed.size() ? m_allowed[size_t(i)] : 0;
}

void ColorMixCorePicker::enable(bool on)
{
    if (m_check != nullptr)
        m_check->Enable(on);
    if (m_choice != nullptr)
        m_choice->Enable(on);
}

unsigned int ColorMixCorePicker::tool() const { return m_check != nullptr && m_check->GetValue() ? selected_tool() : 0; }

// ---------------------------------------------------------------- conversion dialog

ColorMixDialog::ColorMixDialog(wxWindow *parent, Plater *plater)
    : DPIDialog(parent, wxID_ANY, _L("ColorMax Engine - convert model colors"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_plater(plater)
{
    m_ctx = ColorMixContext::load(plater);
    // Opening ColorMix applies the color slots of imported colored models (they loaded as in stock
    // PrusaSlicer): each object prints with its slot's tool.
    if (m_plater->model().colormix.has_pending_import_slots()) {
        m_plater->take_snapshot(_L("ColorMax: tools of the imported colors"));
        m_beyond_tools  = apply_pending_import_slots(m_plater->model(), m_ctx.num_physical);
        m_model_changed = true;
    }
    build();
    load_settings_from_model();
    on_bundle_changed();
    SetMinSize(wxSize(80 * wxGetApp().em_unit(), 60 * wxGetApp().em_unit()));
    Fit();
    CenterOnParent();
    // Nothing is computed until Preview (or Apply) is pressed.
    if (!m_plater->model().colormix.plate_json.empty())
        m_summary->SetLabel(_L("The plater holds a calibration plate or slope test: there is nothing to convert. Open your model, then press Preview."));
}

ColorMixDialog::~ColorMixDialog() { delete m_images; }

void ColorMixDialog::on_dpi_changed(const wxRect &) { Layout(); Refresh(); }

void ColorMixDialog::build()
{
    const int em  = wxGetApp().em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY,
        _L("ColorMax prints the colors of your model as mixes of the spools loaded in your printer: thin layers of different "
           "filaments that blend into one color. Check the spools, press Preview to see how every color will print, then Apply."));
    intro->Wrap(90 * em);
    top->Add(intro, 0, wxEXPAND | wxALL, em);

    // ---- 1. Filaments
    auto     *fil_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("1. Filaments"));
    wxWindow *fb      = fil_box->GetStaticBox();
    auto     *row     = new wxBoxSizer(wxHORIZONTAL);
    auto     *lbl_b   = new wxStaticText(fb, wxID_ANY, _L("Filament set:"));
    row->Add(lbl_b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_bundle_choice = new wxChoice(fb, wxID_ANY);
    for (const Bundle &b : m_ctx.bundles)
        m_bundle_choice->Append(bundle_label(b));
    m_bundle_choice->SetToolTip(_L("The set of filaments you mix with (a \"bundle\"): their colors and which ones can be blended. "
                                   "\"(user)\" marks your own copy, which holds your calibrations."));
    row->Add(m_bundle_choice, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    auto *new_set  = new wxButton(fb, wxID_ANY, _L("New..."), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    auto *edit_set = new wxButton(fb, wxID_ANY, _L("Edit..."), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    new_set->SetToolTip(_L("Create a filament set from your own spools (it starts with the spools loaded in the printer)."));
    edit_set->SetToolTip(_L("Change the filaments, colors and rules of this set. Its calibrations are kept."));
    auto *range_set = new wxButton(fb, wxID_ANY, _L("Color range..."), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    range_set->SetToolTip(_L("A chart of the colors this set can print with your spools, and where your model's colors fall in it."));
    row->Add(new_set, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 4);
    row->Add(edit_set, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 4);
    row->Add(range_set, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 * em);
    range_set->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const Bundle *b = bundle();
        if (b == nullptr)
            return;
        const Calibration *cal = calibration();
        std::vector<std::pair<Lab, std::string>> colors;
        {
            wxBusyCursor wait;
            const Model &model = m_plater->model();
            for (const SourceColor &sc : collect_source_colors(model, current_extruder_colors(model, m_ctx.physical_colors)))
                colors.emplace_back(hex_to_lab(sc.color), sc.label);
        }
        ColorMixGamutDialog dlg(this, *b, cal ? int(cal - b->calibrations.data()) : -1, m_tools->tool_map(), schedule(),
                                m_use_blends->GetValue(), m_dual->GetValue(), std::move(colors));
        dlg.ShowModal();
    });
    new_set->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        ColorMixBundleDialog dlg(this, m_ctx, nullptr, false);
        if (dlg.ShowModal() == wxID_OK)
            reload_bundles(dlg.saved_id());
    });
    edit_set->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const Bundle *b = bundle();
        if (b == nullptr)
            return;
        ColorMixBundleDialog dlg(this, m_ctx, b, true);
        if (dlg.ShowModal() == wxID_OK)
            reload_bundles(dlg.saved_id());
    });
    row->Add(new wxStaticText(fb, wxID_ANY, _L("Color calibration:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_cal_choice = new wxChoice(fb, wxID_ANY);
    m_cal_choice->SetToolTip(_L("Measurements of a calibration plate printed on your printer. With one, predicted colors are typically "
                                "within 2-3 ΔE of the print; without, 5-10 ΔE."));
    row->Add(m_cal_choice, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    auto *calib = new wxButton(fb, wxID_ANY, _L("Calibrate..."));
    calib->SetToolTip(_L("Print a calibration plate, scan it and save a calibration for this filament set."));
    row->Add(calib, 0, wxALIGN_CENTER_VERTICAL);
    fil_box->Add(row, 0, wxEXPAND | wxALL, em / 2);
    m_cal_info = new wxStaticText(fb, wxID_ANY, wxEmptyString);
    fil_box->Add(m_cal_info, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em / 2);
    if (!m_ctx.load_errors.empty()) {
        auto *err = new wxStaticText(fb, wxID_ANY, _L("Some filament set files could not be read:") + " " + from_u8(boost::algorithm::join(m_ctx.load_errors, "; ")));
        err->SetForegroundColour(*wxRED);
        fil_box->Add(err, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em / 2);
    }
    fil_box->Add(new wxStaticText(fb, wxID_ANY, _L("Where each spool is loaded (\"Not loaded\" if you don't have it in the printer):")), 0, wxLEFT | wxRIGHT, em / 2);
    m_tools = new ColorMixToolPanel(fb);
    m_tools->on_change = [this]() {
        if (const Bundle *b = bundle())
            m_core.refill(*b, m_tools->tool_map(), m_ctx.num_physical, 0, false);
        invalidate();
    };
    fil_box->Add(m_tools, 0, wxEXPAND | wxALL, em / 2);
    top->Add(fil_box, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

    // ---- 2. How to print the colors
    auto     *opt_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("2. How to print the colors"));
    wxWindow *ob      = opt_box->GetStaticBox();
    auto add_check = [&](wxWindow *parent, wxSizer *s, const wxString &label, const wxString &tip, bool value) {
        auto *c = new wxCheckBox(parent, wxID_ANY, label);
        c->SetValue(value);
        c->SetToolTip(tip);
        c->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { invalidate(); });
        s->Add(c, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 * em);
        return c;
    };
    auto add_label = [&](wxWindow *parent, wxSizer *s, const wxString &text, const wxString &tip = wxString()) {
        auto *t = new wxStaticText(parent, wxID_ANY, text);
        if (!tip.empty())
            t->SetToolTip(tip);
        s->Add(t, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
        return t;
    };
    auto add_spin = [&](wxWindow *parent, wxSizer *s, double min, double max, double value, double inc, const wxString &tip) {
        auto *sp = new wxSpinCtrlDouble(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(7 * em, -1), wxSP_ARROW_KEYS, min, max, value, inc);
        sp->SetToolTip(tip);
        sp->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxCommandEvent &) { invalidate(); });
        s->Add(sp, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 * em);
        return sp;
    };
    auto *r1 = new wxBoxSizer(wxHORIZONTAL);
    m_use_blends = add_check(ob, r1, _L("Mix colors"),
        _L("Print colors as mixes of the loaded spools, alternating filaments layer by layer. Off: every color prints with the closest "
           "single spool."), true);
    m_dual = add_check(ob, r1, _L("Dual walls"),
        _L("The inner wall gets its own mix. The outer wall is translucent and shows it through, which gives many more colors "
           "(lighter, darker, in-between). Costs extra tool changes. Needs a calibration with dual-wall swatches to predict well."), false);
    m_top = add_check(ob, r1, _L("Patterned tops"),
        _L("Flat tops and shallow slopes show only one layer, so a layer mix would show one filament there. They are tiled with small "
           "cells of the mix's filaments instead."), true);
    m_cleanup = add_check(ob, r1, _L("Clean up specks"),
        _L("Tiny color islands and very short color runs go to the neighbouring color: each costs tool changes and shows as a speck."), true);
    m_core.create(ob, r1, _L("White core:"),
                  _L("Print the inside of the object (deeper than the wall depth) in one filament, normally White. Light that passes "
                     "the translucent walls is reflected back, so colors look bright and clean."),
                  em);
    m_core.on_change = [this]() { invalidate(); };
    opt_box->Add(r1, 0, wxEXPAND | wxALL, em / 2);

    // Advanced settings, folded away.
    auto *pane = new wxCollapsiblePane(ob, wxID_ANY, _L("Advanced settings"), wxDefaultPosition, wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    pane->SetToolTip(_L("Fine-tuning of the techniques above. The defaults suit most models."));
    wxWindow *ap  = pane->GetPane();
    auto     *as  = new wxBoxSizer(wxVERTICAL);
    auto     *a1  = new wxBoxSizer(wxHORIZONTAL);
    add_label(ap, a1, _L("White core: wall depth (mm)"));
    m_core_depth = add_spin(ap, a1, 0.3, 5., 0.9, 0.05, _L("How deep the colored walls are before the core starts. Thinner shows more of the core's whiteness."));
    add_label(ap, a1, _L("Dual walls: outer wall width (mm)"));
    m_dual_width = add_spin(ap, a1, 0.25, 1.2, 0.45, 0.05, _L("About one external perimeter width. Narrower lets more of the inner color through."));
    as->Add(a1, 0, wxEXPAND | wxBOTTOM, em / 2);
    auto *a2 = new wxBoxSizer(wxHORIZONTAL);
    add_label(ap, a2, _L("Patterned tops: cell (mm)"));
    m_top_cell = add_spin(ap, a2, 0.8, 5., 1.5, 0.1, _L("Size of the pattern cells on tops. Smaller blends better from close up; larger prints more cleanly."));
    add_label(ap, a2, _L("layers"));
    m_top_layers = new wxSpinCtrl(ap, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(6 * em, -1), wxSP_ARROW_KEYS, 1, 10, 3);
    m_top_layers->SetToolTip(_L("How many layers under a top get the pattern."));
    m_top_layers->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { invalidate(); });
    a2->Add(m_top_layers, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 * em);
    const wxString band_tip = _L("On a slope each layer shows as a band (layer height / sine of the slope). Where the bands are wider "
                                 "than this, layer mixing shows as stripes, so the surface gets the pattern instead. Print the slope "
                                 "test (Calibrate...) to choose the value. 0 = only flat tops.");
    add_label(ap, a2, _L("also slopes with bands wider than (mm)"), band_tip);
    m_patch_band = new wxSpinCtrlDouble(ap, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(7 * em, -1), wxSP_ARROW_KEYS, 0., 2., 0.3, 0.05);
    m_patch_band->SetToolTip(band_tip);
    a2->Add(m_patch_band, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_patch_angle = new wxStaticText(ap, wxID_ANY, wxEmptyString);
    a2->Add(m_patch_angle, 0, wxALIGN_CENTER_VERTICAL);
    m_patch_band->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxCommandEvent &) { update_patch_angle(); invalidate(); });
    as->Add(a2, 0, wxEXPAND | wxBOTTOM, em / 2);
    auto *a3 = new wxBoxSizer(wxHORIZONTAL);
    add_label(ap, a3, _L("Clean up specks: shortest run (mm)"));
    m_min_run = add_spin(ap, a3, 0., 3., 0.3, 0.05, _L("A color running a shorter distance along a layer's outline is absorbed by its neighbour."));
    add_label(ap, a3, _L("smallest island (mm²)"));
    m_min_island = add_spin(ap, a3, 0., 20., 0.5, 0.1, _L("A patch of one color with less visible surface than this goes to its neighbours. Lower it if "
                                                         "small painted details (eyes, buttons) disappear."));
    m_smooth = add_check(ap, a3, _L("smooth borders"), _L("A point keeps a color only if most of the 5 layers around it have it there. Removes the "
                                                           "jagged, layer-to-layer flicker of borders painted along triangle edges."), true);
    as->Add(a3, 0, wxEXPAND | wxBOTTOM, em / 2);
    auto *a4 = new wxBoxSizer(wxHORIZONTAL);
    add_label(ap, a4, _L("Fewer tool changes (ΔE per change saved on every layer)"),
              _L("Every tool used on a layer costs a tool change (about 12 s on an INDX). This is how much color error (mean ΔE2000) "
                 "one tool change less on every layer is worth; recipes are then chosen and lined up to save changes. 0 = colors only."));
    m_change_weight = add_spin(ap, a4, 0., 5., 0.5, 0.1, _L("0 = best colors regardless of tool changes. Higher = fewer tool changes, slightly less exact colors."));
    as->Add(a4, 0, wxEXPAND | wxBOTTOM, em / 2);
    auto *a5 = new wxBoxSizer(wxHORIZONTAL);
    m_varlayer = add_check(ap, a5, _L("Variable layer thickness:"),
        _L("Repeat these layer heights (mm) above the first layer. A mix then lands on thick and thin layers differently, which gives "
           "more colors. Needs a calibration printed with the same heights."), false);
    m_schedule = new wxTextCtrl(ap, wxID_ANY, "0.12, 0.10, 0.08");
    m_schedule->SetToolTip(_L("Layer heights in mm, separated by commas, repeated from the second layer up."));
    m_schedule->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { invalidate(); update_patch_angle(); });
    a5->Add(m_schedule, 1, wxALIGN_CENTER_VERTICAL);
    as->Add(a5, 0, wxEXPAND | wxBOTTOM, em / 2);
    auto *a6 = new wxBoxSizer(wxHORIZONTAL);
    add_label(ap, a6, _L("Choosing mixes:"));
    m_keep_distinct = add_check(ap, a6, _L("keep different colors different"), _L("Avoid printing two different model colors with nearly the same mix."), true);
    m_families      = add_check(ap, a6, _L("match color families"), _L("When nothing is close, prefer the same color family (a brown for a brown)."), true);
    m_area          = add_check(ap, a6, _L("large areas count more"), _L("Colors covering large visible areas get the most exact mixes."), true);
    as->Add(a6, 0, wxEXPAND);
    ap->SetSizer(as);
    opt_box->Add(pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em / 2);
    pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent &) {
        Layout();
        Fit();
    });
    top->Add(opt_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    // ---- 3. Result
    auto     *res_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("3. Result"));
    wxWindow *rb      = res_box->GetStaticBox();
    auto     *prow    = new wxBoxSizer(wxHORIZONTAL);
    auto     *comp    = new wxButton(rb, wxID_ANY, _L("Preview"));
    prow->Add(comp, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_preview_note = new wxStaticText(rb, wxID_ANY, wxEmptyString);
    prow->Add(m_preview_note, 1, wxALIGN_CENTER_VERTICAL);
    res_box->Add(prow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em / 2);
    m_list = new wxListCtrl(rb, wxID_ANY, wxDefaultPosition, wxSize(-1, 20 * em), wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->SetToolTip(_L("Every color of the model: the mix it will print with, the color that mix is predicted to have, and how "
                          "far that is from the model's color (ΔE2000: below 2 is hard to see, above 5 is clearly different)."));
    m_swatch_size = wxSize(std::max(16, int(3.2 * em)), std::max(8, int(1.6 * em)));
    m_images      = new wxImageList(m_swatch_size.GetWidth(), m_swatch_size.GetHeight(), false);
    m_list->SetImageList(m_images, wxIMAGE_LIST_SMALL);
    m_list->AppendColumn(_L("Model color"), wxLIST_FORMAT_LEFT, 13 * em);
    m_list->AppendColumn(_L("Used by"), wxLIST_FORMAT_LEFT, 18 * em);
    m_list->AppendColumn(_L("Printed as"), wxLIST_FORMAT_LEFT, 26 * em);
    m_list->AppendColumn(_L("Predicted"), wxLIST_FORMAT_LEFT, 11 * em);
    m_list->AppendColumn(_L("Difference (ΔE)"), wxLIST_FORMAT_RIGHT, 10 * em);
    res_box->Add(m_list, 1, wxEXPAND | wxALL, em / 2);
    m_summary = new wxStaticText(rb, wxID_ANY, wxEmptyString);
    res_box->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em / 2);
    top->Add(res_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    // ---- Buttons
    auto *btns       = new wxBoxSizer(wxHORIZONTAL);
    auto *revert_btn = new wxButton(this, wxID_ANY, _L("Revert to original colors"));
    auto *folder     = new wxButton(this, wxID_ANY, _L("Filament sets folder"));
    m_apply          = new wxButton(this, wxID_ANY, _L("Apply to model"));
    auto *cancel     = new wxButton(this, wxID_CANCEL, _L("Close"));
    btns->Add(revert_btn, 0, wxRIGHT, em / 2);
    btns->Add(folder, 0, wxRIGHT, em / 2);
    btns->AddStretchSpacer();
    btns->Add(m_apply, 0, wxRIGHT, em / 2);
    btns->Add(cancel, 0);
    top->Add(btns, 0, wxEXPAND | wxALL, em);

    comp->SetToolTip(_L("Work out the mix for every color of the model and show how it will print. Nothing is changed yet."));
    m_apply->SetToolTip(_L("Repaint the model with the mixes shown and set up the printer's virtual extruders. Can be undone (Ctrl+Z) "
                           "or reverted later. Available after Preview, as long as the settings are those of the preview."));
    m_apply->Enable(false);
    folder->SetToolTip(_L("Open the folder with your own filament set files (*.json), e.g. to add other filaments. A template is put there."));
    revert_btn->SetToolTip(_L("Restore the painting the model was designed with and remove the generated mixes."));
    m_bundle_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { on_bundle_changed(); });
    m_cal_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { update_calibration_info(); invalidate(); });
    comp->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (compute())
            report_unused_spools();
    });
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { apply(); });
    revert_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { this->revert(); });
    folder->Bind(wxEVT_BUTTON, [](wxCommandEvent &) {
        boost::system::error_code ec;
        boost::filesystem::create_directories(ColorMixContext::user_bundle_dir(), ec);
        const std::string sys = (boost::filesystem::path(ColorMixContext::system_bundle_dir()) / "prusament-pla-colormix.json").string();
        const std::string tpl = (boost::filesystem::path(ColorMixContext::user_bundle_dir()) / "example-bundle.json.template").string();
        if (!boost::filesystem::exists(tpl, ec))
            boost::filesystem::copy_file(sys, tpl, ec);
        wxLaunchDefaultApplication(from_u8(ColorMixContext::user_bundle_dir()));
    });
    calib->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const Bundle *b = bundle();
        ColorMixCalibrationDialog dlg(this, m_plater, b ? b->id : std::string(), m_tools->tool_map());
        const int ret = dlg.ShowModal();
        if (ret == wxID_OK || dlg.model_replaced()) {
            // A calibration plate or slope test replaced the project: this dialog's state is stale.
            EndModal(wxID_OK);
            return;
        }
        // Filament sets may have new calibrations.
        reload_bundles(b ? b->id : std::string());
    });

    SetSizer(top);
}

const Bundle *ColorMixDialog::bundle() const
{
    const int i = m_bundle_choice->GetSelection();
    return i >= 0 && size_t(i) < m_ctx.bundles.size() ? &m_ctx.bundles[size_t(i)] : nullptr;
}

const Calibration *ColorMixDialog::calibration() const
{
    const Bundle *b = bundle();
    const int     i = m_cal_choice->GetSelection() - 1; // entry 0 = uncalibrated
    return b && i >= 0 && size_t(i) < b->calibrations.size() ? &b->calibrations[size_t(i)] : nullptr;
}

LayerSchedule ColorMixDialog::schedule() const
{
    LayerSchedule s;
    s.first_layer_height = m_ctx.first_layer_height;
    s.heights            = {m_ctx.layer_height};
    if (m_varlayer->GetValue()) {
        const std::vector<double> v = parse_layer_schedule(into_u8(m_schedule->GetValue()));
        if (!v.empty())
            s.heights = v;
    }
    return s;
}

SliceSettings ColorMixDialog::slice_settings() const
{
    SliceSettings s;
    s.core_extruder = m_core.tool();
    s.wall_depth    = m_core_depth->GetValue();
    s.top_pattern   = m_top->GetValue() && m_use_blends->GetValue();
    s.top_cell_size = m_top_cell->GetValue();
    s.top_layers    = m_top_layers->GetValue();
    if (m_varlayer->GetValue())
        s.layer_schedule = parse_layer_schedule(into_u8(m_schedule->GetValue()));
    s.dual_wall_width = m_dual_width->GetValue();
    s.patch_band_width = m_patch_band->GetValue();
    s.region_cleanup   = m_cleanup->GetValue();
    s.smooth_borders   = m_smooth->GetValue();
    s.min_run          = m_min_run->GetValue();
    s.min_island_area  = m_min_island->GetValue();
    return s;
}

void ColorMixDialog::update_patch_angle()
{
    if (m_patch_angle == nullptr || m_patch_band == nullptr || m_varlayer == nullptr || m_schedule == nullptr)
        return;
    const double B = m_patch_band->GetValue(), h = schedule().average();
    if (B <= 0.)
        m_patch_angle->SetLabel(_L("(flat tops only)"));
    else if (B <= h)
        m_patch_angle->SetLabel(_L("(every slope)"));
    else
        m_patch_angle->SetLabel(format_wxstr(_L("(flatter than %1%° at %2% mm layers)"), wxString::Format("%.0f", std::asin(h / B) * 180. / PI),
                                             wxString::Format("%.2f", h)));
    Layout();
}

void ColorMixDialog::load_settings_from_model()
{
    const ModelData &d   = m_plater->model().colormix;
    // A project without ColorMix settings (or holding a calibration plate) starts from the settings
    // of the last conversion in any project.
    const bool use_last = d.bundle_id.empty() || !d.plate_json.empty();
    m_last_settings.clear();
    if (use_last)
        m_last_settings = wxGetApp().app_config->get(LAST_SETTINGS_KEY);
    const nlohmann::json last = json_object(m_last_settings);
    std::string want = d.bundle_id.empty() ? std::string("prusament-pla-colormix") : d.bundle_id;
    if (use_last && last.contains("bundle_id") && last["bundle_id"].is_string())
        want = last["bundle_id"].get<std::string>();
    int sel = 0;
    for (size_t i = 0; i < m_ctx.bundles.size(); ++i)
        if (m_ctx.bundles[i].id == want)
            sel = int(i);
    m_bundle_choice->SetSelection(m_ctx.bundles.empty() ? wxNOT_FOUND : sel);
    if (use_last && !last.empty()) {
        auto num = [&last](const char *k, double def) { auto it = last.find(k); return it != last.end() && it->is_number() ? it->get<double>() : def; };
        auto flag = [&last](wxCheckBox *c, const char *k) {
            if (auto it = last.find(k); it != last.end() && it->is_boolean())
                c->SetValue(it->get<bool>());
        };
        m_core_depth->SetValue(num("wall_depth", m_core_depth->GetValue()));
        flag(m_top, "top_pattern");
        m_top_cell->SetValue(num("top_cell_size", m_top_cell->GetValue()));
        m_top_layers->SetValue(int(num("top_layers", m_top_layers->GetValue())));
        flag(m_varlayer, "variable_layers");
        if (auto it = last.find("layer_schedule"); it != last.end() && it->is_string() && !it->get<std::string>().empty())
            m_schedule->ChangeValue(from_u8(it->get<std::string>()));
        m_dual_width->SetValue(num("dual_wall_width", m_dual_width->GetValue()));
        m_patch_band->SetValue(num("patch_band_width", m_patch_band->GetValue()));
        flag(m_cleanup, "region_cleanup");
        flag(m_smooth, "smooth_borders");
        m_min_run->SetValue(num("min_run", m_min_run->GetValue()));
        m_min_island->SetValue(num("min_island_area", m_min_island->GetValue()));
        flag(m_use_blends, "layer_blends");
        flag(m_keep_distinct, "keep_distinct");
        flag(m_families, "match_families");
        flag(m_area, "area_weighting");
        flag(m_dual, "dual_walls");
        m_change_weight->SetValue(num("change_weight", m_change_weight->GetValue()));
    }
    if (!d.bundle_id.empty() && d.plate_json.empty()) {
        m_core.set_checked(d.slice.core_extruder > 0);
        m_core_depth->SetValue(d.slice.wall_depth);
        m_top->SetValue(d.slice.top_pattern);
        m_top_cell->SetValue(d.slice.top_cell_size);
        m_top_layers->SetValue(d.slice.top_layers);
        m_varlayer->SetValue(!d.slice.layer_schedule.empty());
        if (!d.slice.layer_schedule.empty())
            m_schedule->ChangeValue(from_u8(format_schedule(d.slice.layer_schedule)));
        m_dual_width->SetValue(d.slice.dual_wall_width);
        m_patch_band->SetValue(d.slice.patch_band_width);
        m_cleanup->SetValue(d.slice.region_cleanup);
        m_smooth->SetValue(d.slice.smooth_borders);
        m_min_run->SetValue(d.slice.min_run);
        m_min_island->SetValue(d.slice.min_island_area);
        m_dual->SetValue(std::any_of(m_plater->model().virtual_extruders.begin(), m_plater->model().virtual_extruders.end(),
                                     [](const FullSpectrum::VirtualExtruder &ve) { return !ve.inner_sequence.empty(); }));
        // Converter options of the last conversion (see apply()).
        const nlohmann::json o = json_object(d.options_json);
        auto set_check = [&o](wxCheckBox *c, const char *key) {
            if (auto it = o.find(key); it != o.end() && it->is_boolean())
                c->SetValue(it->get<bool>());
        };
        set_check(m_use_blends, "layer_blends");
        set_check(m_keep_distinct, "keep_distinct");
        set_check(m_families, "match_families");
        set_check(m_area, "area_weighting");
        set_check(m_dual, "dual_walls");
        if (auto it = o.find("change_weight"); it != o.end() && it->is_number())
            m_change_weight->SetValue(it->get<double>());
    }
    update_patch_angle();
}

void ColorMixDialog::on_bundle_changed()
{
    const Bundle *b = bundle();
    if (b == nullptr)
        return;
    const ModelData &d = m_plater->model().colormix;
    std::vector<unsigned int> tools = d.bundle_id == b->id && d.tool_of_filament.size() == b->filaments.size()
                                          ? d.tool_of_filament : m_ctx.default_tools(*b);
    m_tools->set_bundle(*b, tools, m_ctx.num_physical);
    m_cal_choice->Clear();
    m_cal_choice->Append(_L("None (uncalibrated)"));
    // A project converted with this bundle keeps its calibration (also "None"); otherwise the newest one.
    const bool converted = d.bundle_id == b->id && d.plate_json.empty();
    int        sel = -1, newest = -1;
    for (size_t i = 0; i < b->calibrations.size(); ++i) {
        const Calibration &c = b->calibrations[i];
        m_cal_choice->Append(from_u8(c.id + (c.date.empty() ? "" : " (" + c.date + ")")));
        if (converted && !d.calibration_id.empty() && c.id == d.calibration_id)
            sel = int(i) + 1;
        if (newest < 0 || c.date >= b->calibrations[size_t(newest)].date)
            newest = int(i);
    }
    if (converted && d.calibration_id.empty())
        sel = 0;
    const nlohmann::json last = json_object(m_last_settings);
    if (sel < 0 && last.value("bundle_id", std::string()) == b->id && last.contains("calibration_id") && last["calibration_id"].is_string()) {
        const std::string id = last["calibration_id"].get<std::string>();
        if (id.empty())
            sel = 0;
        for (size_t i = 0; i < b->calibrations.size(); ++i)
            if (b->calibrations[i].id == id)
                sel = int(i) + 1;
    }
    if (sel < 0)
        sel = newest + 1;
    m_cal_choice->SetSelection(sel);
    // Core: the previous choice for this bundle, else the bundle's White.
    const bool   from_last = !m_last_settings.empty() && last.value("bundle_id", std::string()) == b->id;
    unsigned int core = d.bundle_id == b->id && d.plate_json.empty() ? d.slice.core_extruder : from_last ? unsigned(std::max(0, last.value("core_tool", 0))) : 0;
    if (core == 0)
        core = white_tool(*b, tools);
    const bool had_settings = !d.bundle_id.empty() && d.plate_json.empty();
    m_core.refill(*b, tools, m_ctx.num_physical, core, !had_settings && !from_last);
    if (had_settings)
        m_core.set_checked(d.slice.core_extruder > 0);
    else if (from_last)
        m_core.set_checked(last.value("core_tool", 0) > 0);
    update_calibration_info();
    invalidate();
    Layout();
    Fit();
}

void ColorMixDialog::update_calibration_info()
{
    const Bundle      *b = bundle();
    const Calibration *c = calibration();
    if (b == nullptr) {
        m_cal_info->SetLabel(_L("No filament bundle found."));
        return;
    }
    if (c == nullptr) {
        m_cal_info->SetLabel(_L("Not calibrated: colors are predicted from the spools' catalog colors and can be 5-10 ΔE off. "
                                "For accurate colors print a calibration plate (Calibrate...)."));
        m_cal_info->SetForegroundColour(wxColour(200, 110, 0));
        m_cal_info->SetToolTip(wxEmptyString);
        m_cal_info->Wrap(90 * wxGetApp().em_unit());
        return;
    }
    LayerSchedule s;
    s.first_layer_height = c->first_layer_height;
    s.heights            = c->layer_schedule;
    // Default: dual walls on when this calibration measured dual-wall swatches (predictions are then reliable).
    if (m_plater->model().colormix.bundle_id.empty() && m_last_settings.empty() && m_dual != nullptr)
        m_dual->SetValue(std::any_of(c->swatches.begin(), c->swatches.end(), [](const Swatch &sw) { return !sw.inner_word.empty(); }));
    Predictor        p(*b, c, s);
    const FitReport &r = p.report();
    const int        measured = int(c->swatches.size());
    wxString msg = r.swatches_used >= 3 ?
        format_wxstr(_L("Calibrated with %1% measured swatches: predicted colors are typically within %2% ΔE of the print"), measured,
                     wxString::Format("%.1f", r.held_out > 0 ? r.held_out_median : r.loo_median)) :
        format_wxstr(_L("Calibrated with %1% measured swatches (too few mixes to correct the predictions)"), measured);
    msg += r.held_out > 0 ? format_wxstr(_L(" (checked on %1% validation swatches)."), r.held_out) : wxString(".");
    bool warn = false;
    if (!(s.heights == schedule().heights)) {
        msg += " " + _L("Warning: it was printed with other layer heights than the current settings.");
        warn = true;
    }
    m_cal_info->SetLabel(msg);
    m_cal_info->SetForegroundColour(warn ? wxColour(200, 110, 0) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    m_cal_info->SetToolTip(from_u8(r.summary()));
    m_cal_info->Wrap(90 * wxGetApp().em_unit());
}

std::string ColorMixDialog::settings_key() const
{
    std::ostringstream o;
    o << m_bundle_choice->GetSelection() << '|' << m_cal_choice->GetSelection() << '|';
    if (m_tools != nullptr)
        for (unsigned int t : m_tools->tool_map())
            o << t << ',';
    o << '|' << m_core.tool() << '|';
    for (const wxCheckBox *c : {m_use_blends, m_keep_distinct, m_families, m_area, m_top, m_varlayer, m_dual, m_cleanup, m_smooth})
        o << (c != nullptr && c->GetValue() ? '1' : '0');
    for (const wxSpinCtrlDouble *sp : {m_core_depth, m_top_cell, m_dual_width, m_patch_band, m_min_run, m_min_island, m_change_weight})
        o << '|' << (sp != nullptr ? sp->GetValue() : 0.);
    o << '|' << (m_top_layers != nullptr ? m_top_layers->GetValue() : 0) << '|' << (m_schedule != nullptr ? into_u8(m_schedule->GetValue()) : std::string());
    return o.str();
}

bool ColorMixDialog::preview_current() const { return m_result.has_value() && settings_key() == m_preview_key; }

void ColorMixDialog::invalidate()
{
    if (preview_current()) {
        // Back to the settings of the preview (or the same choice picked again): it is current.
        const wxColour fg = wxGetApp().get_label_clr_default();
        for (long i = 0; i < m_list->GetItemCount(); ++i)
            m_list->SetItemTextColour(i, fg);
        m_summary->SetForegroundColour(fg);
        m_summary->Refresh();
        m_preview_note->SetLabel(_L("How each color will print with these settings. Nothing changes until Apply."));
        m_preview_note->SetForegroundColour(fg);
        m_preview_note->Refresh();
        if (m_apply != nullptr)
            m_apply->Enable(true);
        update_patch_angle();
        return;
    }
    if (m_apply != nullptr)
        m_apply->Enable(false);
    if (m_list->GetItemCount() > 0) {
        // Keep the previous preview for reference, grayed out.
        const wxColour gray = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
        for (long i = 0; i < m_list->GetItemCount(); ++i)
            m_list->SetItemTextColour(i, gray);
        m_summary->SetForegroundColour(gray);
        m_summary->Refresh();
        m_preview_note->SetLabel(_L("Settings changed: the list below is the previous preview. Press Preview to update it."));
        m_preview_note->SetForegroundColour(wxColour(220, 100, 20));
    } else {
        m_preview_note->SetLabel(_L("Press Preview to see how each color will print. Nothing changes until Apply."));
        m_preview_note->SetForegroundColour(wxGetApp().get_label_clr_default());
    }
    m_preview_note->Refresh();
    update_patch_angle();
}

bool ColorMixDialog::compute(bool interactive)
{
    const Bundle *b = bundle();
    if (b == nullptr)
        return false;
    m_summary->SetForegroundColour(wxGetApp().get_label_clr_default());
    if (const wxString dup = duplicate_tools_message(*b, m_tools->tool_map()); !dup.empty()) {
        m_result.reset();
        m_list->DeleteAllItems();
        m_summary->SetLabel(dup);
        m_summary->Wrap(78 * wxGetApp().em_unit());
        Layout();
        return false;
    }
    // Painting touched up after the last conversion would be lost: the conversion starts from the
    // source painting. Offer to keep the touch-ups.
    {
        Model               &m = m_plater->model();
        const nlohmann::json o = json_object(m.colormix.options_json);
        // The last conversion's record, or (imported parts not converted yet) their tools.
        const unsigned int num_physical = unsigned(o.value("num_physical", int(m_ctx.num_physical)));
        if (const std::vector<unsigned int> applied = expected_states(m, num_physical); !applied.empty()) {
            if (const size_t n = count_repainted_volumes(m, applied, num_physical); n > 0 && !interactive) {
                // Opening the dialog: no questions yet, Preview asks.
                m_touch_up_note = format_wxstr(_L("The painting of %1% parts was changed after the last conversion: press Preview to "
                                                  "decide whether to keep those changes."), n);
            } else if (n > 0) {
                m_touch_up_note.clear();
                const int answer = MessageDialog(this,
                    format_wxstr(_L("The painting of %1% parts was changed after the last ColorMax conversion. A conversion starts "
                                    "from the source colors, so those changes would be lost.\n\nKeep the changes (they become part of "
                                    "the source colors)?"), n),
                    _L("ColorMax Engine"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal();
                if (answer == wxID_YES) {
                    m_plater->take_snapshot(_L("ColorMax: keep paint changes"));
                    m_model_changed = true;
                    const std::vector<unsigned int> now = fold_repainted_into_source(m, applied, num_physical,
                                                                                     current_extruder_colors(m, m_ctx.physical_colors),
                                                                                     o.contains("applied_states"));
                    m.colormix.unreadable_json.clear(); // replaced by this project's ColorMix data
                    // The record covers the kept colors too (without one, slot_tools does).
                    if (o.contains("applied_states")) {
                        nlohmann::json o2       = o;
                        o2["applied_states"]    = now;
                        m.colormix.options_json = o2.dump();
                    }
                }
            }
        }
    }
    wxBusyCursor wait;
    const LayerSchedule sched = schedule();
    m_predictor = std::make_unique<Predictor>(*b, calibration(), sched);
    const Model &model = m_plater->model();
    m_sources = collect_source_colors(model, current_extruder_colors(model, m_ctx.physical_colors));
    if (m_sources.empty()) {
        m_summary->SetLabel(_L("The plater has no colored objects to convert."));
        return false;
    }
    std::vector<double> areas;
    std::vector<Target> targets;
    for (const SourceColor &s : m_sources) {
        areas.push_back(s.area);
        targets.push_back({hex_to_lab(s.color), 1., s.label, s.z_ranges, s.top_z_ranges});
    }
    if (m_area->GetValue()) {
        const std::vector<double> w = area_weights(areas);
        for (size_t i = 0; i < targets.size(); ++i)
            targets[i].weight = w[i];
    }
    ConvertOptions opt;
    opt.use_blends     = m_use_blends->GetValue();
    opt.keep_distinct  = m_keep_distinct->GetValue();
    opt.match_families = m_families->GetValue();
    opt.dual_walls     = m_dual->GetValue();
    opt.schedule       = sched;
    // Blend ids follow the physical tools: leave room for printers with up to 16 tools, so a later
    // switch to more tools can't push them past the 255 painting states.
    opt.max_virtual    = int(TRIANGLE_STATE_TYPE_COUNT) - 1 - std::max(16, int(m_ctx.num_physical));
    opt.change_weight  = m_change_weight->GetValue();
    opt.time_limit_s   = 5.; // the dialog waits for it: keep it short
    opt.core_tool      = m_core.tool();
    opt.top_layers     = m_top->GetValue() && m_use_blends->GetValue() ? m_top_layers->GetValue() : 0;
    const std::vector<unsigned int> tools = m_tools->tool_map();
    m_result = convert_colors(targets, *b, *m_predictor, tools, opt);
    m_preview_key = settings_key();
    m_apply->Enable(m_result->problem.empty());
    m_preview_note->SetLabel(_L("How each color will print with these settings. Nothing changes until Apply."));
    m_summary->SetForegroundColour(wxGetApp().get_label_clr_default());
    m_preview_note->SetForegroundColour(wxGetApp().get_label_clr_default());
    m_preview_note->Refresh();

    m_list->DeleteAllItems();
    m_images->RemoveAll();
    if (!m_result->problem.empty()) {
        m_summary->SetLabel(from_u8(m_result->problem));
        m_result.reset();
        return false;
    }
    std::set<unsigned int> tools_used;
    for (size_t k = 0; k < m_sources.size(); ++k) {
        const SourceColor &s   = m_sources[k];
        const int          img = m_images->Add(swatch_bitmap(s.color, m_swatch_size.GetWidth(), m_swatch_size.GetHeight()));
        const long         row = m_list->InsertItem(long(k), from_u8(s.color), img);
        m_list->SetItem(row, 1, from_u8(s.label));
        const int ci = m_result->choice[k];
        if (ci < 0) {
            m_list->SetItem(row, 2, _L("(nothing loaded can print it)"));
            continue;
        }
        const Candidate &c = m_result->candidates[size_t(ci)];
        for (int f : c.recipe.word)
            if (size_t(f) < tools.size())
                tools_used.insert(tools[size_t(f)]);
        wxString name = from_u8(c.name);
        if (c.prediction.measured)
            name += "  " + _L("(measured)");
        m_list->SetItem(row, 2, name);
        const std::string pred_hex = lab_to_hex(c.prediction.color);
        const int         img2     = m_images->Add(swatch_bitmap(pred_hex, m_swatch_size.GetWidth(), m_swatch_size.GetHeight()));
        m_list->SetItem(row, 3, from_u8(pred_hex), img2);
        m_list->SetItem(row, 4, wxString::Format("%.1f", m_result->de[k]));
    }
    const SliceSettings ss = slice_settings();
    if (ss.core_extruder > 0)
        tools_used.insert(ss.core_extruder);
    // Tools on the busiest layer (from the heights where each color is painted), else all tools used.
    size_t per_layer = 0;
    for (const std::set<unsigned int> &u : estimate_tools_per_layer(targets, m_result->candidates, m_result->choice, tools, sched,
                                                                     opt.core_tool, opt.top_layers))
        per_layer = std::max(per_layer, u.size());
    if (per_layer == 0)
        for (size_t k = 0; k < m_sources.size(); ++k)
            if (const int ci = m_result->choice[k]; ci >= 0)
                for (int f : m_result->candidates[size_t(ci)].recipe.inner_word)
                    if (size_t(f) < tools.size())
                        tools_used.insert(tools[size_t(f)]);
    wxString summary = format_wxstr(_L("%1% source colors -> %2% layer blends. Weighted mean ΔE2000 %3%, worst %4%. "
                                       "Up to %5% tools per layer (each tool used on a layer costs one tool change)."),
                                    m_sources.size(), m_result->blends_used, wxString::Format("%.1f", m_result->weighted_mean_de),
                                    wxString::Format("%.1f", m_result->worst_de), per_layer > 0 ? per_layer : tools_used.size());
    // The calibration's swatches were printed with or without a core / with other walls.
    if (const Calibration *c = calibration(); c != nullptr) {
        const SliceSettings now = slice_settings();
        if (c->white_core != (now.core_extruder > 0) || (c->wall_depth > 0. && std::abs(c->wall_depth - now.wall_depth) > 0.05))
            summary += "\n" + _L("The calibration was printed with other white core / wall settings: its measured colors fit "
                                  "these settings less well.");
    }
    if (m_result->tool_changes >= 0) {
        summary += "\n" + format_wxstr(_L("Estimated tool changes: %1%"), m_result->tool_changes);
        if (m_result->tool_changes_before > m_result->tool_changes)
            summary += " " + format_wxstr(_L("(%1% without choosing recipes for fewer changes)"), m_result->tool_changes_before);
        summary += ".";
    }
    if (!m_result->optimal)
        summary += " " + _L("(search time limit reached; result may not be optimal)");
    if (!m_predictor->calibrated())
        summary += "\n" + _L("Predictions are uncalibrated.");
    if (!m_touch_up_note.empty())
        summary += "\n" + m_touch_up_note;
    if (m_beyond_tools > 0)
        summary += "\n" + format_wxstr(_L("%1% imported objects use colors beyond the printer's %2% tools: they print with their own "
                                           "extruder until converted."), m_beyond_tools, m_ctx.num_physical);
    if (!m_varlayer->GetValue() &&
        std::any_of(m_plater->model().objects.begin(), m_plater->model().objects.end(), [](const ModelObject *o) {
            return !o->layer_height_profile.empty() ||
                   std::any_of(o->layer_config_ranges.begin(), o->layer_config_ranges.end(), [](const auto &r) { return r.second.has("layer_height"); });
        }))
        summary += "\n" + _L("Some objects have variable layer height or height ranges: blends will print other colors than predicted. "
                              "Remove them, or use Variable layer thickness.");
    m_summary->SetLabel(summary);
    m_summary->Wrap(78 * wxGetApp().em_unit());
    Layout();
    return true;
}

void ColorMixDialog::report_unused_spools()
{
    const Bundle *b = bundle();
    if (b == nullptr || !m_result || !m_result->problem.empty())
        return;
    const std::vector<unsigned int> tools = m_tools->tool_map();
    std::set<int> used;
    for (const int ci : m_result->choice)
        if (ci >= 0 && size_t(ci) < m_result->candidates.size()) {
            const Recipe &r = m_result->candidates[size_t(ci)].recipe;
            used.insert(r.word.begin(), r.word.end());
            used.insert(r.inner_word.begin(), r.inner_word.end());
        }
    const unsigned int core = m_core.tool();
    wxString lines;
    for (size_t f = 0; f < b->filaments.size() && f < tools.size(); ++f) {
        if (tools[f] == 0 || used.count(int(f)) || tools[f] == core)
            continue; // not loaded, used by a color, or printing the white core
        lines += "\n    " + format_wxstr(_L("%1% (Tool %2%)"), from_u8(b->filaments[f].name), tools[f]);
    }
    if (lines.empty())
        return;
    MessageDialog(this,
                  _L("No color of this model uses these spools:") + lines + "\n\n" +
                      _L("They aren't needed for this print."),
                  _L("ColorMax Engine"), wxOK | wxICON_INFORMATION)
        .ShowModal();
}

void ColorMixDialog::apply()
{
    // Paint touch-ups noticed when the dialog opened haven't been decided on yet: ask now.
    if ((!preview_current() || !m_touch_up_note.empty()) && !compute(true))
        return;
    const Bundle *b = bundle();
    if (b == nullptr || !preview_current() || !m_predictor)
        return;
    const SliceSettings ss = slice_settings();
    if (m_varlayer->GetValue() && ss.layer_schedule.empty()) {
        MessageDialog(this, _L("The layer schedule is not valid. Enter layer heights in mm separated by commas, e.g. 0.12, 0.10, 0.08."),
                      _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    if (!schedule_within_limits(this, m_ctx, ss.layer_schedule))
        return;
    const std::vector<unsigned int> tools = m_tools->tool_map();
    m_plater->take_snapshot(_L("ColorMax conversion"));
    Model &model = m_plater->model();
    capture_source(model, current_extruder_colors(model, m_ctx.physical_colors));
    const ApplySummary summary = apply_conversion(model, m_sources, *m_result, *b, tools, m_ctx.num_physical);
    model.colormix.bundle_id        = b->id;
    model.colormix.calibration_id   = calibration() ? calibration()->id : std::string();
    model.colormix.tool_of_filament = tools;
    model.colormix.slice            = ss;
    model.colormix.plate_json.clear(); // the plater no longer holds just a calibration plate
    model.colormix.unreadable_json.clear(); // replaced by this conversion's data
    // Converter options, and the extruder colors from before the first conversion (for revert()).
    nlohmann::json o = json_object(model.colormix.options_json);
    if (auto it = o.find("original_extruder_colors"); it == o.end() || !it->is_array())
        o["original_extruder_colors"] = printer_extruder_colors();
    o["layer_blends"]   = m_use_blends->GetValue();
    o["keep_distinct"]  = m_keep_distinct->GetValue();
    o["match_families"] = m_families->GetValue();
    o["area_weighting"] = m_area->GetValue();
    o["dual_walls"]     = m_dual->GetValue();
    o["change_weight"]  = m_change_weight->GetValue();
    // How source slots were painted, to notice paint touch-ups before the next conversion.
    o["applied_states"] = summary.state_to_extruder;
    o["num_physical"]   = int(m_ctx.num_physical);
    model.colormix.options_json = o.dump();
    {
        // Defaults for the next new project.
        nlohmann::json last;
        last["bundle_id"]        = b->id;
        last["calibration_id"]   = model.colormix.calibration_id;
        last["core_tool"]        = int(ss.core_extruder);
        last["wall_depth"]       = ss.wall_depth;
        last["top_pattern"]      = m_top->GetValue();
        last["top_cell_size"]    = ss.top_cell_size;
        last["top_layers"]       = ss.top_layers;
        last["variable_layers"]  = m_varlayer->GetValue();
        last["layer_schedule"]   = into_u8(m_schedule->GetValue());
        last["dual_wall_width"]  = ss.dual_wall_width;
        last["patch_band_width"] = ss.patch_band_width;
        last["region_cleanup"]   = ss.region_cleanup;
        last["smooth_borders"]   = ss.smooth_borders;
        last["min_run"]          = ss.min_run;
        last["min_island_area"]  = ss.min_island_area;
        for (const char *k : {"layer_blends", "keep_distinct", "match_families", "area_weighting", "dual_walls", "change_weight"})
            last[k] = o[k];
        wxGetApp().app_config->set(LAST_SETTINGS_KEY, last.dump());
    }

    // Show the loaded spools in their real (measured if known) colors.
    std::vector<std::string> colors = m_ctx.physical_colors;
    for (size_t f = 0; f < tools.size() && f < b->filaments.size(); ++f)
        if (tools[f] >= 1 && tools[f] <= colors.size())
            colors[tools[f] - 1] = lab_to_hex(m_predictor->filament_colors()[f]);
    // An extra core spool shows as white.
    if (ss.core_extruder >= 1 && ss.core_extruder <= colors.size() && ss.core_extruder != white_tool(*b, tools))
        colors[ss.core_extruder - 1] = core_tool_color(*b, tools, ss.core_extruder);
    set_tool_colors(m_plater, colors);
    record_extruder_colors(model, nlohmann::json());
    if (summary.unprintable_colors > 0)
        MessageDialog(this,
                      format_wxstr(_L("%1% colors can't be printed with the loaded spools. They print with their part's own extruder "
                                      "(tool 1 where that was a layer blend)."), summary.unprintable_colors),
                      _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
    if (summary.unmapped_colors > 0)
        MessageDialog(this,
                      format_wxstr(_L("%1% colors could not be converted: the blends need more than the 255 painting states. "
                                      "They print with their part's own extruder."), summary.unmapped_colors),
                      _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
    EndModal(wxID_OK);
}

void ColorMixDialog::reload_bundles(const std::string &select_id)
{
    m_ctx = ColorMixContext::load(m_plater);
    m_bundle_choice->Clear();
    int sel = 0;
    for (size_t i = 0; i < m_ctx.bundles.size(); ++i) {
        m_bundle_choice->Append(bundle_label(m_ctx.bundles[i]));
        if (m_ctx.bundles[i].id == select_id)
            sel = int(i);
    }
    m_bundle_choice->SetSelection(m_ctx.bundles.empty() ? wxNOT_FOUND : sel);
    on_bundle_changed();
}

void ColorMixDialog::revert()
{
    Model &model = m_plater->model();
    bool   has   = false;
    for (const ModelObject *o : model.objects)
        for (const ModelVolume *v : o->volumes)
            has |= v->has_colormix_source();
    if (!has) {
        MessageDialog(this, _L("This model has not been converted, there is nothing to revert."), _L("ColorMax Engine"), wxOK).ShowModal();
        return;
    }
    m_plater->take_snapshot(_L("ColorMax revert"));
    revert_to_source(model, m_ctx.num_physical);
    model.colormix.unreadable_json.clear(); // replaced by this project's ColorMix data
    // The extruder colors apply() replaced.
    nlohmann::json o = json_object(model.colormix.options_json);
    if (auto it = o.find("original_extruder_colors"); it != o.end() && it->is_array()) {
        std::vector<std::string> colors;
        for (const nlohmann::json &c : *it)
            colors.push_back(c.is_string() ? c.get<std::string>() : std::string());
        o.erase("original_extruder_colors");
        o.erase("extruder_colors");
        o.erase("applied_states"); // the painting is the source again: nothing to compare with
        model.colormix.options_json = o.empty() ? std::string() : o.dump();
        set_tool_colors(m_plater, colors, true);
    }
    EndModal(wxID_OK);
}

void colormix_sync_extruder_colors_after_undo(Plater *plater, const std::string &options_before, const std::string &options_after)
{
    const nlohmann::json before = json_object(options_before), after = json_object(options_after);
    auto colors_of = [](const nlohmann::json &o, const char *key) {
        std::vector<std::string> colors;
        if (auto it = o.find(key); it != o.end() && it->is_array())
            for (const nlohmann::json &c : *it)
                colors.push_back(c.is_string() ? c.get<std::string>() : std::string());
        return colors;
    };
    // The jump went to a state whose ColorMix colors differ (a conversion, a calibration plate):
    // the printer gets that state's colors.
    if (const std::vector<std::string> now = colors_of(after, "extruder_colors"); !now.empty()) {
        if (now != colors_of(before, "extruder_colors"))
            set_tool_colors(plater, now, true);
    }
    // The jump went back to before ColorMix changed the colors: the printer gets its colors back.
    else if (const std::vector<std::string> original = colors_of(before, "original_extruder_colors");
             !original.empty() && colors_of(after, "original_extruder_colors").empty())
        set_tool_colors(plater, original, true);
}

// ---------------------------------------------------------------- calibration dialog

ColorMixCalibrationDialog::ColorMixCalibrationDialog(wxWindow *parent, Plater *plater, const std::string &bundle_id,
                                                     const std::vector<unsigned int> &tool_map)
    : DPIDialog(parent, wxID_ANY, _L("ColorMax Engine - calibrate a filament bundle"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_plater(plater)
{
    m_ctx        = ColorMixContext::load(plater);
    const int em = wxGetApp().em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY,
        _L("Calibration teaches ColorMax how your spools really mix on your printer: print a plate of color swatches, scan it, "
           "and save the measured colors. Predictions then match your prints to about 2 ΔE instead of 5-10."));
    intro->Wrap(95 * em);
    top->Add(intro, 0, wxEXPAND | wxALL, em);

    // ---- 1. Print the calibration plate
    auto     *s1 = new wxStaticBoxSizer(wxVERTICAL, this, _L("1. Print the calibration plate"));
    wxWindow *p1 = s1->GetStaticBox();
    auto     *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(p1, wxID_ANY, _L("Filament set:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_bundle_choice = new wxChoice(p1, wxID_ANY);
    int sel = 0;
    for (size_t i = 0; i < m_ctx.bundles.size(); ++i) {
        m_bundle_choice->Append(bundle_label(m_ctx.bundles[i]));
        if (m_ctx.bundles[i].id == bundle_id)
            sel = int(i);
    }
    m_bundle_choice->SetSelection(m_ctx.bundles.empty() ? wxNOT_FOUND : sel);
    m_bundle_choice->SetToolTip(_L("The filaments to calibrate. The calibration is saved into your own copy of this set."));
    row->Add(m_bundle_choice, 1, wxALIGN_CENTER_VERTICAL);
    s1->Add(row, 0, wxEXPAND | wxALL, em / 2);
    s1->Add(new wxStaticText(p1, wxID_ANY, _L("Where each spool is loaded - the same as for your prints:")), 0, wxLEFT | wxRIGHT, em / 2);
    m_tools = new ColorMixToolPanel(p1);
    if (const Bundle *b = bundle())
        m_tools->set_bundle(*b, tool_map.size() == b->filaments.size() ? tool_map : m_ctx.default_tools(*b), m_ctx.num_physical);
    m_tools->on_change = [this]() {
        update_core_default();
        rebuild_plan();
    };
    s1->Add(m_tools, 0, wxEXPAND | wxALL, em / 2);

    auto *r2 = new wxBoxSizer(wxHORIZONTAL);
    auto *lh = new wxStaticText(p1, wxID_ANY, _L("Layer height (mm):"));
    r2->Add(lh, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_schedule = new wxTextCtrl(p1, wxID_ANY, from_u8(format_schedule({m_ctx.layer_height})), wxDefaultPosition, wxSize(14 * em, -1));
    const wxString lh_tip = _L("The layer height you print with (from the print settings). For variable layer thickness enter the "
                               "repeating heights, e.g. 0.12, 0.10, 0.08. A calibration is only valid for the heights it was printed with.");
    lh->SetToolTip(lh_tip);
    m_schedule->SetToolTip(lh_tip);
    r2->Add(m_schedule, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3 * em);
    m_core.create(p1, r2, _L("White core:"),
                  _L("Print the inside of every swatch in one filament, like the white core of your prints. Use the same core tool "
                     "and filament you print with: the core shows through the walls and is part of the measured color."),
                  em);
    m_core.on_change = [this]() { m_core_touched = true; };
    s1->Add(r2, 0, wxEXPAND | wxALL, em / 2);
    update_core_default();

    auto *pane = new wxCollapsiblePane(p1, wxID_ANY, _L("Advanced plate settings"), wxDefaultPosition, wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    wxWindow *ap = pane->GetPane();
    auto     *a1 = new wxBoxSizer(wxHORIZONTAL);
    auto add_label = [&](const wxString &text, const wxString &tip) {
        auto *t = new wxStaticText(ap, wxID_ANY, text);
        t->SetToolTip(tip);
        a1->Add(t, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    };
    const wxString len_tip = _L("Longest layer pattern (in layers) measured directly. Longer patterns are predicted from the measured ones.");
    add_label(_L("Longest measured pattern (layers):"), len_tip);
    m_max_len = new wxSpinCtrl(ap, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(6 * em, -1), wxSP_ARROW_KEYS, 1, 6, 4);
    m_max_len->SetToolTip(len_tip);
    a1->Add(m_max_len, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 * em);
    const wxString ho_tip = _L("Extra swatches that are not used for fitting: they show how well colors that weren't measured are predicted.");
    add_label(_L("Validation swatches:"), ho_tip);
    m_held_out = new wxSpinCtrl(ap, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(6 * em, -1), wxSP_ARROW_KEYS, 0, 40, 10);
    m_held_out->SetToolTip(ho_tip);
    a1->Add(m_held_out, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 * em);
    const wxString dual_tip = _L("Swatches with a different inner wall, so dual-wall colors are predicted from measurements. Extra boards "
                                 "print side by side in the same layers: they add print time but almost no tool changes.");
    add_label(_L("Dual-wall swatches:"), dual_tip);
    m_dual = new wxChoice(ap, wxID_ANY);
    m_dual->Append(_L("None"));
    m_dual->Append(_L("Basic"));
    m_dual->Append(_L("Full (recommended)"));
    m_dual->SetSelection(2);
    m_dual->SetToolTip(dual_tip);
    a1->Add(m_dual, 0, wxALIGN_CENTER_VERTICAL);
    ap->SetSizer(a1);
    s1->Add(pane, 0, wxEXPAND | wxLEFT | wxRIGHT, em / 2);
    pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent &) {
        Layout();
        Fit();
    });

    auto *b1    = new wxBoxSizer(wxHORIZONTAL);
    auto *plate = new wxButton(p1, wxID_ANY, _L("Create calibration plate"));
    plate->SetToolTip(_L("Replaces the plater with the calibration boards and asks where to save the plate file. Slice and print "
                         "the plate with the same printer, profile and spools you print with."));
    b1->Add(plate, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_only_missing = new wxCheckBox(p1, wxID_ANY, _L("only the swatches still missing"));
    m_only_missing->SetToolTip(_L("Print a smaller plate with only the swatches that have no value yet - ones that misprinted, or were "
                                  "left out because a filament didn't print. The values you have are kept."));
    b1->Add(m_only_missing, 0, wxALIGN_CENTER_VERTICAL);
    b1->AddStretchSpacer();
    auto *slope = new wxButton(p1, wxID_ANY, _L("Slope test plate..."));
    slope->SetToolTip(_L("Optional: wedges from 40 to 5 degrees printed with layer mixing, to choose from which slope on surfaces get "
                         "patterned tops (Advanced settings of the conversion)."));
    slope->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { create_slope_test(); });
    b1->Add(slope, 0, wxALIGN_CENTER_VERTICAL);
    s1->Add(b1, 0, wxEXPAND | wxALL, em / 2);
    top->Add(s1, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

    // ---- 2. Scan the boards and read them
    auto     *s2 = new wxStaticBoxSizer(wxVERTICAL, this, _L("2. Scan the boards and read them"));
    wxWindow *p2 = s2->GetStaticBox();
    auto     *rpf = new wxBoxSizer(wxHORIZONTAL);
    auto *open_pf = new wxButton(p2, wxID_ANY, _L("Open plate file..."));
    open_pf->SetToolTip(_L("The file saved when the plate was created. It holds the plate's settings (they are locked while it is open) "
                           "and the measurements so far. The last one used opens automatically."));
    rpf->Add(open_pf, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_plate_file_label = new wxStaticText(p2, wxID_ANY, _L("No plate file open."));
    rpf->Add(m_plate_file_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_plate_file_close = new wxButton(p2, wxID_ANY, _L("Close plate file"));
    m_plate_file_close->SetToolTip(_L("Unlocks the settings, e.g. to make a new plate with other settings."));
    m_plate_file_close->Enable(false);
    rpf->Add(m_plate_file_close, 0, wxALIGN_CENTER_VERTICAL);
    s2->Add(rpf, 0, wxEXPAND | wxALL, em / 2);
    m_progress = new wxStaticText(p2, wxID_ANY, wxEmptyString);
    s2->Add(m_progress, 0, wxEXPAND | wxLEFT | wxRIGHT, em / 2);
    open_pf->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxFileDialog dlg(this, _L("Open the plate file"), from_u8(wxGetApp().app_config->get(LAST_PLATE_FILE_KEY)), wxEmptyString,
                         "ColorMax plate (*.json)|*.json", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK)
            open_plate_file(into_u8(dlg.GetPath()));
    });
    m_plate_file_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        set_plate_file(std::string());
        m_status->SetLabel(_L("Plate file closed: the settings can be changed (for a new plate)."));
    });

    auto *b2    = new wxBoxSizer(wxHORIZONTAL);
    auto *photo = new wxButton(p2, wxID_ANY, _L("Read a board (scan or photo)..."));
    photo->SetToolTip(_L("Scan a board in a hole cut into a sheet of white paper (recommended) or photograph it on white paper, open "
                         "the image and click the board's corners. Instructions are shown first."));
    photo->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { read_photo(); });
    b2->Add(photo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    auto *csv = new wxButton(p2, wxID_ANY, _L("Import values (CSV)..."));
    csv->SetToolTip(_L("For colorimeter measurements: a file with \"swatch number; L; a; b\" (or #RRGGBB) per line. Values can also be "
                       "typed or pasted into the table."));
    b2->Add(csv, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_legend_btn = new wxButton(p2, wxID_ANY, _L("Save plate file..."));
    m_legend_btn->SetToolTip(_L("Saves the current settings, the swatch list (board, row, column) and the measurements as a plate file. "
                                "Normally done when the plate is created."));
    m_legend_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { ask_save_plate_file(); });
    b2->Add(m_legend_btn, 0, wxALIGN_CENTER_VERTICAL);
    s2->Add(b2, 0, wxEXPAND | wxALL, em / 2);

    m_grid = new wxGrid(p2, wxID_ANY, wxDefaultPosition, wxSize(70 * em, 24 * em));
    m_grid->CreateGrid(0, 3);
    m_grid->SetColLabelValue(0, _L("Swatch: board / row / column, recipe"));
    m_grid->SetColLabelValue(1, _L("Expected"));
    m_grid->SetColLabelValue(2, _L("Measured (L a b or #RRGGBB)"));
    m_grid->SetColSize(0, 40 * em);
    m_grid->SetColSize(1, 9 * em);
    m_grid->SetColSize(2, 20 * em);
    m_grid->HideRowLabels();
    m_grid->SetToolTip(_L("Every swatch of the plan. \"Expected\" is the uncalibrated prediction; measured values come from the scans "
                          "(or type them in)."));
    s2->Add(m_grid, 1, wxEXPAND | wxALL, em / 2);
    top->Add(s2, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    // ---- 3. Save the calibration
    auto     *s3 = new wxStaticBoxSizer(wxHORIZONTAL, this, _L("3. Save the calibration"));
    wxWindow *p3 = s3->GetStaticBox();
    s3->Add(new wxStaticText(p3, wxID_ANY, _L("Name:")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, em / 2);
    char date[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
    m_cal_name = new wxTextCtrl(p3, wxID_ANY, from_u8(std::string("calibration ") + date));
    m_cal_name->SetToolTip(_L("Shown in the conversion dialog's calibration list. The name of an existing calibration (the default when "
                              "there is one for these layer heights) updates it: its other measurements are kept."));
    s3->Add(m_cal_name, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    auto *save = new wxButton(p3, wxID_ANY, _L("Save calibration"));
    save->SetToolTip(_L("Checks the measurements (e.g. for a filament that didn't print), fits the color model and saves it into your "
                        "copy of the filament set. It is used for all later conversions."));
    s3->Add(save, 0, wxALIGN_CENTER_VERTICAL | wxALL, em / 2);
    top->Add(s3, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    top->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
    auto *btns  = new wxBoxSizer(wxHORIZONTAL);
    auto *close = new wxButton(this, wxID_CANCEL, _L("Close"));
    btns->AddStretchSpacer();
    btns->Add(close, 0);
    top->Add(btns, 0, wxEXPAND | wxALL, em);

    m_bundle_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        if (const Bundle *b = bundle())
            m_tools->set_bundle(*b, m_ctx.default_tools(*b), m_ctx.num_physical);
        update_core_default();
        rebuild_plan();
        update_default_name();
    });
    m_schedule->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { rebuild_plan(); update_default_name(); });
    m_cal_name->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { m_name_from_plate = true; }); // typed by the user: keep it
    m_max_len->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { rebuild_plan(); });
    m_held_out->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { rebuild_plan(); });
    m_dual->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { rebuild_plan(); });
    plate->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { create_plate(); });
    csv->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { load_csv(); });
    save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { save_calibration(); });
    m_grid->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent &e) {
        store_measurements();
        e.Skip();
    });

    SetSizer(top);
    // The plate on the plater (its plate file, else its settings); otherwise the last plate file used,
    // so measurements already made are there without loading anything.
    update_default_name();
    wxString restored = restore_plate_settings();
    if (restored.empty() && m_plate_file.empty()) {
        // The plate file of this filament set's newest calibration, else the last plate file used.
        std::vector<std::string> candidates;
        if (const Bundle *b = bundle()) {
            std::vector<const Calibration *> cals;
            for (const Calibration &c : b->calibrations)
                cals.push_back(&c);
            std::sort(cals.begin(), cals.end(), [](const Calibration *x, const Calibration *y) { return x->date > y->date; });
            for (const Calibration *c : cals)
                if (!c->plate_file.empty())
                    candidates.push_back(c->plate_file);
        }
        candidates.push_back(wxGetApp().app_config->get(LAST_PLATE_FILE_KEY));
        for (const std::string &pf : candidates) {
            if (pf.empty() || !boost::filesystem::exists(pf))
                continue;
            // Only a plate file of the selected filament set.
            boost::nowide::ifstream in(pf, std::ios::binary);
            const nlohmann::json    jf = json_object(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            if (!jf.contains("settings") || !bundle() || jf["settings"].value("bundle_id", std::string()) != bundle()->id)
                continue;
            if (open_plate_file(pf, true)) {
                restored = format_wxstr(_L("Opened the plate file: %1%"), from_u8(pf));
                break;
            }
        }
    }
    rebuild_plan();
    if (!restored.empty())
        m_status->SetLabel(restored);
    Fit();
    CenterOnParent();
}

void ColorMixCalibrationDialog::on_dpi_changed(const wxRect &) { Layout(); Refresh(); }

const Bundle *ColorMixCalibrationDialog::bundle() const
{
    const int i = m_bundle_choice->GetSelection();
    return i >= 0 && size_t(i) < m_ctx.bundles.size() ? &m_ctx.bundles[size_t(i)] : nullptr;
}

LayerSchedule ColorMixCalibrationDialog::schedule() const
{
    LayerSchedule s;
    s.first_layer_height       = m_ctx.first_layer_height;
    const std::vector<double> v = parse_layer_schedule(into_u8(m_schedule->GetValue()));
    s.heights                  = v.empty() ? std::vector<double>{m_ctx.layer_height} : v;
    return s;
}

void ColorMixCalibrationDialog::update_core_default()
{
    // Until the user picks one: the core tool of the current project (same bundle), else the
    // bundle's White. Only the bundle's White or a tool with no bundle filament is offered.
    const Bundle *b = bundle();
    if (b == nullptr)
        return;
    const std::vector<unsigned int> tools = m_tools->tool_map();
    if (m_core_touched) {
        m_core.refill(*b, tools, m_ctx.num_physical, 0, false);
        return;
    }
    unsigned int     core = 0;
    const ModelData &d    = m_plater->model().colormix;
    if (d.bundle_id == b->id)
        core = d.slice.core_extruder;
    if (core == 0)
        core = white_tool(*b, tools);
    m_core.refill(*b, tools, m_ctx.num_physical, core, true);
}

bool ColorMixCalibrationDialog::checked_schedule(std::vector<double> &out)
{
    out.clear();
    const std::string text = boost::algorithm::trim_copy(into_u8(m_schedule->GetValue()));
    std::vector<double> v = text.empty() ? std::vector<double>() : parse_layer_schedule(text);
    if (!text.empty() && v.empty()) {
        MessageDialog(this, _L("The layer heights are not valid. Enter layer heights in mm separated by commas, e.g. 0.12, 0.10, 0.08."),
                      _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
        return false;
    }
    // Always explicit (no field: the profile's layer height): the plate prints with the heights it
    // records, even when the profile changes before it is printed.
    if (v.empty())
        v = {m_ctx.layer_height};
    if (!schedule_within_limits(this, m_ctx, v))
        return false;
    out = v;
    return true;
}

std::string ColorMixCalibrationDialog::plate_settings_json(const std::string &kind, const std::vector<unsigned int> &tools) const
{
    nlohmann::json j;
    j["kind"]           = kind;
    j["bundle_id"]      = bundle() ? bundle()->id : std::string();
    j["layer_schedule"] = into_u8(m_schedule->GetValue());
    j["max_word_length"] = m_max_len->GetValue();
    j["held_out"]       = m_held_out->GetValue();
    j["dual_level"]     = std::max(0, m_dual->GetSelection());
    j["core_tool"]      = m_core.tool();
    j["tool_map"]       = tools;
    j["plan_id"]        = calibration_plan_id(m_plan);
    j["windows"]        = false; // plates without the "windows" key are old ones (not supported)
    if (!m_plate_rows.empty())
        j["rows"] = m_plate_rows;
    char date[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
    j["created"] = date;
    // How the swatches are printed: what a calibration of this plate must record.
    const LayerSchedule sched = schedule();
    j["first_layer_height"] = sched.first_layer_height;
    j["schedule_heights"]   = sched.heights;
    j["wall_depth"]         = SliceSettings().wall_depth;
    j["dual_wall_width"]    = SliceSettings().dual_wall_width;
    return j.dump();
}

bool ColorMixCalibrationDialog::apply_plate_settings(const std::string &json)
{
    const Bundle        *b = bundle();
    const nlohmann::json j = json_object(json);
    if (b == nullptr || j.empty())
        return false;
    auto it_bundle = j.find("bundle_id");
    if (it_bundle == j.end() || !it_bundle->is_string() || it_bundle->get<std::string>() != b->id)
        return false;
    // Plates of early development versions (with windows, no "windows" key) can't be read.
    if (j.value("windows", true))
        return false;
    try {
        if (auto it = j.find("tool_map"); it != j.end() && it->is_array()) {
            const std::vector<unsigned int> tools = it->get<std::vector<unsigned int>>();
            if (tools.size() == b->filaments.size())
                m_tools->set_bundle(*b, tools, m_ctx.num_physical);
        }
        if (auto it = j.find("layer_schedule"); it != j.end() && it->is_string())
            m_schedule->ChangeValue(from_u8(it->get<std::string>()));
        if (auto it = j.find("max_word_length"); it != j.end() && it->is_number_integer())
            m_max_len->SetValue(it->get<int>());
        if (auto it = j.find("held_out"); it != j.end() && it->is_number_integer())
            m_held_out->SetValue(it->get<int>());
        if (auto it = j.find("dual_level"); it != j.end() && it->is_number_integer())
            if (const int level = it->get<int>(); level >= 0 && level < int(m_dual->GetCount()))
                m_dual->SetSelection(level);
        if (auto it = j.find("core_tool"); it != j.end() && it->is_number_integer()) {
            m_core.refill(*b, m_tools->tool_map(), m_ctx.num_physical, unsigned(std::max(0, it->get<int>())), true);
            m_core_touched = true;
        }
        m_plate_rows.clear();
        if (auto it = j.find("rows"); it != j.end() && it->is_array())
            m_plate_rows = it->get<std::vector<int>>();
        m_pending_measured.clear();
        if (auto it = j.find("measured"); it != j.end() && it->is_object())
            for (auto m = it->begin(); m != it->end(); ++m)
                if (m.value().is_string())
                    m_pending_measured[std::atoi(m.key().c_str())] = m.value().get<std::string>();
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

wxString ColorMixCalibrationDialog::restore_plate_settings()
{
    const ModelData &d = m_plater->model().colormix;
    if (bundle() == nullptr || d.plate_json.empty())
        return wxString();
    const nlohmann::json j = json_object(d.plate_json);
    // The plate's own file, when it was saved: the settings and the measurements entered so far.
    if (const std::string pf = j.value("plate_file", std::string()); !pf.empty() && boost::filesystem::exists(pf)) {
        if (open_plate_file(pf))
            return format_wxstr(_L("The plate file of the plate on the plater was opened (settings locked): %1%"), from_u8(pf));
    }
    if (!apply_plate_settings(d.plate_json))
        return wxString();
    if (j.value("kind", std::string()) == "slope_test")
        return _L("Settings restored from the slope test on the plater.");
    return _L("Settings restored from the calibration plate on the plater. To read its photos, open its plate file.");
}

namespace {
// Writes `text` to `path` through a temporary file, so a failed write never leaves half a file.
bool write_text_file_safely(const std::string &path, const std::string &text)
{
    const std::string tmp = path + ".tmp";
    {
        boost::nowide::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << text;
        if (!out)
            return false;
    }
    boost::system::error_code ec;
    boost::filesystem::rename(tmp, path, ec);
    return !ec;
}
} // namespace

bool ColorMixCalibrationDialog::write_plate_file(const std::string &path)
{
    const Bundle *b = bundle();
    if (b == nullptr)
        return false;
    const nlohmann::json pj = json_object(m_plater->model().colormix.plate_json);
    nlohmann::json settings = json_object(plate_settings_json("plate", m_tools->tool_map()));
    // The plate on the plater made with these settings keeps its creation date.
    if (pj.value("plan_id", std::string()) == settings.value("plan_id", std::string()) && pj.contains("created"))
        settings["created"] = pj["created"];
    nlohmann::json j;
    j["format"]   = "colormax-calibration-plate";
    j["version"]  = 1;
    j["about"]    = "ColorMax Engine calibration plate. Keep this file: Read a board (scan or photo) opens it to read the boards with the settings "
                    "the plate was made with, and the measurements are saved into it as they are read.";
    j["settings"] = settings;
    // The calibration these measurements belong to (saved again under this name, it is updated).
    j["calibration_name"] = into_u8(m_cal_name->GetValue());
    nlohmann::json list = nlohmann::json::array();
    for (size_t i = 0; i < m_plan.size(); ++i) {
        nlohmann::json e;
        e["swatch"] = int(i + 1);
        if (const int pp = plate_position(i); pp >= 0) {
            const auto pos = swatch_position(pp, m_layout);
            e["board"]     = pos[0] + 1;
            e["row"]       = pos[1] + 1;
            e["column"]    = pos[2] + 1;
        } else
            e["board"] = nullptr; // not on this plate
        e["recipe"] = m_plan[i].label;
        std::vector<std::string> w, in;
        for (int f : m_plan[i].word)
            w.push_back(b->filaments[size_t(f)].id);
        for (int f : m_plan[i].inner_word)
            in.push_back(b->filaments[size_t(f)].id);
        e["word"] = w;
        if (!in.empty())
            e["inner_word"] = in;
        if (m_plan[i].held_out)
            e["validation"] = true;
        list.push_back(e);
    }
    j["swatches"] = list;
    nlohmann::json measured = nlohmann::json::object();
    for (int r = 0; r < m_grid->GetNumberRows(); ++r)
        if (const std::string v = boost::algorithm::trim_copy(into_u8(m_grid->GetCellValue(r, 2))); !v.empty())
            measured[std::to_string(r)] = v;
    j["measured"] = measured;
    return write_text_file_safely(path, j.dump(1));
}

bool ColorMixCalibrationDialog::ask_save_plate_file()
{
    if (m_plan.empty())
        return false;
    const std::string folder = wxGetApp().app_config->get(LAST_PLATE_FILE_KEY);
    char date[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
    const std::string name = "ColorMax plate " + calibration_plan_id(m_plan) + " " + date + ".json";
    wxFileDialog dlg(this, _L("Save the plate file (keep it: it is needed to read the photos of this plate)"),
                     folder.empty() ? wxString() : from_u8(boost::filesystem::path(folder).parent_path().string()), from_u8(name),
                     "ColorMax plate (*.json)|*.json", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return false;
    const std::string path = into_u8(dlg.GetPath());
    if (!write_plate_file(path)) {
        MessageDialog(this, _L("The plate file could not be written."), _L("ColorMax Engine"), wxOK | wxICON_ERROR).ShowModal();
        return false;
    }
    set_plate_file(path);
    // The plate on the plater remembers its file, so reopening the project opens it again.
    ModelData &d = m_plater->model().colormix;
    if (nlohmann::json pj = json_object(d.plate_json); !pj.empty() && pj.value("plan_id", std::string()) == calibration_plan_id(m_plan)) {
        pj["plate_file"] = path;
        d.plate_json     = pj.dump();
    }
    m_status->SetLabel(format_wxstr(_L("Plate file saved: %1%"), from_u8(path)));
    return true;
}

bool ColorMixCalibrationDialog::open_plate_file(const std::string &path, bool quiet)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    const std::string       text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const nlohmann::json    j = json_object(text);
    auto fail = [&](const wxString &msg) {
        if (!quiet)
            MessageDialog(this, msg, _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
        return false;
    };
    if (j.value("format", std::string()) != "colormax-calibration-plate" || !j.contains("settings") || !j["settings"].is_object())
        return fail(_L("This is not a ColorMax plate file."));
    nlohmann::json settings = j["settings"];
    if (settings.value("windows", true))
        return fail(_L("This plate file is from an early development version (boards with windows) and can't be read: create a new plate."));
    const std::string bundle_id = settings.value("bundle_id", std::string());
    int sel = -1;
    for (size_t i = 0; i < m_ctx.bundles.size(); ++i)
        if (m_ctx.bundles[i].id == bundle_id)
            sel = int(i);
    if (sel < 0)
        return fail(format_wxstr(_L("The plate file is for the filament bundle \"%1%\", which isn't installed."), from_u8(bundle_id)));
    set_plate_file(std::string()); // unlocked while the settings are applied
    m_bundle_choice->SetSelection(sel);
    m_tools->set_bundle(m_ctx.bundles[size_t(sel)], m_ctx.default_tools(m_ctx.bundles[size_t(sel)]), m_ctx.num_physical);
    if (j.contains("measured"))
        settings["measured"] = j["measured"];
    m_fixed_plan.clear();
    if (!apply_plate_settings(settings.dump()))
        return fail(_L("The plate file could not be read."));
    // The swatches as printed, from the file: they hold even if this ColorMax plans differently today.
    if (auto it = j.find("swatches"); it != j.end() && it->is_array()) {
        const Bundle            &fb = m_ctx.bundles[size_t(sel)];
        std::vector<SwatchSpec>  plan;
        bool                     ok = true;
        auto ids = [&](const nlohmann::json &e, const char *key, Word &out) {
            if (auto w = e.find(key); w != e.end() && w->is_array())
                for (const nlohmann::json &id : *w) {
                    const int f = id.is_string() ? fb.filament_index(id.get<std::string>()) : -1;
                    ok = ok && f >= 0;
                    out.push_back(f);
                }
        };
        for (const nlohmann::json &e : *it) {
            SwatchSpec sw;
            ids(e, "word", sw.word);
            ids(e, "inner_word", sw.inner_word);
            sw.held_out = e.value("validation", false);
            sw.label    = e.value("recipe", std::string());
            ok          = ok && !sw.word.empty();
            plan.push_back(std::move(sw));
        }
        if (ok && !plan.empty() && calibration_plan_id(plan) == settings.value("plan_id", std::string())) {
            m_fixed_plan = std::move(plan);
            m_fixed_key  = plan_inputs_key();
        }
    }
    // Values of another plan must not carry over.
    for (int r = 0; r < m_grid->GetNumberRows(); ++r)
        m_grid->SetCellValue(r, 2, wxEmptyString);
    rebuild_plan();
    const std::string want = settings.value("plan_id", std::string()), got = calibration_plan_id(m_plan);
    if (!want.empty() && want != got)
        return fail(format_wxstr(_L("The swatch list of this plate (plan %1%) can't be rebuilt here (plan %2%): the filament bundle "
                                    "or ColorMax changed since the plate was made. The photos can't be read reliably."),
                                 from_u8(want), from_u8(got)));
    set_plate_file(path);
    if (const std::string name = j.value("calibration_name", std::string()); !name.empty()) {
        m_cal_name->ChangeValue(from_u8(name));
        m_name_from_plate = true;
    } else {
        m_name_from_plate = false;
        update_default_name();
    }
    int n = 0;
    for (int r = 0; r < m_grid->GetNumberRows(); ++r)
        n += m_grid->GetCellValue(r, 2).empty() ? 0 : 1;
    m_status->SetLabel(format_wxstr(_L("Plate file opened: plan %1%, %2% swatches on %3% boards, %4% measured so far. The settings are locked to "
                                       "the plate."),
                                    from_u8(got), plate_swatch_count(), board_count(plate_swatch_count(), m_layout), n));
    return true;
}

void ColorMixCalibrationDialog::set_plate_file(const std::string &path)
{
    m_plate_file = path;
    lock_settings(!path.empty());
    if (!path.empty())
        wxGetApp().app_config->set(LAST_PLATE_FILE_KEY, path);
    m_plate_file_label->SetLabel(path.empty() ? _L("No plate file open.") :
                                                format_wxstr(_L("Plate file: %1% (settings locked)"), from_u8(path)));
    m_plate_file_close->Enable(!path.empty());
    Layout();
}

void ColorMixCalibrationDialog::lock_settings(bool lock)
{
    for (wxWindow *w : std::initializer_list<wxWindow *>{m_bundle_choice, m_tools, m_schedule, m_max_len, m_held_out, m_dual})
        if (w != nullptr)
            w->Enable(!lock);
    m_core.enable(!lock);
}

void ColorMixCalibrationDialog::after_model_replaced()
{
    m_model_replaced = true;
    // Object list, extruder column and 3D scene, like after a conversion.
    m_plater->sidebar().update_after_colormix_change();
}

Vec2d ColorMixCalibrationDialog::bed_center() const
{
    // Middle of the printer's bed (bounding box of bed_shape).
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->full_config();
    if (const auto *bs = cfg.option<ConfigOptionPoints>("bed_shape"); bs != nullptr && !bs->values.empty()) {
        BoundingBoxf bb;
        for (const Vec2d &p : bs->values)
            bb.merge(p);
        return bb.center();
    }
    return Vec2d(125., 105.);
}

double ColorMixCalibrationDialog::bed_depth() const
{
    // Depth of the printer's bed (Y extent of bed_shape).
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->full_config();
    if (const auto *bs = cfg.option<ConfigOptionPoints>("bed_shape"); bs != nullptr && !bs->values.empty()) {
        double ymin = 1e9, ymax = -1e9;
        for (const Vec2d &p : bs->values) {
            ymin = std::min(ymin, p.y());
            ymax = std::max(ymax, p.y());
        }
        if (ymax > ymin)
            return ymax - ymin;
    }
    return 210.;
}

void ColorMixCalibrationDialog::rebuild_plan()
{
    const Bundle *b = bundle();
    if (b == nullptr)
        return;
    // Keep what was typed for tiles that stay the same.
    std::map<std::string, wxString> typed;
    for (int r = 0; r < m_grid->GetNumberRows(); ++r)
        if (size_t(r) < m_plan.size())
            typed[swatch_key(m_plan[size_t(r)].label)] = m_grid->GetCellValue(r, 2);
    const LayerSchedule sched = schedule();
    if (!m_fixed_plan.empty() && plan_inputs_key() == m_fixed_key) {
        m_plan        = m_fixed_plan; // the plate file's own swatches
        m_plan_report = PlanReport();
    } else {
        m_fixed_plan.clear();
        m_plan = plan_calibration_swatches(*b, m_tools->tool_map(), sched, m_max_len->GetValue(), m_held_out->GetValue(),
                                           std::max(0, m_dual->GetSelection()), m_layout.swatches_per_board(),
                                           int(TRIANGLE_STATE_TYPE_COUNT) - 1 - int(m_ctx.num_physical), &m_plan_report);
    }
    if (m_grid->GetNumberRows() > 0)
        m_grid->DeleteRows(0, m_grid->GetNumberRows());
    m_grid->AppendRows(int(m_plan.size()));
    Predictor p(*b, nullptr, sched);
    for (size_t i = 0; i < m_plan.size(); ++i) {
        const int r = int(i);
        const int pp = plate_position(i);
        if (pp >= 0) {
            const auto pos = swatch_position(pp, m_layout);
            m_grid->SetCellValue(r, 0, wxString::Format("B%d R%d C%-2d  ", pos[0] + 1, pos[1] + 1, pos[2] + 1) + from_u8(m_plan[i].label));
        } else
            m_grid->SetCellValue(r, 0, _L("(not on the plate)") + "  " + from_u8(m_plan[i].label));
        m_grid->SetReadOnly(r, 0);
        Recipe rec;
        rec.word    = m_plan[i].word;
        rec.weights = word_weights(m_plan[i].word, b->filaments.size(), sched);
        if (!m_plan[i].inner_word.empty()) {
            rec.inner_word    = m_plan[i].inner_word;
            rec.inner_weights = word_weights(m_plan[i].inner_word, b->filaments.size(), sched);
        }
        const std::string hex = lab_to_hex(p.predict(rec).color);
        m_grid->SetCellValue(r, 1, from_u8(hex));
        wxColour c;
        c.Set(from_u8(hex));
        m_grid->SetCellBackgroundColour(r, 1, c);
        m_grid->SetCellTextColour(r, 1, c.GetLuminance() > 0.5 ? *wxBLACK : *wxWHITE);
        m_grid->SetReadOnly(r, 1);
        if (auto it = typed.find(swatch_key(m_plan[i].label)); it != typed.end())
            m_grid->SetCellValue(r, 2, it->second);
        if (auto it = m_pending_measured.find(int(i)); it != m_pending_measured.end() && m_grid->GetCellValue(r, 2).empty())
            m_grid->SetCellValue(r, 2, from_u8(it->second));
    }
    m_pending_measured.clear();
    const int boards = board_count(plate_swatch_count(), m_layout);
    wxString  msg    = format_wxstr(_L("%1% swatches (%2% for validation, %3% dual-wall) on %4% boards."), m_plan.size(),
                                    std::count_if(m_plan.begin(), m_plan.end(), [](const SwatchSpec &s) { return s.held_out; }),
                                    std::count_if(m_plan.begin(), m_plan.end(), [](const SwatchSpec &s) { return !s.inner_word.empty(); }), boards);
    if (15. + boards * m_layout.pitch() > bed_depth())
        msg += " " + _L("They do not fit on the bed in one go: lower the longest word or the number of validation swatches.");
    if (const std::string text = boost::algorithm::trim_copy(into_u8(m_schedule->GetValue()));
        !text.empty() && parse_layer_schedule(text).empty())
        msg += "\n" + _L("The layer heights are not valid: the list uses the profile's layer height.");
    if (const wxString dup = duplicate_tools_message(*b, m_tools->tool_map()); !dup.empty())
        msg += "\n" + dup;
    if (m_plan_report.dropped_core > 0)
        msg += "\n" + plan_cap_message();
    else if (m_plan_report.dropped_dual > 0)
        msg += "\n" + format_wxstr(_L("%1% dual-wall swatches were left out: PrusaSlicer allows %2% mixed colors on a plate."),
                                    m_plan_report.dropped_dual, m_plan_report.budget);
    m_status->SetLabel(msg);
    m_status->SetForegroundColour(m_plan_report.dropped_core > 0 ? wxColour(200, 0, 0) : wxGetApp().get_label_clr_default());
    update_progress();
}

std::string ColorMixCalibrationDialog::plan_inputs_key() const
{
    std::ostringstream o;
    const Bundle *b = bundle();
    o << (b ? b->id : std::string()) << '|' << into_u8(m_schedule->GetValue()) << '|' << m_max_len->GetValue() << '|' << m_held_out->GetValue()
      << '|' << m_dual->GetSelection() << '|';
    for (unsigned int t : m_tools->tool_map())
        o << t << ',';
    return o.str();
}

wxString ColorMixCalibrationDialog::plan_cap_message() const
{
    return format_wxstr(_L("This filament set can't be calibrated on one plate: PrusaSlicer allows %1% mixed colors on a plate "
                           "(255 painting colors minus the printer's tools), and its basic swatches need %2%. Set one or more filaments "
                           "to \"Accent\" or \"Solid only\" (Edit... next to the filament set), or set some spools to \"Not loaded\", "
                           "and try again."),
                        m_plan_report.budget, m_plan_report.needed - m_plan_report.dropped_validation - m_plan_report.dropped_dual);
}

const Calibration *ColorMixCalibrationDialog::matching_calibration() const
{
    const Bundle *b = bundle();
    if (b == nullptr)
        return nullptr;
    const LayerSchedule sched = schedule();
    const Calibration  *best  = nullptr;
    for (const Calibration &c : b->calibrations) {
        if (c.layer_schedule.size() != sched.heights.size())
            continue;
        bool same = true;
        for (size_t i = 0; i < sched.heights.size(); ++i)
            same = same && std::abs(c.layer_schedule[i] - sched.heights[i]) < 1e-6;
        if (same && (best == nullptr || c.date >= best->date))
            best = &c;
    }
    return best;
}

void ColorMixCalibrationDialog::update_default_name()
{
    // A name from the plate file wins; otherwise continue the newest calibration with these layer heights.
    if (m_name_from_plate || m_cal_name == nullptr)
        return;
    if (const Calibration *c = matching_calibration())
        m_cal_name->ChangeValue(from_u8(c->id));
    else {
        char date[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
        m_cal_name->ChangeValue(from_u8(std::string("calibration ") + date));
    }
}

void ColorMixCalibrationDialog::update_progress()
{
    if (m_progress == nullptr || m_plan.empty())
        return;
    const int per = m_layout.swatches_per_board(), on_plate = plate_swatch_count(), boards = board_count(on_plate, m_layout);
    int       total = 0;
    std::vector<int> done(size_t(std::max(1, boards)), 0), size(size_t(std::max(1, boards)), 0);
    for (int pos = 0; pos < on_plate; ++pos) {
        const int r = m_plate_rows.empty() ? pos : m_plate_rows[size_t(pos)];
        ++size[size_t(pos / per)];
        if (r < m_grid->GetNumberRows() && !m_grid->GetCellValue(r, 2).empty())
            ++done[size_t(pos / per)];
    }
    for (int r = 0; r < m_grid->GetNumberRows(); ++r)
        total += m_grid->GetCellValue(r, 2).empty() ? 0 : 1;
    wxString boards_txt;
    for (int k = 0; k < boards; ++k)
        boards_txt += (k ? ",  " : "") + format_wxstr(_L("board %1%: %2%/%3%"), k + 1, done[size_t(k)], size[size_t(k)]);
    m_progress->SetLabel(format_wxstr(_L("Plan %1%: %2% swatches, %3% measured. This plate (%4% boards): %5%"),
                                      from_u8(calibration_plan_id(m_plan)), m_plan.size(), total, boards, boards_txt));
}

void ColorMixCalibrationDialog::create_plate()
{
    const Bundle *b = bundle();
    if (b == nullptr || m_plan.empty())
        return;
    if (m_plan_report.dropped_core > 0) {
        MessageDialog(this, plan_cap_message(), _L("ColorMax Engine - too many mixing filaments"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    const std::vector<unsigned int> tools = m_tools->tool_map();
    std::vector<double>             sched;
    if (!check_tools(this, *b, tools) || !checked_schedule(sched))
        return;
    std::vector<int> rows;
    if (m_only_missing->GetValue()) {
        for (int r = 0; r < m_grid->GetNumberRows() && size_t(r) < m_plan.size(); ++r)
            if (boost::algorithm::trim_copy(into_u8(m_grid->GetCellValue(r, 2))).empty())
                rows.push_back(r);
        if (rows.empty()) {
            MessageDialog(this, _L("Every swatch has a measurement: there is nothing to print."), _L("ColorMax Engine"), wxOK | wxICON_INFORMATION).ShowModal();
            return;
        }
        if (rows.size() == m_plan.size())
            rows.clear();
    }
    if (!close_current_project(this, m_plater, _L("Creating the calibration plate closes the current project and replaces all objects on the plater.")))
        return;
    std::vector<SwatchSpec> swatches;
    if (rows.empty())
        swatches = m_plan;
    else
        for (int r : rows)
            swatches.push_back(m_plan[size_t(r)]);
    m_plate_rows = rows;
    Model plate;
    // Frame in black when it is loaded: high contrast against the white paper for the photo.
    unsigned int frame_tool = 0;
    if (const int k = b->filament_index("black"); k >= 0 && size_t(k) < tools.size())
        frame_tool = tools[size_t(k)];
    const std::string legend = build_calibration_plate(plate, swatches, *b, tools, m_ctx.num_physical, m_layout, frame_tool, bed_depth(),
                                                        m_ctx.first_layer_height);
    center_on_bed(plate, bed_center()); // the middle of the bed prints best
    const nlohmann::json original_colors = original_extruder_colors(m_plater->model());
    m_plater->reset();
    Model &model = m_plater->model();
    model.virtual_extruders          = plate.virtual_extruders;
    model.colormix                   = ModelData();
    model.colormix.bundle_id         = b->id;
    model.colormix.tool_of_filament  = tools;
    model.colormix.slice.layer_schedule = sched;
    // Print the swatches like real prints: the chosen core tool behind the walls.
    model.colormix.slice.core_extruder      = m_core.tool();
    model.colormix.slice.whole_part_regions = true; // full-color cells: segmented as whole parts
    model.colormix.plate_json          = plate_settings_json("plate", tools);
    m_plater->add_model_objects(plate.objects);
    std::vector<std::string> colors = m_ctx.physical_colors;
    for (size_t f = 0; f < tools.size() && f < b->filaments.size(); ++f)
        if (tools[f] >= 1 && tools[f] <= colors.size())
            colors[tools[f] - 1] = b->filaments[f].color;
    if (const unsigned int core = model.colormix.slice.core_extruder; core >= 1 && core <= colors.size())
        colors[core - 1] = core_tool_color(*b, tools, core);
    set_tool_colors(m_plater, colors);
    record_extruder_colors(model, original_colors);
    after_model_replaced();

    set_plate_file(std::string()); // a new plate: its own file
    store_measurements();
    rebuild_plan(); // board / row / column of each swatch on this plate

    m_legend = legend;
    // The plate file holds the settings the plate is made with: the photos are read with them.
    bool saved = ask_save_plate_file();
    if (!saved &&
        MessageDialog(this, _L("Without the plate file, reading the photos later depends on setting everything exactly as now. Save it?"),
                      _L("ColorMax Engine"), wxYES_NO | wxICON_WARNING).ShowModal() == wxID_YES)
        saved = ask_save_plate_file();
    const wxString what = m_plate_rows.empty() ?
        _L("Calibration plate created. Slice and print it, then read the boards with Read a board (scan or photo).") :
        format_wxstr(_L("Plate with the %1% swatches without a measurement created; the values you have are kept in its plate file. Read its "
                        "boards when printed, then save the calibration again."), m_plate_rows.size());
    m_status->SetLabel(what + " " + (saved ? format_wxstr(_L("Plate file: %1%"), from_u8(m_plate_file)) :
                                             _L("No plate file was saved (Save plate file... saves one).")));
}

void ColorMixCalibrationDialog::create_slope_test()
{
    const Bundle *b = bundle();
    if (b == nullptr)
        return;
    const std::vector<unsigned int> tools = m_tools->tool_map();
    std::vector<double>             sched;
    if (!check_tools(this, *b, tools) || !checked_schedule(sched))
        return;
    unsigned int frame_tool = 0;
    if (const int k = b->filament_index("black"); k >= 0 && size_t(k) < tools.size())
        frame_tool = tools[size_t(k)];
    Model             test;
    const std::string legend = build_slope_test(test, *b, tools, m_ctx.num_physical, frame_tool, schedule(), bed_depth());
    center_on_bed(test, bed_center());
    if (test.objects.empty()) {
        MessageDialog(this, from_u8(legend), _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    if (!close_current_project(this, m_plater, _L("Creating the slope test closes the current project and replaces all objects on the plater.")))
        return;
    const nlohmann::json original_colors = original_extruder_colors(m_plater->model());
    m_plater->reset();
    Model &model = m_plater->model();
    model.virtual_extruders         = test.virtual_extruders;
    model.colormix                  = ModelData();
    model.colormix.bundle_id        = b->id;
    model.colormix.tool_of_filament = tools;
    model.colormix.slice.layer_schedule = sched;
    model.colormix.slice.core_extruder      = m_core.tool();
    model.colormix.slice.whole_part_regions = true; // full-color cells: segmented as whole parts
    model.colormix.slice.top_pattern   = false; // layer mixing only: that's what is being judged
    model.colormix.plate_json          = plate_settings_json("slope_test", tools);
    m_plater->add_model_objects(test.objects);
    std::vector<std::string> colors = m_ctx.physical_colors;
    for (size_t f = 0; f < tools.size() && f < b->filaments.size(); ++f)
        if (tools[f] >= 1 && tools[f] <= colors.size())
            colors[tools[f] - 1] = b->filaments[f].color;
    if (const unsigned int core = model.colormix.slice.core_extruder; core >= 1 && core <= colors.size())
        colors[core - 1] = core_tool_color(*b, tools, core);
    set_tool_colors(m_plater, colors);
    record_extruder_colors(model, original_colors);
    after_model_replaced();
    m_status->SetLabel(_L("Slope test created. Print it with the print settings you use for real prints."));
    MessageDialog(this, from_u8(legend), _L("ColorMax Engine - slope test"), wxOK | wxICON_INFORMATION).ShowModal();
}

int ColorMixCalibrationDialog::plate_position(size_t i) const
{
    if (m_plate_rows.empty())
        return int(i);
    const auto it = std::find(m_plate_rows.begin(), m_plate_rows.end(), int(i));
    return it == m_plate_rows.end() ? -1 : int(it - m_plate_rows.begin());
}

void ColorMixCalibrationDialog::store_measurements()
{
    nlohmann::json measured = nlohmann::json::object();
    for (int r = 0; r < m_grid->GetNumberRows(); ++r) {
        const std::string v = boost::algorithm::trim_copy(into_u8(m_grid->GetCellValue(r, 2)));
        if (!v.empty())
            measured[std::to_string(r)] = v;
    }
    const std::string plan = calibration_plan_id(m_plan);
    auto update = [&](const std::string &text) -> std::string {
        nlohmann::json j = json_object(text);
        if (j.empty() || j.value("kind", std::string()) != "plate" || j.value("plan_id", std::string()) != plan)
            return std::string();
        j["measured"] = measured;
        return j.dump();
    };
    ModelData &d = m_plater->model().colormix;
    if (const std::string j = update(d.plate_json); !j.empty())
        d.plate_json = j;
    update_progress();
    // The plate file: only its measurements change.
    if (!m_plate_file.empty()) {
        boost::nowide::ifstream in(m_plate_file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        nlohmann::json f = json_object(text);
        if (f.value("format", std::string()) == "colormax-calibration-plate" && f.contains("settings") &&
            f["settings"].value("plan_id", std::string()) == plan) {
            f["measured"]         = measured;
            f["calibration_name"] = into_u8(m_cal_name->GetValue());
            if (!write_text_file_safely(m_plate_file, f.dump(1)))
                m_status->SetLabel(format_wxstr(_L("The measurements could not be saved to the plate file %1%."), from_u8(m_plate_file)));
        }
    }
}

void ColorMixCalibrationDialog::load_csv()
{
    wxFileDialog dlg(this, _L("Load measurements"), wxEmptyString, wxEmptyString, "CSV (*.csv;*.txt)|*.csv;*.txt",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    boost::nowide::ifstream in(into_u8(dlg.GetPath()), std::ios::binary);
    const std::string       text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const CsvMeasurements   csv = parse_measurement_csv(text, size_t(m_grid->GetNumberRows()));
    if (!csv.error.empty()) {
        MessageDialog(this, from_u8(csv.error) + "\n\n" + _L("Nothing was loaded."), _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    for (size_t r = 0; r < csv.values.size(); ++r)
        if (!csv.values[r].empty())
            m_grid->SetCellValue(int(r), 2, from_u8(csv.values[r]));
    wxString msg = format_wxstr(_L("Loaded %1% measurements."), csv.loaded);
    if (!csv.bad_lines.empty()) {
        wxString lines;
        for (int l : csv.bad_lines)
            lines += (lines.empty() ? "" : ", ") + wxString::Format("%d", l);
        msg += " " + format_wxstr(_L("Lines that could not be read (skipped): %1%."), lines);
    }
    m_status->SetLabel(msg);
    store_measurements();
}

void ColorMixCalibrationDialog::save_legend()
{
    if (m_legend.empty())
        return;
    wxFileDialog dlg(this, _L("Save the swatch list"), wxEmptyString, "colormix_calibration_swatches.txt",
                     "Text (*.txt)|*.txt", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() == wxID_OK) {
        boost::nowide::ofstream out(into_u8(dlg.GetPath()));
        out << m_legend;
    }
}

namespace {

wxString photo_instructions(bool scanner)
{
    const wxString common = _L("\n\nTHEN\nPress Open photo..., pick the board number (count the dots on its top edge) and click the four corners of the board: first the corner next to the dots, then the others clockwise. Check that the yellow outlines sit on the swatches, then press Read colors. Repeat for every board.\n\nThe plate file keeps the settings the plate was made with (they are locked while it is open) and the measurements as you read them. Save calibration checks the readings: a filament whose swatches are all far off (e.g. it didn't print) is offered to be left out, and \"only swatches without a measurement\" then prints just those swatches again.");
    return (scanner ? _L("FLATBED SCANNER (recommended: fixed, even light, the most repeatable colors)\n\nSetup\n- Cut a hole the size of a board (196 x 23 mm) into the middle of a sheet of plain white printer paper. A snug hole is best: where the paper doesn't reach the board you see the lid, held 4 mm up by the board, which reads gray (the reading skips it, but less paper is left as reference).\n- Clean the glass. Lay the sheet flat on the glass, away from the edges of the glass (the outer centimeter can be darker), and put the board face-down in the hole. Either face works.\n- Close the lid gently. Scan one board at a time and use the same settings for all boards.\n\nSettings\n- Color, 24-bit (8 bits per channel), 300 dpi (600 works too, but makes big files).\n- Save as PNG or TIFF (any compression). JPEG at the highest quality works too. Not PDF.\n- Leave Brightness, Contrast and Gamma at their defaults. Brightness in particular subtracts from every pixel: dark colors then come out too dark, and ColorMax can't undo it.\n- If your scanner software has an advanced / professional mode: turn OFF auto exposure, auto levels / auto color fix, color restoration, backlight correction, dust or scratch removal, descreening, sharpening / text enhancement, automatic cropping or photo splitting (it would cut off the paper). If the reading says the paper is pure white and there is an Exposure or Levels setting, lower the exposure a little or set the output highlight (white point) to about 240.\n- Without an advanced mode, scan with the defaults: a paper that is only just pure white costs about 1 dE.\n- Standard sRGB color (the default).") :
                      _L("PHONE OR CAMERA PHOTO (only if you have no scanner: a scanner gives much more repeatable colors)\n\nSetup\n- Lay the board face-up on a sheet of plain white printer paper, with at least 2 cm of paper on every side. Either face works.\n- Light: two identical lamps left and right of the board, fairly high (no flash, no direct sun). Daylight works, but most office paper contains brighteners that glow blue in daylight; the colors then come out a little too yellow.\n- Hold the phone straight above the board, parallel to it, and use the 2x or 3x zoom from further away (less distortion, no phone shadow). The board and its paper border should fill most of the picture. Avoid reflections.\n- One board per photo, same light and settings for all boards.\n\nSettings (Samsung: Camera > More > Pro; other phones have similar settings)\n- Fixed white balance (Daylight or about 5000-5500 K, never Auto), low ISO (50-100), a fixed exposure: the paper must look bright but not pure white (EV -0.3 to -0.7 if the reading says it's overexposed). Focus on the board.\n- Highest JPEG quality (or PNG). RAW files can't be read directly.\n- Turn OFF: flash, scene optimizer / AI scene detection, auto HDR, filters and beauty effects, Night and Portrait mode, any automatic editing in the Gallery app. Don't edit or crop the photo afterwards.")) + common;
}

const char *PHOTO_SOURCE_KEY = "colormix_photo_source";

// A scanner gives far more repeatable colors than a phone: it is the default.
bool saved_source_is_scanner() { return wxGetApp().app_config->get(PHOTO_SOURCE_KEY) != "camera"; }
void save_source(bool scanner) { wxGetApp().app_config->set(PHOTO_SOURCE_KEY, scanner ? "scanner" : "camera"); }

// Instructions for taking the photo or scan. Returns false when cancelled; `scanner` is in/out.
bool show_photo_instructions(wxWindow *parent, bool &scanner, bool can_cancel)
{
    wxDialog dlg(parent, wxID_ANY, _L("ColorMax Engine - how to photograph or scan the boards"), wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    const int em  = wxGetApp().em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);
    wxString  choices[] = {_L("Flatbed scanner (strongly recommended)"), _L("Phone or camera photo")};
    auto     *source    = new wxRadioBox(&dlg, wxID_ANY, _L("I will use"), wxDefaultPosition, wxDefaultSize, 2, choices, 2, wxRA_SPECIFY_COLS);
    source->SetSelection(scanner ? 0 : 1);
    source->SetItemToolTip(0, _L("Fixed, even light and no automatic color processing: the most repeatable colors. On the first test "
                                 "plate the scan's colors were consistent where a phone photo read ~8 units too yellow."));
    top->Add(source, 0, wxEXPAND | wxALL, em);
    auto *text = new wxTextCtrl(&dlg, wxID_ANY, photo_instructions(scanner), wxDefaultPosition, wxSize(80 * em, 40 * em),
                                wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    top->Add(text, 1, wxEXPAND | wxLEFT | wxRIGHT, em);
    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer();
    btns->Add(new wxButton(&dlg, wxID_OK, can_cancel ? _L("Continue") : _L("Close")), 0, wxRIGHT, em / 2);
    if (can_cancel)
        btns->Add(new wxButton(&dlg, wxID_CANCEL, _L("Cancel")), 0);
    top->Add(btns, 0, wxEXPAND | wxALL, em);
    dlg.SetSizer(top);
    dlg.Fit();
    dlg.CenterOnParent();
    source->Bind(wxEVT_RADIOBOX, [text, source](wxCommandEvent &) {
        text->SetValue(photo_instructions(source->GetSelection() == 0));
        text->SetInsertionPoint(0);
    });
    if (dlg.ShowModal() != wxID_OK)
        return false;
    scanner = source->GetSelection() == 0;
    save_source(scanner);
    return true;
}

} // namespace

void ColorMixCalibrationDialog::read_photo()
{
    if (m_plan.empty())
        return;
    // Photos are read with the settings the plate was made with: those of its plate file.
    if (m_plate_file.empty()) {
        wxMessageDialog ask(this,
            _L("Reading photos needs the plate file saved when the plate was created: it holds the settings the plate was made with "
               "(which swatch is where) and keeps the measurements.\n\n"
               "No plate file? If the settings below are still exactly those the plate was made with, save a plate file from them."),
            _L("ColorMax Engine"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
        ask.SetYesNoCancelLabels(_L("Open plate file..."), _L("Save the current settings as plate file..."), _L("Cancel"));
        const int rc = ask.ShowModal();
        if (rc == wxID_YES) {
            wxFileDialog dlg(this, _L("Open the plate file"), from_u8(wxGetApp().app_config->get(LAST_PLATE_FILE_KEY)), wxEmptyString,
                             "ColorMax plate (*.json)|*.json", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dlg.ShowModal() != wxID_OK || !open_plate_file(into_u8(dlg.GetPath())))
                return;
        } else if (rc != wxID_NO || !ask_save_plate_file())
            return;
    }
    bool scanner = saved_source_is_scanner();
    if (!m_photo_guide_shown) {
        if (!show_photo_instructions(this, scanner, true))
            return;
        m_photo_guide_shown = true;
    }
    // Start with the next board not read yet in this session.
    const int boards = std::max(1, board_count(plate_swatch_count(), m_layout));
    m_boards_read.resize(size_t(boards), false);
    int next = (m_last_board + 1) % boards;
    for (int k = 0; k < boards; ++k)
        if (const int c = (m_last_board + 1 + k) % boards; !m_boards_read[size_t(c)]) {
            next = c;
            break;
        }
    ColorMixPhotoDialog dlg(this, boards, m_layout, scanner, next);
    const int  rc = dlg.ShowModal();
    save_source(dlg.scanner());
    if (rc != wxID_OK)
        return;
    if (dlg.board() >= 0 && dlg.board() < boards) {
        m_boards_read[size_t(dlg.board())] = true;
        m_last_board                       = dlg.board();
    }
    const int per = m_layout.swatches_per_board();
    int       n   = 0;
    wxString  skipped;
    for (size_t k = 0; k < dlg.cells().size(); ++k) {
        const int pos = dlg.board() * per + int(k);
        if (pos >= plate_swatch_count())
            break;
        const int idx = m_plate_rows.empty() ? pos : m_plate_rows[size_t(pos)];
        if (idx >= m_grid->GetNumberRows())
            break;
        if (k >= dlg.cells_ok().size() || !dlg.cells_ok()[k]) {
            m_grid->SetCellValue(idx, 2, wxEmptyString);
            skipped += (skipped.empty() ? "" : ", ") + wxString::Format("%d", idx + 1);
            continue;
        }
        const Lab &l = dlg.cells()[k];
        m_grid->SetCellValue(idx, 2, from_u8(format_lab(l, 1)));
        ++n;
    }
    wxString msg = format_wxstr(_L("Board %1%: read %2% swatches from the photo."), dlg.board() + 1, n);
    if (!skipped.empty())
        msg += " " + format_wxstr(_L("Not read reliably (outside the photo or not uniform), left empty: swatches %1%."), skipped);
    m_status->SetLabel(msg);
    store_measurements();
}

// ---------------------------------------------------------------- color range chart

namespace {
constexpr double GAMUT_IN_RANGE_DE = 3.;   // a cell / model color is "printable" within this (CIEDE2000)

wxColour lab_colour(const Lab &lab)
{
    const LinRGB l = lab_to_linear(lab);
    auto c8 = [](double v) { return (unsigned char) std::lround(std::clamp(linear_to_srgb8(std::clamp(v, 0., 1.)), 0., 255.)); };
    return wxColour(c8(l[0]), c8(l[1]), c8(l[2]));
}
// Text that reads on this color.
wxColour ink_on(const Lab &lab) { return lab.L > 55. ? wxColour(0, 0, 0) : wxColour(255, 255, 255); }
} // namespace

ColorMixGamutDialog::ColorMixGamutDialog(wxWindow *parent, const Bundle &bundle, int calibration, std::vector<unsigned int> tools,
                                         const LayerSchedule &schedule, bool use_blends, bool dual_walls,
                                         std::vector<std::pair<Lab, std::string>> model_colors)
    : DPIDialog(parent, wxID_ANY, _L("ColorMax Engine - color range"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_bundle(bundle)
    , m_loaded_tools(std::move(tools))
    , m_schedule(schedule)
    , m_use_blends(use_blends)
    , m_model_colors(std::move(model_colors))
{
    const int em = wxGetApp().em_unit();
    m_pitch      = std::max(22, int(3.4 * em));
    SetBackgroundColour(wxGetApp().get_window_default_clr());
    auto *top = new wxBoxSizer(wxVERTICAL);
    auto *intro = new wxStaticText(this, wxID_ANY,
        _L("Which colors this filament set can print. Each square: top = a screen color, bottom = the closest color the spools "
           "can print. Framed squares are within 3 ΔE (hard to tell apart); the others show how far off the closest one is. "
           "The last column holds the grays. Below the chart: your model's colors, as designed and the closest color the spools can print. Hover for details."));
    intro->Wrap(100 * em);
    top->Add(intro, 0, wxEXPAND | wxALL, em);

    auto *row = new wxBoxSizer(wxHORIZONTAL);
    m_spools = new wxChoice(this, wxID_ANY);
    m_spools->Append(_L("Spools loaded in the printer"));
    m_spools->Append(_L("All spools of the set"));
    m_spools->SetSelection(0);
    m_spools->SetToolTip(_L("The range of the spools you have loaded, or of the whole set (e.g. to see what a spool you don't have loaded would add)."));
    row->Add(m_spools, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_cal = new wxChoice(this, wxID_ANY);
    m_cal->Append(_L("Uncalibrated"));
    for (const Calibration &c : m_bundle.calibrations)
        m_cal->Append(format_wxstr(_L("Calibration: %1%"), from_u8(c.id + (c.date.empty() ? "" : " (" + c.date + ")"))));
    m_cal->SetSelection(calibration >= 0 && calibration < int(m_bundle.calibrations.size()) ? calibration + 1 : 0);
    m_cal->SetToolTip(_L("Predict the printed colors with a calibration (measured) or without (estimated)."));
    row->Add(m_cal, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_saturation = new wxChoice(this, wxID_ANY);
    m_saturation->Append(_L("Vivid screen colors"));
    m_saturation->Append(_L("Medium colors"));
    m_saturation->Append(_L("Soft colors"));
    m_saturation->SetSelection(0);
    m_saturation->SetToolTip(_L("How saturated the screen colors of the chart are: vivid = the most saturated colors a screen shows, soft = pastel "
                                "and muted ones."));
    row->Add(m_saturation, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_dual = new wxCheckBox(this, wxID_ANY, _L("Dual walls"));
    m_dual->SetValue(dual_walls && use_blends);
    m_dual->Enable(use_blends);
    m_dual->SetToolTip(_L("Also use recipes with a second, different inner wall (more colors, more tool changes)."));
    row->Add(m_dual, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_show_model = new wxCheckBox(this, wxID_ANY, _L("Show the model's colors"));
    m_show_model->SetValue(!m_model_colors.empty());
    m_show_model->Enable(!m_model_colors.empty());
    row->Add(m_show_model, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(row, 0, wxLEFT | wxRIGHT, em);

    m_canvas = new wxWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE);
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
    top->Add(m_canvas, 0, wxALL, em);
    m_summary = new wxStaticText(this, wxID_ANY, wxEmptyString);
    top->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    auto *save = new wxButton(this, wxID_ANY, _L("Save as PNG..."));
    btns->Add(save, 0, wxRIGHT, em / 2);
    btns->AddStretchSpacer();
    btns->Add(new wxButton(this, wxID_CANCEL, _L("Close")), 0);
    top->Add(btns, 0, wxEXPAND | wxALL, em);
    SetSizer(top);
    SetEscapeId(wxID_CANCEL);

    m_canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(m_canvas);
        dc.SetBackground(wxBrush(wxGetApp().get_window_default_clr()));
        dc.Clear();
        if (m_bitmap.IsOk())
            dc.DrawBitmap(m_bitmap, 0, 0);
    });
    m_canvas->Bind(wxEVT_MOTION, [this](wxMouseEvent &e) { update_tooltip(e.GetPosition()); });
    for (wxChoice *c : {m_spools, m_cal, m_saturation})
        c->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { recompute(); });
    m_dual->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { recompute(); });
    m_show_model->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        render();
    });
    save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxFileDialog dlg(this, _L("Save the color range chart"), wxEmptyString, from_u8("ColorMax color range - " + m_bundle.name + ".png"),
                         "PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK || !m_bitmap.IsOk())
            return;
        if (!m_bitmap.ConvertToImage().SaveFile(dlg.GetPath(), wxBITMAP_TYPE_PNG))
            MessageDialog(this, _L("The chart could not be saved."), _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
    });

    recompute();
    Fit();
    CenterOnParent();
}

void ColorMixGamutDialog::on_dpi_changed(const wxRect &)
{
    m_pitch = std::max(22, int(3.4 * wxGetApp().em_unit()));
    render();
    Fit();
    Refresh();
}

void ColorMixGamutDialog::recompute()
{
    wxBusyCursor wait;
    const int ci = m_cal->GetSelection() - 1;
    const Calibration *cal = ci >= 0 && ci < int(m_bundle.calibrations.size()) ? &m_bundle.calibrations[size_t(ci)] : nullptr;
    const Predictor pred(m_bundle, cal, m_schedule);
    std::vector<unsigned int> tools = m_loaded_tools;
    if (m_spools->GetSelection() == 1)
        for (size_t i = 0; i < m_bundle.filaments.size(); ++i) {
            if (tools.size() <= i)
                tools.push_back(0);
            tools[i] = unsigned(i + 1);
        }
    ConvertOptions opt;
    opt.schedule   = m_schedule;
    opt.use_blends = m_use_blends;
    opt.dual_walls = m_use_blends && m_dual->GetValue();
    const double scale[] = {1., 0.6, 0.35};
    m_chart = make_gamut_chart(m_bundle, pred, tools, opt, m_model_colors, 24, 9, scale[std::clamp(m_saturation->GetSelection(), 0, 2)]);
    render();
}

wxString ColorMixGamutDialog::caption() const
{
    const int ci = m_cal->GetSelection() - 1;
    wxString s = from_u8(m_bundle.name) + " - " + m_spools->GetStringSelection() + ", " +
                 (ci >= 0 ? from_u8(m_bundle.calibrations[size_t(ci)].id) : _L("uncalibrated")) +
                 (m_dual->GetValue() ? ", " + _L("dual walls") : wxString()) + ", " + m_saturation->GetStringSelection().Lower();
    return s;
}

void ColorMixGamutDialog::render()
{
    const int em = wxGetApp().em_unit();
    if (!m_chart.problem.empty()) {
        m_bitmap = wxBitmap();
        m_summary->SetLabel(from_u8(m_chart.problem));
        m_canvas->SetMinSize(wxSize(10 * em, 2 * em));
        Layout();
        m_canvas->Refresh();
        return;
    }
    const int  P = m_pitch, gap = std::max(2, P / 12), cols = m_chart.columns(), rows = m_chart.rows();
    const int  gray_gap = P / 3;   // between the hues and the grays
    const int  header = int(1.8 * em), line = int(2.0 * em);
    const int  W = cols * P + gray_gap + 2;
    const bool show_model = m_show_model->GetValue() && !m_chart.points.empty();
    auto col_x = [&](int c) { return c * P + (c >= m_chart.hues ? gray_gap : 0); };

    // The model's colors: designed over printed, farthest first, in rows under the chart.
    m_strip_order.clear();
    for (size_t i = 0; i < m_chart.points.size(); ++i)
        m_strip_order.push_back(int(i));
    std::stable_sort(m_strip_order.begin(), m_strip_order.end(), [this](int a, int b) { return m_chart.points[size_t(a)].de > m_chart.points[size_t(b)].de; });
    m_strip_pitch   = std::max(16, int(0.75 * P));
    m_strip_per_row = std::max(1, (W - 2) / m_strip_pitch);
    m_strip_top     = header + rows * P + line + line;   // a caption line, then the strip's heading
    const int strip_rows = show_model ? int((m_strip_order.size() + size_t(m_strip_per_row) - 1) / size_t(m_strip_per_row)) : 0;
    const int H = show_model ? m_strip_top + strip_rows * m_strip_pitch + em / 2 : header + rows * P + line + em / 2;

    wxBitmap bmp(W, H);
    int      in_range = 0, model_in = 0;
    double   model_worst = 0.;
    {
        wxMemoryDC dc(bmp);
        const wxColour bg = wxGetApp().get_window_default_clr(), fg = wxGetApp().get_label_clr_default();
        dc.SetBackground(wxBrush(bg));
        dc.Clear();
        wxFont small_font = GetFont();
        small_font.SetPointSize(std::max(6, small_font.GetPointSize() - 1));
        dc.SetFont(small_font);
        dc.SetTextForeground(fg);
        dc.DrawText(_L("Hue") + "  " + wxString::FromUTF8("\xe2\x86\x92"), 2, 2);   // "Hue  ->"
        {
            const wxString g = _L("Grays");
            const wxSize   t = dc.GetTextExtent(g);
            dc.DrawText(g, col_x(m_chart.hues) + (P - t.x) / 2, 2);
        }
        // A color pair: `top` over `bottom`, framed when close, else the distance on the bottom half.
        auto pair = [&](int x, int y, int s, const Lab &top, const Lab &bottom, double de, bool red_when_far) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(lab_colour(top)));
            dc.DrawRectangle(x, y, s, s / 2);
            dc.SetBrush(wxBrush(lab_colour(bottom)));
            dc.DrawRectangle(x, y + s / 2, s, s - s / 2);
            const int fw = std::max(2, s / 12);
            if (de <= GAMUT_IN_RANGE_DE) {
                dc.SetPen(wxPen(fg, fw));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRectangle(x - fw / 2, y - fw / 2, s + fw, s + fw);
            } else {
                if (red_when_far) {
                    dc.SetPen(wxPen(wxColour(230, 0, 0), fw));
                    dc.SetBrush(*wxTRANSPARENT_BRUSH);
                    dc.DrawRectangle(x - fw / 2, y - fw / 2, s + fw, s + fw);
                }
                const wxString t  = wxString::Format("%.0f", de);
                const wxSize   te = dc.GetTextExtent(t);
                dc.SetTextForeground(ink_on(bottom));
                dc.DrawText(t, x + (s - te.x) / 2, y + s / 2 + (s - s / 2 - te.y) / 2);
            }
        };
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                const GamutCell &cell = m_chart.cell(r, c);
                const Lab printed = cell.candidate >= 0 ? m_chart.candidates[size_t(cell.candidate)].prediction.color : cell.target;
                in_range += cell.de <= GAMUT_IN_RANGE_DE ? 1 : 0;
                pair(col_x(c) + gap / 2 + 1, header + r * P + gap / 2, P - gap, cell.target, printed, cell.de, false);
            }
        for (const GamutPoint &p : m_chart.points) {
            model_in += p.de <= GAMUT_IN_RANGE_DE ? 1 : 0;
            model_worst = std::max(model_worst, p.de);
        }
        dc.SetTextForeground(fg);
        dc.DrawText(caption(), 2, header + rows * P + em / 2);
        if (show_model) {
            dc.SetTextForeground(fg);
            dc.DrawText(format_wxstr(_L("Your model's %1% colors - top: as designed, bottom: the closest printable color; farthest first (red frame: more "
                                        "than 3 ΔE off)"), m_chart.points.size()),
                        2, m_strip_top - line + em / 3);
            const int sg = std::max(2, m_strip_pitch / 8);
            for (size_t k = 0; k < m_strip_order.size(); ++k) {
                const GamutPoint &p = m_chart.points[size_t(m_strip_order[k])];
                const int x = int(k % size_t(m_strip_per_row)) * m_strip_pitch + sg / 2 + 1;
                const int y = m_strip_top + int(k / size_t(m_strip_per_row)) * m_strip_pitch + sg / 2;
                const Lab printed = p.candidate >= 0 ? m_chart.candidates[size_t(p.candidate)].prediction.color : p.color;
                pair(x, y, m_strip_pitch - sg, p.color, printed, p.de, true);
            }
        }
        dc.SelectObject(wxNullBitmap);
    }
    wxString sum = format_wxstr(_L("%1% of %2% squares can be printed within 3 ΔE (%3% recipes)."), in_range, rows * cols, m_chart.candidates.size());
    if (!m_chart.points.empty())
        sum += " " + format_wxstr(_L("Model colors: %1% of %2% within 3 ΔE, the farthest %3$.1f ΔE away."), model_in, m_chart.points.size(),
                                  model_worst);
    m_summary->SetLabel(sum);
    m_summary->Wrap(std::max(W, 60 * em));
    m_bitmap = bmp;
    m_canvas->SetMinSize(wxSize(W, H));
    m_canvas->SetSize(wxSize(W, H));
    m_tip.clear();
    Layout();
    Fit();
    m_canvas->Refresh();
}

void ColorMixGamutDialog::update_tooltip(const wxPoint &pos)
{
    if (!m_chart.problem.empty() || m_chart.cells.empty())
        return;
    const int em = wxGetApp().em_unit(), P = m_pitch, gray_gap = P / 3, header = int(1.8 * em);
    auto recipe  = [this](int k) { return k >= 0 ? from_u8(m_chart.candidates[size_t(k)].name) : wxString(); };
    auto printed = [this](int k) { return k >= 0 ? from_u8(lab_to_hex(m_chart.candidates[size_t(k)].prediction.color)) : wxString(); };
    wxString tip;
    if (m_show_model->GetValue() && pos.y >= m_strip_top && m_strip_pitch > 0) {
        const size_t k = size_t((pos.y - m_strip_top) / m_strip_pitch) * size_t(m_strip_per_row) + size_t(pos.x / m_strip_pitch);
        if (pos.x / m_strip_pitch < m_strip_per_row && k < m_strip_order.size()) {
            const GamutPoint &p = m_chart.points[size_t(m_strip_order[k])];
            tip = format_wxstr(_L("Model color %1% (%2%)\nClosest printable: %3%, %4$.1f ΔE away\n%5%"), from_u8(lab_to_hex(p.color)), from_u8(p.label),
                               printed(p.candidate), p.de, recipe(p.candidate));
        }
    } else {
        int c = -1;
        if (pos.x < m_chart.hues * P)
            c = pos.x / P;
        else if (pos.x >= m_chart.hues * P + gray_gap && pos.x < m_chart.columns() * P + gray_gap)
            c = m_chart.hues;
        const int r = pos.y >= header ? (pos.y - header) / P : -1;
        if (c >= 0 && r >= 0 && r < m_chart.rows()) {
            const GamutCell &cell = m_chart.cell(r, c);
            tip = format_wxstr(_L("Screen color %1%\nClosest printable: %2%, %3$.1f ΔE away\n%4%"), from_u8(lab_to_hex(cell.target)),
                               printed(cell.candidate), cell.de, recipe(cell.candidate));
        }
    }
    if (tip != m_tip) {
        m_tip = tip;
        m_canvas->SetToolTip(tip);
    }
}

// ---------------------------------------------------------------- filament database lookup

namespace {
std::string filament_db_path() { return (boost::filesystem::path(data_dir()) / "colormix" / "filament_db.json").string(); }

// Downloads the measured swatches of filamentcolors.xyz (page by page) and SpoolmanDB's catalog.
bool download_filament_db(wxWindow *parent, std::vector<FilamentRecord> &out, wxString &error)
{
    wxProgressDialog prog(_L("ColorMax Engine - filament database"),
                          _L("Downloading measured colors from filamentcolors.xyz...") + "\n" + format_wxstr(_L("%1% filaments so far"), 0), 32, parent,
                          wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
    auto fetch = [&error](const std::string &url, std::string &body) {
        bool ok = false;
        Http::get(url)
            .timeout_max(60)
            .size_limit(64 * 1024 * 1024) // SpoolmanDB's file is over the default 5 MB
            .on_complete([&](std::string b, unsigned) {
                body = std::move(b);
                ok   = true;
            })
            .on_error([&](std::string, std::string e, unsigned status) {
                error = from_u8(e.empty() ? "HTTP " + std::to_string(status) : e);
            })
            .perform_sync();
        return ok;
    };
    std::vector<FilamentRecord> measured;
    std::string                 url = FILAMENTCOLORS_SWATCH_URL;
    for (int page = 1; !url.empty() && page <= 60; ++page) {
        std::string body, next;
        if (!fetch(url, body))
            return false;
        const auto recs = parse_filamentcolors(body, &next);
        measured.insert(measured.end(), recs.begin(), recs.end());
        url = next;
        if (!prog.Update(std::min(page, 30), _L("Downloading measured colors from filamentcolors.xyz...") + "\n" +
                                                 format_wxstr(_L("%1% filaments so far"), measured.size()))) {
            error = _L("Cancelled.");
            return false;
        }
    }
    if (!prog.Update(31, _L("Downloading the catalog from SpoolmanDB...") + "\n" + format_wxstr(_L("%1% filaments so far"), measured.size())))
        return false;
    std::string body;
    if (!fetch(SPOOLMANDB_URL, body))
        return false;
    const auto catalog = parse_spoolmandb(body);
    if (measured.empty() && catalog.empty()) {
        error = _L("The downloads contained no filaments.");
        return false;
    }
    out = merge_filament_records(measured, catalog);
    return true;
}
} // namespace

ColorMixFilamentLookupDialog::ColorMixFilamentLookupDialog(wxWindow *parent, const wxString &query)
    : DPIDialog(parent, wxID_ANY, _L("ColorMax Engine - look up a filament"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    const int em  = wxGetApp().em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);
    auto *intro = new wxStaticText(this, wxID_ANY,
        _L("Search by brand, color name and material (e.g. \"polymaker panchroma red\"). Measured colors come from printed swatches and "
           "are the better start; catalog colors are what the maker states. TD (transmission distance, mm) tells how translucent "
           "a filament is: higher = more light passes."));
    intro->Wrap(90 * em);
    top->Add(intro, 0, wxEXPAND | wxALL, em);
    m_search = new wxTextCtrl(this, wxID_ANY, query);
    m_search->SetHint(_L("Search..."));
    top->Add(m_search, 0, wxEXPAND | wxLEFT | wxRIGHT, em);
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(95 * em, 30 * em), wxLC_REPORT | wxLC_SINGLE_SEL);
    m_swatch = wxSize(std::max(16, int(2.4 * em)), std::max(10, int(1.4 * em)));
    m_images = new wxImageList(m_swatch.GetWidth(), m_swatch.GetHeight(), false);
    m_list->SetImageList(m_images, wxIMAGE_LIST_SMALL);
    m_list->AppendColumn(_L("Brand"), wxLIST_FORMAT_LEFT, 18 * em);
    m_list->AppendColumn(_L("Filament"), wxLIST_FORMAT_LEFT, 28 * em);
    m_list->AppendColumn(_L("Material"), wxLIST_FORMAT_LEFT, 13 * em);
    m_list->AppendColumn(_L("Color"), wxLIST_FORMAT_LEFT, 10 * em);
    m_list->AppendColumn(_L("TD (mm)"), wxLIST_FORMAT_RIGHT, 8 * em);
    m_list->AppendColumn(_L("Source"), wxLIST_FORMAT_LEFT, 16 * em);
    top->Add(m_list, 1, wxEXPAND | wxALL, em);
    m_info = new wxStaticText(this, wxID_ANY, wxEmptyString);
    top->Add(m_info, 0, wxEXPAND | wxLEFT | wxRIGHT, em);
    auto *btns   = new wxBoxSizer(wxHORIZONTAL);
    auto *update = new wxButton(this, wxID_ANY, _L("Download / update database"));
    update->SetToolTip(_L("Downloads about 8 MB: the measured swatches of filamentcolors.xyz and the SpoolmanDB catalog. Kept for later "
                          "lookups; update it now and then for new filaments."));
    btns->Add(update, 0, wxRIGHT, em / 2);
    btns->AddStretchSpacer();
    m_ok = new wxButton(this, wxID_OK, _L("Use this filament"));
    m_ok->Enable(false);
    btns->Add(m_ok, 0, wxRIGHT, em / 2);
    btns->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0);
    top->Add(btns, 0, wxEXPAND | wxALL, em);
    SetSizer(top);
    Fit();
    CenterOnParent();

    m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { refill(); });
    m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent &e) {
        m_sel = int(e.GetIndex());
        m_ok->Enable(true);
    });
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent &e) {
        m_sel = int(e.GetIndex());
        EndModal(wxID_OK);
    });
    update->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { download(); });

    boost::nowide::ifstream in(filament_db_path(), std::ios::binary);
    if (in) {
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        m_records = filament_db_from_json(text, &m_downloaded);
    }
    refill();
    if (m_hits.empty() && !m_search->GetValue().empty()) {
        m_search->ChangeValue(wxEmptyString);
        refill();
    }
    if (m_records.empty())
        CallAfter([this]() {
            if (MessageDialog(this, _L("The filament database isn't on this computer yet. Download it now (about 8 MB from filamentcolors.xyz and "
                                       "GitHub)?"),
                              _L("ColorMax Engine"), wxYES_NO | wxICON_QUESTION).ShowModal() == wxID_YES)
                download();
        });
}

ColorMixFilamentLookupDialog::~ColorMixFilamentLookupDialog() { delete m_images; }

void ColorMixFilamentLookupDialog::on_dpi_changed(const wxRect &) { Layout(); Refresh(); }

bool ColorMixFilamentLookupDialog::download()
{
    std::vector<FilamentRecord> recs;
    wxString                    error;
    if (!download_filament_db(this, recs, error)) {
        MessageDialog(this, format_wxstr(_L("The filament database could not be downloaded: %1%"), error), _L("ColorMax Engine"),
                      wxOK | wxICON_WARNING).ShowModal();
        return false;
    }
    char date[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
    m_records    = std::move(recs);
    m_downloaded = date;
    boost::system::error_code ec;
    boost::filesystem::create_directories(boost::filesystem::path(filament_db_path()).parent_path(), ec);
    boost::nowide::ofstream out(filament_db_path(), std::ios::binary | std::ios::trunc);
    out << filament_db_to_json(m_records, m_downloaded);
    refill();
    return true;
}

void ColorMixFilamentLookupDialog::refill()
{
    m_list->DeleteAllItems();
    m_images->RemoveAll();
    m_sel = -1;
    m_ok->Enable(false);
    m_hits = search_filaments(m_records, into_u8(m_search->GetValue()), 300);
    for (size_t k = 0; k < m_hits.size(); ++k) {
        const FilamentRecord &f = m_records[m_hits[k]];
        const int img = m_images->Add(swatch_bitmap(f.color, m_swatch.GetWidth(), m_swatch.GetHeight()));
        const long row = m_list->InsertItem(long(k), from_u8(f.manufacturer), img);
        m_list->SetItem(row, 1, from_u8(f.name) + (f.effect ? " *" : wxString()));
        m_list->SetItem(row, 2, from_u8(f.material));
        m_list->SetItem(row, 3, from_u8(f.color));
        m_list->SetItem(row, 4, f.td > 0. ? wxString::Format("%.1f", f.td) : wxString());
        m_list->SetItem(row, 5, f.measured ? _L("measured") + " (filamentcolors.xyz)" : _L("catalog") + " (SpoolmanDB)");
    }
    size_t with_td = 0;
    for (const FilamentRecord &f : m_records)
        with_td += f.td > 0. ? 1 : 0;
    m_info->SetLabel(m_records.empty() ?
        _L("No filament database on this computer yet: press Download / update database.") :
        format_wxstr(_L("%1% of %2% filaments shown (%3% with TD; downloaded %4%; * = glitter / silk / multicolor, prints solid only). "
                        "Measured colors and TD: filamentcolors.xyz (CC BY 4.0). Catalog: SpoolmanDB (MIT)."),
                     m_hits.size(), m_records.size(), with_td, from_u8(m_downloaded)));
    m_info->Wrap(95 * wxGetApp().em_unit());
    Layout();
}

const FilamentRecord *ColorMixFilamentLookupDialog::selected() const
{
    return m_sel >= 0 && size_t(m_sel) < m_hits.size() ? &m_records[m_hits[size_t(m_sel)]] : nullptr;
}

// ---------------------------------------------------------------- filament set editor

namespace {
enum BundleCol { BC_NAME = 0, BC_COLOR, BC_TOOL, BC_MIX, BC_TD, BC_ROLE, BC_COUNT };
// The "Mixing" column, in MixRole order.
wxArrayString mix_role_names()
{
    wxArrayString a;
    a.Add(_L("Mix"));
    a.Add(_L("Accent (glitter)"));
    a.Add(_L("Solid only"));
    return a;
}
MixRole mix_role_of(const wxString &name)
{
    const wxArrayString a = mix_role_names();
    return name == a[1] ? MixRole::Accent : name == a[2] ? MixRole::Solid : MixRole::Mix;
}
} // namespace

ColorMixBundleDialog::ColorMixBundleDialog(wxWindow *parent, const ColorMixContext &ctx, const Bundle *base, bool edit)
    : DPIDialog(parent, wxID_ANY, edit ? _L("ColorMax Engine - edit a filament set") : _L("ColorMax Engine - new filament set"),
                wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_ctx(ctx)
    , m_edit(edit && base != nullptr)
{
    if (base != nullptr)
        m_base = *base;
    const int em  = wxGetApp().em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);
    auto *intro = new wxStaticText(this, wxID_ANY,
        m_edit ? _L("Change the filaments of this set. Its calibrations are kept; measurements of filaments you remove no longer apply.") :
                 _L("A filament set is the group of spools you mix with: give each one a name and its color (the color of the spool "
                    "as it looks printed). It starts with the spools loaded in your printer. Mark your white spool (used for the white "
                    "core) and a dark one (the frame of the calibration boards). Then calibrate the set for accurate colors."));
    intro->Wrap(90 * em);
    top->Add(intro, 0, wxEXPAND | wxALL, em);

    auto *r1 = new wxBoxSizer(wxHORIZONTAL);
    r1->Add(new wxStaticText(this, wxID_ANY, _L("Name:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_name = new wxTextCtrl(this, wxID_ANY, m_edit ? from_u8(m_base.name) : wxString());
    m_name->SetHint(_L("e.g. Basic PLA set"));
    m_name->SetToolTip(_L("Shown in the filament set list."));
    r1->Add(m_name, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 * em);
    r1->Add(new wxStaticText(this, wxID_ANY, _L("Material:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_material = new wxTextCtrl(this, wxID_ANY, from_u8(m_edit ? m_base.material : std::string("PLA")), wxDefaultPosition, wxSize(8 * em, -1));
    m_material->SetToolTip(_L("For your information (e.g. PLA, PETG). Mix only filaments that print with the same settings."));
    r1->Add(m_material, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(r1, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

    m_grid = new wxGrid(this, wxID_ANY, wxDefaultPosition, wxSize(92 * em, 22 * em));
    m_grid->CreateGrid(0, BC_COUNT);
    m_grid->HideRowLabels();
    m_grid->SetColLabelValue(BC_NAME, _L("Filament"));
    m_grid->SetColLabelValue(BC_COLOR, _L("Color (double-click)"));
    m_grid->SetColLabelValue(BC_TOOL, _L("Tool"));
    m_grid->SetColLabelValue(BC_MIX, _L("Mixing"));
    m_grid->SetColLabelValue(BC_TD, _L("TD (mm)"));
    m_grid->SetColLabelValue(BC_ROLE, _L("Use as"));
    m_grid->SetColSize(BC_NAME, 26 * em);
    m_grid->SetColSize(BC_COLOR, 15 * em);
    m_grid->SetColSize(BC_TOOL, 6 * em);
    m_grid->SetColSize(BC_MIX, 14 * em);
    m_grid->SetColSize(BC_TD, 7 * em);
    m_grid->SetColSize(BC_ROLE, 13 * em);
    m_grid->SetToolTip(_L("Filament: any name. Color: double-click to pick it, or type #RRGGBB (as the spool looks printed; a calibration "
                          "measures it exactly). Tool: where it is usually loaded (0 = not loaded). Mixing: Mix = blended with the others in "
                          "layers; Accent = glitter made for layering (calibrated, but conversions don't use it yet); Solid only = "
                          "printed on its own (silk, marble...). TD: transmission distance in mm (how translucent it "
                          "is; from the filament database, optional - it sets how strongly an uncalibrated filament shows in a mix). Use as: your White (white core) and a dark filament "
                          "(calibration board frames). Look up... fills a row from a filament database."));
    top->Add(m_grid, 1, wxEXPAND | wxALL, em);

    auto *rb = new wxBoxSizer(wxHORIZONTAL);
    auto *add = new wxButton(this, wxID_ANY, _L("Add filament"));
    auto *del = new wxButton(this, wxID_ANY, _L("Remove filament"));
    auto *printer = new wxButton(this, wxID_ANY, _L("Use the printer's spools"));
    printer->SetToolTip(_L("Replace the list with the filaments loaded in the printer: their preset names, colors and tools."));
    auto *lookup = new wxButton(this, wxID_ANY, _L("Look up filament..."));
    lookup->SetToolTip(_L("Find a filament in the public databases (measured colors from filamentcolors.xyz, the SpoolmanDB catalog) and "
                          "fill the selected row with its name, color and TD."));
    rb->Add(add, 0, wxRIGHT, em / 2);
    rb->Add(del, 0, wxRIGHT, em / 2);
    rb->Add(lookup, 0, wxRIGHT, em / 2);
    rb->Add(printer, 0, wxRIGHT, em / 2);
    lookup->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const wxArrayInt rows = m_grid->GetSelectedRows();
        look_up(rows.empty() ? m_grid->GetGridCursorRow() : rows.front());
    });
    top->Add(rb, 0, wxLEFT | wxRIGHT, em);

    auto *pane = new wxCollapsiblePane(this, wxID_ANY, _L("Advanced: striping rules"), wxDefaultPosition, wxDefaultSize,
                                       wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    wxWindow *ap = pane->GetPane();
    auto     *as = new wxBoxSizer(wxHORIZONTAL);
    const RecipeRules rules = m_edit ? m_base.rules : RecipeRules();
    auto spin = [&](const wxString &label, const wxString &tip, int min, int max, int value) {
        auto *t = new wxStaticText(ap, wxID_ANY, label);
        t->SetToolTip(tip);
        as->Add(t, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
        auto *sp = new wxSpinCtrl(ap, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(6 * em, -1), wxSP_ARROW_KEYS, min, max, value);
        sp->SetToolTip(tip);
        as->Add(sp, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 * em);
        return sp;
    };
    m_max_run = spin(_L("Same filament in a row (layers):"), _L("Most consecutive layers of one filament. Lower = less visible striping, fewer mixes."), 1, 8, rules.max_run);
    m_window  = spin(_L("Every filament within (layers):"), _L("Each filament of a mix appears at least once in every this many layers."), 1, 12, rules.recurrence_window);
    m_max_len = spin(_L("Longest pattern (layers):"), _L("Longest repeating layer pattern. Longer = finer ratios, more visible banding."), 1, 12, rules.max_word_length);
    m_max_fil = spin(_L("Filaments per mix:"), _L("Most filaments in one mix (3 is a good balance of colors and tool changes)."), 1, 4, rules.max_filaments);
    ap->SetSizer(as);
    top->Add(pane, 0, wxEXPAND | wxALL, em);
    pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent &) {
        Layout();
        Fit();
    });

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer();
    auto *ok = new wxButton(this, wxID_OK, _L("Save filament set"));
    btns->Add(ok, 0, wxRIGHT, em / 2);
    btns->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0);
    top->Add(btns, 0, wxEXPAND | wxALL, em);
    SetSizer(top);

    if (m_edit) {
        for (const BundleFilament &f : m_base.filaments)
            add_row(f);
    } else
        fill_from_printer();

    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        BundleFilament f;
        f.color   = "#808080";
        add_row(f);
    });
    del->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const wxArrayInt rows = m_grid->GetSelectedRows();
        int r = rows.empty() ? m_grid->GetGridCursorRow() : rows.front();
        if (r >= 0 && r < m_grid->GetNumberRows()) {
            m_grid->DeleteRows(r);
            m_row_ids.erase(m_row_ids.begin() + r);
        }
    });
    printer->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { fill_from_printer(); });
    m_grid->Bind(wxEVT_GRID_CELL_LEFT_DCLICK, [this](wxGridEvent &e) {
        if (e.GetCol() == BC_COLOR)
            pick_color(e.GetRow());
        else
            e.Skip();
    });
    m_grid->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent &e) {
        const int r = e.GetRow();
        if (e.GetCol() == BC_COLOR)
            refresh_color(r);
        // One White and one Black at most.
        if (e.GetCol() == BC_ROLE && !m_grid->GetCellValue(r, BC_ROLE).empty())
            for (int o = 0; o < m_grid->GetNumberRows(); ++o)
                if (o != r && m_grid->GetCellValue(o, BC_ROLE) == m_grid->GetCellValue(r, BC_ROLE))
                    m_grid->SetCellValue(o, BC_ROLE, wxEmptyString);
        e.Skip();
    });
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (save())
            EndModal(wxID_OK);
    });
    Fit();
    CenterOnParent();
}

void ColorMixBundleDialog::on_dpi_changed(const wxRect &) { Layout(); Refresh(); }

wxArrayString ColorMixBundleDialog::role_names() const
{
    wxArrayString r;
    r.Add(wxEmptyString);
    r.Add(_L("White (core)"));
    r.Add(_L("Dark (board frame)"));
    return r;
}

void ColorMixBundleDialog::add_row(const BundleFilament &f)
{
    const int r = m_grid->GetNumberRows();
    m_grid->AppendRows(1);
    m_row_ids.push_back(f.id);
    m_grid->SetCellValue(r, BC_NAME, from_u8(f.name));
    m_grid->SetCellValue(r, BC_COLOR, from_u8(f.color));
    m_grid->SetCellEditor(r, BC_TOOL, new wxGridCellNumberEditor(0, int(std::max(1u, m_ctx.num_physical))));
    m_grid->SetCellValue(r, BC_TOOL, wxString::Format("%u", f.default_tool));
    m_grid->SetCellEditor(r, BC_MIX, new wxGridCellChoiceEditor(mix_role_names()));
    m_grid->SetCellValue(r, BC_MIX, mix_role_names()[int(f.role)]);
    m_grid->SetCellEditor(r, BC_TD, new wxGridCellFloatEditor(-1, 1));
    m_grid->SetCellValue(r, BC_TD, f.td > 0. ? wxString::Format("%.1f", f.td) : wxString());
    const wxArrayString roles = role_names();
    m_grid->SetCellEditor(r, BC_ROLE, new wxGridCellChoiceEditor(roles));
    m_grid->SetCellValue(r, BC_ROLE, f.id == "white" ? roles[1] : f.id == "black" ? roles[2] : wxString());
    refresh_color(r);
}

void ColorMixBundleDialog::refresh_color(int r)
{
    const std::string hex = normalize_hex(into_u8(m_grid->GetCellValue(r, BC_COLOR)));
    wxColour c;
    if (!hex.empty() && c.Set(from_u8(hex))) {
        m_grid->SetCellValue(r, BC_COLOR, from_u8(hex));
        m_grid->SetCellBackgroundColour(r, BC_COLOR, c);
        m_grid->SetCellTextColour(r, BC_COLOR, c.GetLuminance() > 0.5 ? *wxBLACK : *wxWHITE);
    } else {
        m_grid->SetCellBackgroundColour(r, BC_COLOR, *wxWHITE);
        m_grid->SetCellTextColour(r, BC_COLOR, *wxRED);
    }
    m_grid->ForceRefresh();
}

void ColorMixBundleDialog::pick_color(int r)
{
    wxColourData data;
    wxColour     cur;
    if (cur.Set(m_grid->GetCellValue(r, BC_COLOR)))
        data.SetColour(cur);
    data.SetChooseFull(true);
    wxColourDialog dlg(this, &data);
    dlg.SetTitle(_L("Color of the spool as it looks printed"));
    if (dlg.ShowModal() != wxID_OK)
        return;
    const wxColour c = dlg.GetColourData().GetColour();
    m_grid->SetCellValue(r, BC_COLOR, wxString::Format("#%02X%02X%02X", c.Red(), c.Green(), c.Blue()));
    refresh_color(r);
}

void ColorMixBundleDialog::fill_from_printer()
{
    if (m_grid->GetNumberRows() > 0)
        m_grid->DeleteRows(0, m_grid->GetNumberRows());
    m_row_ids.clear();
    const auto &fils = wxGetApp().preset_bundle->extruders_filaments;
    for (unsigned int t = 1; t <= m_ctx.num_physical; ++t) {
        BundleFilament f;
        std::string    name = t - 1 < fils.size() ? fils[t - 1].get_selected_preset_name() : std::string();
        // "Prusament PLA @COREONEINDX HF0.4 - Good" -> "Prusament PLA"
        if (const size_t at = name.find(" @"); at != std::string::npos)
            name = name.substr(0, at);
        f.name         = name.empty() ? "Tool " + std::to_string(t) : name;
        f.color        = t - 1 < m_ctx.physical_colors.size() ? normalize_hex(m_ctx.physical_colors[t - 1]) : std::string();
        f.color        = f.color.empty() ? "#808080" : f.color;
        f.default_tool = t;
        // Names of several spools are often the same preset: number them.
        for (int k = 0; k < m_grid->GetNumberRows(); ++k)
            if (into_u8(m_grid->GetCellValue(k, BC_NAME)) == f.name)
                f.name += " (" + std::to_string(t) + ")";
        add_row(f);
    }
}

void ColorMixBundleDialog::look_up(int r)
{
    ColorMixFilamentLookupDialog dlg(this, r >= 0 && r < m_grid->GetNumberRows() ? m_grid->GetCellValue(r, BC_NAME) : wxString());
    if (dlg.ShowModal() != wxID_OK || dlg.selected() == nullptr)
        return;
    const FilamentRecord &f = *dlg.selected();
    if (r < 0 || r >= m_grid->GetNumberRows()) {
        BundleFilament nf;
        add_row(nf);
        r = m_grid->GetNumberRows() - 1;
    }
    // "Polymaker Panchroma Red" rather than "Panchroma Red" (unless the name says the brand already).
    std::string name = f.name;
    if (!f.manufacturer.empty() && boost::algorithm::to_lower_copy(name).find(boost::algorithm::to_lower_copy(f.manufacturer)) == std::string::npos)
        name = f.manufacturer + " " + name;
    m_grid->SetCellValue(r, BC_NAME, from_u8(name));
    m_grid->SetCellValue(r, BC_COLOR, from_u8(f.color));
    // Glitter / silk / multicolor from the database: solid only (Prusa's ColorMix glitters are accents).
    m_grid->SetCellValue(r, BC_MIX, mix_role_names()[int(f.effect ? MixRole::Solid : MixRole::Mix)]);
    m_grid->SetCellValue(r, BC_TD, f.td > 0. ? wxString::Format("%.1f", f.td) : wxString());
    refresh_color(r);
    m_grid->SetGridCursor(r, BC_NAME);
}

bool ColorMixBundleDialog::save()
{
    auto fail = [this](const wxString &msg) {
        MessageDialog(this, msg, _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
        return false;
    };
    m_grid->SaveEditControlValue();
    Bundle b = m_edit ? m_base : Bundle();
    b.name     = boost::algorithm::trim_copy(into_u8(m_name->GetValue()));
    b.material = boost::algorithm::trim_copy(into_u8(m_material->GetValue()));
    if (!m_edit) {
        std::vector<std::string> taken;
        for (const Bundle &o : m_ctx.bundles)
            taken.push_back(o.id);
        b.id          = make_id(b.name, taken, "filament-set");
        b.description = "Created in ColorMax.";
        b.calibrations.clear();
    }
    b.rules.max_run           = m_max_run->GetValue();
    b.rules.recurrence_window = m_window->GetValue();
    b.rules.max_word_length   = m_max_len->GetValue();
    b.rules.max_filaments     = m_max_fil->GetValue();
    const wxArrayString roles = role_names();
    b.filaments.clear();
    // Ids: "white" / "black" for the marked filaments, the existing id of an edited filament, else one from its name.
    std::vector<std::string> taken = {"white", "black"};
    for (int r = 0; r < m_grid->GetNumberRows(); ++r) {
        const wxString role = m_grid->GetCellValue(r, BC_ROLE);
        if (role.empty() && (size_t(r) < m_row_ids.size()) && !m_row_ids[size_t(r)].empty() && m_row_ids[size_t(r)] != "white" &&
            m_row_ids[size_t(r)] != "black")
            taken.push_back(m_row_ids[size_t(r)]);
    }
    for (int r = 0; r < m_grid->GetNumberRows(); ++r) {
        BundleFilament f;
        f.name  = boost::algorithm::trim_copy(into_u8(m_grid->GetCellValue(r, BC_NAME)));
        if (f.name.empty() && boost::algorithm::trim_copy(into_u8(m_grid->GetCellValue(r, BC_COLOR))).empty())
            continue; // an empty row
        f.color = normalize_hex(into_u8(m_grid->GetCellValue(r, BC_COLOR)));
        long tool = 0;
        m_grid->GetCellValue(r, BC_TOOL).ToLong(&tool);
        f.default_tool = unsigned(std::clamp(tool, 0L, 64L));
        f.role         = mix_role_of(m_grid->GetCellValue(r, BC_MIX));
        double td      = 0.;
        if (m_grid->GetCellValue(r, BC_TD).ToCDouble(&td) || m_grid->GetCellValue(r, BC_TD).ToDouble(&td))
            f.td = std::clamp(td, 0., 100.);
        const wxString role = m_grid->GetCellValue(r, BC_ROLE);
        const std::string old = size_t(r) < m_row_ids.size() ? m_row_ids[size_t(r)] : std::string();
        if (role == roles[1])
            f.id = "white";
        else if (role == roles[2])
            f.id = "black";
        else if (!old.empty() && old != "white" && old != "black")
            f.id = old;
        else {
            f.id = make_id(f.name, taken);
            taken.push_back(f.id);
        }
        if (m_edit)
            if (const int k = m_base.filament_index(f.id); k >= 0)
                f.filament_preset = m_base.filaments[size_t(k)].filament_preset;
        b.filaments.push_back(std::move(f));
    }
    if (const std::string err = check_bundle(b); !err.empty())
        return fail(from_u8(err));
    if (b.filament_index("white") < 0 &&
        MessageDialog(this, _L("No filament is marked as White (core). The white core can then only use an extra spool loaded in a tool "
                               "with no filament of this set. Save anyway?"),
                      _L("ColorMax Engine"), wxYES_NO | wxICON_QUESTION).ShowModal() != wxID_YES)
        return false;
    if (m_edit && !m_base.calibrations.empty()) {
        // Measurements of removed filaments no longer apply.
        std::vector<std::string> gone;
        for (const BundleFilament &f : m_base.filaments)
            if (b.filament_index(f.id) < 0)
                gone.push_back(f.name);
        if (!gone.empty() &&
            MessageDialog(this,
                          format_wxstr(_L("The calibrations of this set measured %1%. Measurements with removed filaments no longer "
                                          "apply. Save anyway?"),
                                       from_u8(boost::algorithm::join(gone, ", "))),
                          _L("ColorMax Engine"), wxYES_NO | wxICON_QUESTION).ShowModal() != wxID_YES)
            return false;
    }
    const std::string path = m_edit && m_base.user_bundle && !m_base.source_path.empty() ?
        m_base.source_path :
        (boost::filesystem::path(ColorMixContext::user_bundle_dir()) / user_bundle_file_name(b)).string();
    boost::system::error_code ec;
    boost::filesystem::create_directories(ColorMixContext::user_bundle_dir(), ec);
    std::string err;
    if (!save_bundle(b, path, &err))
        return fail(from_u8(err));
    m_saved_id = b.id;
    return true;
}

// ---------------------------------------------------------------- photo dialog

bool ColorMixPhotoDialog::scanner() const { return m_source != nullptr && m_source->GetSelection() == 0; }

ColorMixPhotoDialog::ColorMixPhotoDialog(wxWindow *parent, int num_boards, const BoardLayout &layout, bool scanner, int initial_board)
    : DPIDialog(parent, wxID_ANY, _L("ColorMax Engine - read a calibration board from a scan or photo"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_layout(layout)
{
    wxInitAllImageHandlers();
    const int em  = wxGetApp().em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);
    auto *steps = new wxStaticText(this, wxID_ANY,
        _L("1. Open the scan (or photo) of one board.   2. Pick the board: count the dots on its top edge.   3. Click the board's four "
           "corners: first the corner next to the dots, then clockwise.   4. Check that the yellow cells sit on the swatches and press "
           "Read colors.   5. Use these values. (Instructions... explains how to scan.)"));
    steps->Wrap(110 * em);
    top->Add(steps, 0, wxEXPAND | wxALL, em);
    auto *row0 = new wxBoxSizer(wxHORIZONTAL);
    row0->Add(new wxStaticText(this, wxID_ANY, _L("Image from:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_source = new wxChoice(this, wxID_ANY);
    m_source->Append(_L("Flatbed scanner (recommended): board in a hole in white paper"));
    m_source->Append(_L("Phone or camera: board on white paper"));
    m_source->SetSelection(scanner ? 0 : 1);
    m_source->SetToolTip(_L("How the image was taken: the white paper around the board is found differently."));
    row0->Add(m_source, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    auto *howto = new wxButton(this, wxID_ANY, _L("Instructions..."));
    row0->Add(howto, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(row0, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *open = new wxButton(this, wxID_ANY, _L("Open scan or photo..."));
    row->Add(open, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    row->Add(new wxStaticText(this, wxID_ANY, _L("Board:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_board_choice = new wxChoice(this, wxID_ANY);
    for (int b = 0; b < num_boards; ++b)
        m_board_choice->Append(format_wxstr(_L("%1% (%2% dots)"), b + 1, b + 1));
    if (num_boards > 0)
        m_board_choice->SetSelection(std::clamp(initial_board, 0, num_boards - 1));
    row->Add(m_board_choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    row->Add(new wxStaticText(this, wxID_ANY, _L("Paper reflectance:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_paper = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(7 * em, -1), wxSP_ARROW_KEYS, 0.6, 0.98, 0.88, 0.01);
    m_paper->SetToolTip(_L("Plain office paper reflects about 88% of the light. Use the value printed on a reference white card if you use one."));
    row->Add(m_paper, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    auto *clear = new wxButton(this, wxID_ANY, _L("Clear corners"));
    row->Add(clear, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

    m_canvas = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(90 * em, 50 * em), wxFULL_REPAINT_ON_RESIZE);
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
    top->Add(m_canvas, 1, wxEXPAND | wxALL, em);
    m_status = new wxStaticText(this, wxID_ANY, _L("Open the scan or photo of a board."));
    top->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    auto *read = new wxButton(this, wxID_ANY, _L("Read colors"));
    m_ok       = new wxButton(this, wxID_OK, _L("Use these values"));
    m_ok->SetToolTip(_L("Puts the colors read from this board into the calibration's table (and its plate file)."));
    read->SetToolTip(_L("Reads every swatch of the board, using the white paper around it as the reference."));
    m_ok->Enable(false);
    btns->AddStretchSpacer();
    btns->Add(read, 0, wxRIGHT, em / 2);
    btns->Add(m_ok, 0, wxRIGHT, em / 2);
    btns->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0);
    top->Add(btns, 0, wxEXPAND | wxALL, em);
    SetSizer(top);
    Fit();
    CenterOnParent();

    open->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { open_photo(); });
    clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_corners.clear(); m_cells.clear(); m_ok->Enable(false); m_canvas->Refresh(); });
    read->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { this->read(); });
    m_board_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { m_cells.clear(); m_ok->Enable(false); });
    m_paper->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxCommandEvent &) { m_cells.clear(); m_ok->Enable(false); });
    m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { m_cells.clear(); m_ok->Enable(false); m_canvas->Refresh(); });
    howto->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        bool sc = this->scanner();
        show_photo_instructions(this, sc, false);
        m_source->SetSelection(sc ? 1 : 0);
        m_cells.clear();
        m_ok->Enable(false);
        m_canvas->Refresh();
    });
    m_canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent &) { on_paint(); });
    m_canvas->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) { on_click(e.GetPosition()); });
}

ColorMixPhotoDialog::~ColorMixPhotoDialog()
{
    delete m_image;
    delete m_preview;
}

void ColorMixPhotoDialog::on_dpi_changed(const wxRect &) { Layout(); Refresh(); }

void ColorMixPhotoDialog::open_photo()
{
    wxFileDialog dlg(this, _L("Open photo"), wxEmptyString, wxEmptyString, "Images (*.jpg;*.jpeg;*.png;*.tif;*.tiff;*.bmp)|*.jpg;*.jpeg;*.png;*.tif;*.tiff;*.bmp",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    auto *img = new wxImage();
    // TIFF (many scanners save nothing else) is decoded here: wxWidgets is built without it.
    std::vector<unsigned char> bytes;
    {
        boost::nowide::ifstream in(into_u8(dlg.GetPath()), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (is_tiff(bytes.data(), bytes.size())) {
        PhotoImage  pi;
        std::string err;
        const bool  ok = decode_tiff(bytes.data(), bytes.size(), pi, &err, [](const std::vector<unsigned char> &jpeg, PhotoImage &out) {
            wxMemoryInputStream stream(jpeg.data(), jpeg.size());
            wxImage             part;
            if (!part.LoadFile(stream, wxBITMAP_TYPE_JPEG) || !part.IsOk())
                return false;
            out.width  = part.GetWidth();
            out.height = part.GetHeight();
            out.rgb.assign(part.GetData(), part.GetData() + size_t(out.width) * size_t(out.height) * 3);
            return true;
        });
        if (!ok) {
            delete img;
            m_status->SetLabel(from_u8(err));
            return;
        }
        unsigned char *data = static_cast<unsigned char *>(malloc(pi.rgb.size()));
        std::copy(pi.rgb.begin(), pi.rgb.end(), data);
        img->Create(pi.width, pi.height, data); // takes ownership
    } else if (!img->LoadFile(dlg.GetPath()) || !img->IsOk()) {
        delete img;
        m_status->SetLabel(_L("The photo could not be opened."));
        return;
    }
    delete m_image;
    delete m_preview;
    m_image   = img;
    m_preview = nullptr;
    m_corners.clear();
    m_cells.clear();
    m_ok->Enable(false);
    m_status->SetLabel(format_wxstr(_L("%1% x %2% pixels. Click the corner next to the marker dots, then the other corners clockwise."),
                                    m_image->GetWidth(), m_image->GetHeight()));
    m_canvas->Refresh();
}

void ColorMixPhotoDialog::on_paint()
{
    wxAutoBufferedPaintDC dc(m_canvas);
    dc.SetBackground(wxBrush(wxColour(60, 60, 60)));
    dc.Clear();
    if (m_image == nullptr)
        return;
    const wxSize sz = m_canvas->GetClientSize();
    m_scale = std::min(double(sz.x) / m_image->GetWidth(), double(sz.y) / m_image->GetHeight());
    const int w = std::max(1, int(m_image->GetWidth() * m_scale)), h = std::max(1, int(m_image->GetHeight() * m_scale));
    m_off_x = (sz.x - w) / 2.;
    m_off_y = (sz.y - h) / 2.;
    if (m_preview == nullptr || m_preview->GetWidth() != w || m_preview->GetHeight() != h) {
        delete m_preview;
        m_preview = new wxBitmap(m_image->Scale(w, h, wxIMAGE_QUALITY_BILINEAR));
    }
    dc.DrawBitmap(*m_preview, int(m_off_x), int(m_off_y));
    auto to_panel = [&](const std::array<double, 2> &p) { return wxPoint(int(m_off_x + p[0] * m_scale), int(m_off_y + p[1] * m_scale)); };
    const int em = wxGetApp().em_unit();
    const int r_marker = std::max(3, int(0.6 * em)), text_off = std::max(4, int(0.8 * em));
    dc.SetPen(wxPen(wxColour(255, 40, 40), 2));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    for (size_t i = 0; i < m_corners.size(); ++i) {
        const wxPoint p = to_panel(m_corners[i]);
        dc.DrawCircle(p, r_marker);
        dc.SetTextForeground(wxColour(255, 40, 40));
        dc.DrawText(wxString::Format("%d", int(i + 1)), p + wxPoint(text_off, -text_off));
    }
    if (m_corners.size() == 4) {
        BoardCorners c = {m_corners[0], m_corners[1], m_corners[2], m_corners[3]};
        auto quad = [&](const std::array<double, 4> &r, const wxColour &col) {
            dc.SetPen(wxPen(col, 1));
            wxPoint pts[4] = {to_panel(board_to_image(c, m_layout, r[0], r[3])), to_panel(board_to_image(c, m_layout, r[2], r[3])),
                              to_panel(board_to_image(c, m_layout, r[2], r[1])), to_panel(board_to_image(c, m_layout, r[0], r[1]))};
            dc.DrawPolygon(4, pts);
        };
        quad({0., 0., m_layout.width(), m_layout.height()}, wxColour(255, 40, 40));
        for (int r = 0; r < m_layout.rows; ++r)
            for (int col = 0; col < m_layout.cols; ++col)
                quad(m_layout.cell_rect(r, col), wxColour(255, 230, 40));
    }
}

void ColorMixPhotoDialog::on_click(const wxPoint &pt)
{
    if (m_image == nullptr || m_corners.size() >= 4)
        return;
    const double u = (pt.x - m_off_x) / m_scale, v = (pt.y - m_off_y) / m_scale;
    if (u < 0 || v < 0 || u >= m_image->GetWidth() || v >= m_image->GetHeight())
        return;
    m_corners.push_back({u, v});
    m_status->SetLabel(m_corners.size() < 4 ? format_wxstr(_L("Corner %1% of 4 set."), m_corners.size()) :
                                               _L("Check that the yellow cells sit on the swatches, then press Read colors."));
    m_canvas->Refresh();
}

void ColorMixPhotoDialog::read()
{
    if (m_image == nullptr || m_corners.size() != 4) {
        m_status->SetLabel(_L("Open a scan or photo and click the board's four corners first."));
        return;
    }
    PhotoImage img;
    img.width  = m_image->GetWidth();
    img.height = m_image->GetHeight();
    img.rgb.assign(m_image->GetData(), m_image->GetData() + size_t(img.width) * size_t(img.height) * 3);
    PhotoReadOptions opt;
    opt.paper_reflectance = m_paper->GetValue();
    opt.scanner           = scanner();
    try {
        wxBusyCursor        wait;
        const PhotoReading  r = read_board_photo(img, {m_corners[0], m_corners[1], m_corners[2], m_corners[3]}, m_layout, opt);
        m_cells               = r.cells;
        // A cell outside the photo, or one whose pixels disagree wildly (glare, shadow, a misplaced
        // corner), is not a measurement: it stays empty rather than being saved as a wrong color.
        constexpr double MAX_SPREAD = 12.;
        m_cells_ok.assign(r.cells.size(), false);
        for (size_t i = 0; i < r.cells.size(); ++i)
            m_cells_ok[i] = i < r.valid.size() && r.valid[i] && i < r.spread.size() && r.spread[i] <= MAX_SPREAD;
        m_board               = std::max(0, m_board_choice->GetSelection());
        wxString msg =
            format_wxstr(_L("Read %1% swatches. White reference: %2% points of the paper around the board; light varies %3%x across the board."),
                         r.cells.size(), r.paper_samples, wxString::Format("%.2f", r.light_falloff));
        if (!r.warning.empty())
            msg += "\n" + _L("Check:") + " " + from_u8(r.warning);
        msg += "\n" + _L("A scan or photo is less accurate than a colorimeter (expect a few dE); measured swatches still improve the predictions a lot.");
        m_status->SetLabel(msg);
        m_status->Wrap(88 * wxGetApp().em_unit());
        m_ok->Enable(true);
        Layout();
    } catch (const std::exception &e) {
        m_status->SetLabel(from_u8(e.what()));
        m_ok->Enable(false);
    }
}

void ColorMixCalibrationDialog::save_calibration()
{
    const Bundle *b = bundle();
    if (b == nullptr)
        return;
    // The rows must belong to the plan that was printed.
    if (const nlohmann::json pj = json_object(m_plater->model().colormix.plate_json); pj.contains("plan_id") && pj["plan_id"].is_string()) {
        const std::string printed = pj["plan_id"].get<std::string>(), now = calibration_plan_id(m_plan);
        if (printed != now &&
            MessageDialog(this, format_wxstr(_L("The plate on the plater was made for plan %1%, but the list now shows plan %2% (the settings "
                                                "differ). Measurements would be saved against the wrong swatches.\n\nSave anyway?"),
                                             from_u8(printed), from_u8(now)),
                          _L("ColorMax Engine"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING).ShowModal() != wxID_YES)
            return;
    }
    std::vector<Lab>  measured(m_plan.size());
    std::vector<bool> has(m_plan.size(), false);
    int bad = 0, count = 0;
    for (size_t i = 0; i < m_plan.size(); ++i) {
        const std::string v = into_u8(m_grid->GetCellValue(int(i), 2));
        if (boost::algorithm::trim_copy(v).empty())
            continue;
        if (parse_measurement(v, measured[i])) {
            has[i] = true;
            ++count;
        } else
            ++bad;
    }
    if (bad > 0) {
        MessageDialog(this, format_wxstr(_L("%1% measurements could not be read. Use \"L a b\" (e.g. 62.1 -21.4 -30.2) or #RRGGBB."), bad),
                      _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    if (count < 3) {
        MessageDialog(this, _L("Enter at least the pure filaments and a few mixes."), _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    // A filament that didn't print (or isn't the bundle's) would teach the calibration wrong colors
    // for every recipe with it.
    for (const FilamentCheck &c : check_filaments(*b, m_plan, measured, has, schedule())) {
        const wxString name = from_u8(b->filaments[size_t(c.filament)].name);
        const int      rc   = MessageDialog(this,
            format_wxstr(_L("The %1% swatches with %2% read far from their expected colors (median %3% dE; the other swatches %4% dE).\n\n"
                            "%2% probably didn't print (empty spool, clogged nozzle, wrong tool: the other layers then fill the gaps and the "
                            "swatch shows the mix without it), or the spool isn't %2%.\n\n"
                            "Leave these swatches out? Their values are cleared, so \"only swatches without a measurement\" reprints exactly "
                            "them once the problem is fixed."),
                         c.swatches.size(), name, wxString::Format("%.0f", c.median_with), wxString::Format("%.0f", c.median_without)),
            _L("ColorMax Engine"), wxYES_NO | wxCANCEL | wxICON_WARNING).ShowModal();
        if (rc == wxID_CANCEL)
            return;
        if (rc == wxID_YES)
            for (int i : c.swatches) {
                if (has[size_t(i)])
                    --count;
                has[size_t(i)] = false;
                m_grid->SetCellValue(i, 2, wxEmptyString);
            }
    }
    store_measurements();
    if (count < 3) {
        MessageDialog(this, _L("Too few measurements are left to save a calibration."), _L("ColorMax Engine"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    const std::string printer = wxGetApp().preset_bundle->printers.get_edited_preset().name;
    // The layer schedule the plate was printed with (recorded with the plate), not the dialog's.
    LayerSchedule  printed = schedule();
    nlohmann::json pj      = json_object(m_plater->model().colormix.plate_json);
    if (pj.contains("schedule_heights") && pj["schedule_heights"].is_array() && !pj["schedule_heights"].empty()) {
        printed.heights            = pj["schedule_heights"].get<std::vector<double>>();
        printed.first_layer_height = pj.value("first_layer_height", printed.first_layer_height);
    } else {
        wxString heights;
        for (double h : printed.heights)
            heights += (heights.empty() ? "" : ", ") + wxString::Format("%.3g", h);
        if (MessageDialog(this, format_wxstr(_L("The plate isn't on the plater, so the layer heights it was printed with are unknown. "
                                                "Save the measurements as printed with %1% mm layers (first layer %2% mm)?"),
                                             heights, wxString::Format("%.3g", printed.first_layer_height)),
                          _L("ColorMax Engine"), wxYES_NO | wxICON_QUESTION).ShowModal() != wxID_YES)
            return;
    }
    Calibration cal = make_calibration(*b, m_plan, measured, has, printed, into_u8(m_cal_name->GetValue()), printer);
    cal.white_core      = pj.contains("core_tool") ? pj.value("core_tool", 0) > 0 : m_core.tool() > 0;
    cal.wall_depth      = pj.value("wall_depth", SliceSettings().wall_depth);
    cal.dual_wall_width = pj.value("dual_wall_width", SliceSettings().dual_wall_width);
    char date[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
    cal.date = date;

    cal.plate_file = m_plate_file;
    Bundle user = *b;
    wxString merged_note;
    if (auto it = std::find_if(user.calibrations.begin(), user.calibrations.end(), [&](const Calibration &c) { return c.id == cal.id; });
        it != user.calibrations.end()) {
        // The same name: update that calibration (its other measurements are kept), unless it was
        // printed with other layer heights.
        Calibration merged = cal;
        int         kept   = 0;
        if (merge_calibration(*it, merged, &kept)) {
            if (MessageDialog(this,
                              format_wxstr(_L("Update the calibration \"%1%\"?\n\nThe %2% measurements here replace or add to it; its %3% "
                                              "other measurements are kept.\n\nChoose No and give this one another name to keep both."),
                                           from_u8(cal.id), count, kept),
                              _L("ColorMax Engine"), wxYES_NO | wxICON_QUESTION).ShowModal() != wxID_YES)
                return;
            cal = merged;
            count += kept;
            merged_note = format_wxstr(_L(" (updated: %1% measurements kept from before)"), kept);
        } else if (MessageDialog(this,
                                 format_wxstr(_L("A calibration named \"%1%\" exists already, printed with other layer heights. Replace it?\n\n"
                                                 "Choose No and give this one another name to keep both."),
                                              from_u8(cal.id)),
                                 _L("ColorMax Engine"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal() != wxID_YES)
            return;
    }
    user.calibrations.erase(std::remove_if(user.calibrations.begin(), user.calibrations.end(),
                                           [&](const Calibration &c) { return c.id == cal.id; }),
                            user.calibrations.end());
    user.calibrations.push_back(cal);
    // A user bundle is saved back into its own file (another file with the same id could win on the
    // next start); a system bundle gets a user copy.
    const std::string path = b->user_bundle && !b->source_path.empty() ?
        b->source_path :
        (boost::filesystem::path(ColorMixContext::user_bundle_dir()) / user_bundle_file_name(user)).string();
    std::string err;
    if (!save_bundle(user, path, &err)) {
        MessageDialog(this, from_u8(err), _L("ColorMax Engine"), wxOK | wxICON_ERROR).ShowModal();
        return;
    }
    // The saved file overrides the bundle from now on: a second calibration saved in this session keeps the first.
    user.user_bundle = true;
    user.source_path = path;
    if (const int sel = m_bundle_choice->GetSelection(); sel >= 0 && size_t(sel) < m_ctx.bundles.size()) {
        m_ctx.bundles[size_t(sel)] = user;
        m_bundle_choice->SetString(unsigned(sel), bundle_label(user));
    }
    Predictor p(user, &user.calibrations.back(), schedule());
    MessageDialog(this, format_wxstr(_L("Saved calibration \"%1%\" (%2% swatches)%3% to\n%4%\n\n%5%"), cal.id, count, merged_note, path,
                                     p.report().summary()),
                  _L("ColorMax Engine"), wxOK | wxICON_INFORMATION).ShowModal();
    m_status->SetLabel(from_u8(p.report().summary()));
}

} // namespace Slic3r::GUI
