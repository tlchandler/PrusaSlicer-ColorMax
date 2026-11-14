///|/ ColorMax engine - GUI: convert a colored model, calibrate a filament bundle.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <map>
#include <vector>

#include <wx/panel.h>

#include "GUI_Utils.hpp"
#include <wx/bitmap.h>
#include "libslic3r/Feature/ColorMix/FilamentDb.hpp"
#include "libslic3r/Feature/ColorMix/Gamut.hpp"
#include "libslic3r/Feature/ColorMix/Calibration.hpp"
#include "libslic3r/Feature/ColorMix/Converter.hpp"
#include "libslic3r/Feature/ColorMix/ModelConvert.hpp"
#include "libslic3r/Feature/ColorMix/ModelData.hpp"
#include <functional>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxFlexGridSizer;
class wxGrid;
class wxImageList;
class wxListCtrl;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;
class wxTextCtrl;
class wxImage;
class wxBitmap;
class wxSizer;

namespace Slic3r::GUI {

class Plater;

// Which tool holds each filament of a bundle.
class ColorMixToolPanel : public wxPanel
{
public:
    ColorMixToolPanel(wxWindow *parent);
    void set_bundle(const ColorMix::Bundle &bundle, const std::vector<unsigned int> &tool_of_filament, unsigned int num_physical);
    std::vector<unsigned int> tool_map() const;
    // Called when a tool changes.
    std::function<void()> on_change;

private:
    wxFlexGridSizer         *m_grid{nullptr};
    std::vector<wxChoice *>   m_tools;   // per filament: "Not loaded", "Tool 1", ...
};

// Filament bundles, settings helpers shared by both dialogs.
struct ColorMixContext
{
    std::vector<ColorMix::Bundle> bundles;
    std::vector<std::string>      load_errors;
    unsigned int                  num_physical{1};
    std::vector<std::string>      physical_colors;   // per physical extruder
    double                        layer_height{0.1};
    double                        first_layer_height{0.2};
    // The printer's layer height limits (all tools).
    double                        min_layer_height{0.};
    double                        max_layer_height{0.};

    static ColorMixContext load(Plater *plater);
    static std::string     system_bundle_dir();
    static std::string     user_bundle_dir();
    std::vector<unsigned int> default_tools(const ColorMix::Bundle &b) const;
};

// "White core" checkbox + tool choice. The core may only print from the tool holding the bundle's
// White or from a tool that holds none of the bundle's filaments (an extra spool).
class ColorMixCorePicker
{
public:
    void create(wxWindow *parent, wxSizer *sizer, const wxString &label, const wxString &tooltip, int em);
    // Refills the tool list for this bundle and tool map. Selects `preferred` if allowed, else keeps
    // following White / the previous tool. `init_check`: checkbox on iff `preferred` is allowed.
    void refill(const ColorMix::Bundle &bundle, const std::vector<unsigned int> &tool_of_filament, unsigned int num_physical,
                unsigned int preferred, bool init_check);
    void         set_checked(bool on);
    void         enable(bool on);
    unsigned int tool() const;          // 0 = no core
    unsigned int selected_tool() const; // even when the checkbox is off
    std::function<void()> on_change;

private:
    wxCheckBox               *m_check{nullptr};
    wxChoice                 *m_choice{nullptr};
    std::vector<unsigned int> m_allowed;
    unsigned int              m_white_tool{0};
    bool                      m_white_selected{false};
};

// Parses "0.12, 0.10, 0.08"; empty on error.
std::vector<double> parse_layer_schedule(const std::string &text);

class ColorMixDialog : public DPIDialog
{
public:
    ColorMixDialog(wxWindow *parent, Plater *plater);
    ~ColorMixDialog() override;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void build();
    void load_settings_from_model();
    void on_bundle_changed();
    void update_calibration_info();
    void invalidate();
    // All settings that a preview depends on, to tell whether the preview shown is still current.
    std::string settings_key() const;
    bool        preview_current() const;
    std::string m_preview_key;   // settings_key() of the last preview
    // `interactive`: the user pressed Compute (or Apply) - questions may be asked and the model changed.
    bool compute(bool interactive = true);
    bool m_model_changed{false};
    // Settings of the last Apply in any project (app config): the defaults of a new project.
    std::string m_last_settings;
    wxString m_touch_up_note;
    size_t   m_beyond_tools{0};
public:
    // The dialog changed the model (e.g. kept paint touch-ups) even if it is closed without Apply.
    bool model_changed() const { return m_model_changed; }
private:
    void apply();
    // After Preview: tells which loaded spools no color of the model uses.
    void report_unused_spools();
    void revert();
    // Reloads the filament sets (after one was created, edited or calibrated) and selects `select_id`.
    void reload_bundles(const std::string &select_id);
    const ColorMix::Bundle      *bundle() const;
    const ColorMix::Calibration *calibration() const;
    ColorMix::LayerSchedule      schedule() const;
    ColorMix::SliceSettings      slice_settings() const;

    Plater                 *m_plater{nullptr};
    ColorMixContext         m_ctx;
    wxChoice               *m_bundle_choice{nullptr};
    wxChoice               *m_cal_choice{nullptr};
    wxStaticText           *m_cal_info{nullptr};
    ColorMixToolPanel      *m_tools{nullptr};
    wxCheckBox             *m_use_blends{nullptr};
    wxCheckBox             *m_keep_distinct{nullptr};
    wxCheckBox             *m_families{nullptr};
    wxCheckBox             *m_area{nullptr};
    ColorMixCorePicker      m_core;
    wxSpinCtrlDouble       *m_core_depth{nullptr};
    wxCheckBox             *m_top{nullptr};
    wxSpinCtrlDouble       *m_top_cell{nullptr};
    wxSpinCtrl             *m_top_layers{nullptr};
    wxCheckBox             *m_varlayer{nullptr};
    wxCheckBox             *m_dual{nullptr};
    wxSpinCtrlDouble       *m_dual_width{nullptr};
    wxSpinCtrlDouble       *m_patch_band{nullptr};
    wxStaticText           *m_patch_angle{nullptr};
    wxCheckBox             *m_cleanup{nullptr};
    wxCheckBox             *m_smooth{nullptr};
    wxSpinCtrlDouble       *m_min_run{nullptr};
    wxSpinCtrlDouble       *m_min_island{nullptr};
    wxSpinCtrlDouble       *m_change_weight{nullptr};
    void                    update_patch_angle();
    wxTextCtrl             *m_schedule{nullptr};
    wxListCtrl             *m_list{nullptr};
    wxImageList            *m_images{nullptr};
    wxSize                  m_swatch_size;      // of the swatches in m_images
    wxStaticText           *m_summary{nullptr};
    wxStaticText           *m_preview_note{nullptr};
    wxButton               *m_apply{nullptr};

    std::unique_ptr<ColorMix::Predictor>  m_predictor;
    std::vector<ColorMix::SourceColor>     m_sources;
    std::optional<ColorMix::ConversionResult> m_result;
};

// The colors a filament set can print: a chart of screen colors (hue x lightness, plus grays), each with the
// closest printable color, and the model's colors placed on it.
class ColorMixGamutDialog : public DPIDialog
{
public:
    ColorMixGamutDialog(wxWindow *parent, const ColorMix::Bundle &bundle, int calibration, std::vector<unsigned int> tools,
                        const ColorMix::LayerSchedule &schedule, bool use_blends, bool dual_walls,
                        std::vector<std::pair<ColorMix::Lab, std::string>> model_colors);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void     recompute();
    void     render();
    void     update_tooltip(const wxPoint &pos);
    wxString caption() const;

    ColorMix::Bundle                                   m_bundle;
    std::vector<unsigned int>                          m_loaded_tools;
    ColorMix::LayerSchedule                            m_schedule;
    bool                                               m_use_blends{true};
    std::vector<std::pair<ColorMix::Lab, std::string>> m_model_colors;
    ColorMix::GamutChart                               m_chart;
    wxChoice                                          *m_spools{nullptr};
    wxChoice                                          *m_cal{nullptr};
    wxChoice                                          *m_saturation{nullptr};
    wxCheckBox                                        *m_dual{nullptr};
    wxCheckBox                                        *m_show_model{nullptr};
    wxWindow                                          *m_canvas{nullptr};
    wxStaticText                                      *m_summary{nullptr};
    wxBitmap                                           m_bitmap;
    int                                                m_pitch{30};      // cell pitch in pixels
    // The strip of model colors under the chart (farthest first): layout, for the tooltips.
    std::vector<int>                                   m_strip_order;
    int                                                m_strip_top{0}, m_strip_pitch{0}, m_strip_per_row{1};
    wxString                                           m_tip;
};

class ColorMixCalibrationDialog : public DPIDialog
{
public:
    ColorMixCalibrationDialog(wxWindow *parent, Plater *plater, const std::string &bundle_id,
                              const std::vector<unsigned int> &tool_map);
    // A calibration plate or slope test replaced the project on the plater.
    bool model_replaced() const { return m_model_replaced; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void rebuild_plan();
    // A plate file's own swatch list: used while the settings are the plate's, so a plate stays readable
    // when the planner changes (e.g. a newer ColorMax plans different swatches for the same settings).
    std::vector<ColorMix::SwatchSpec> m_fixed_plan;
    std::string                       m_fixed_key;
    std::string                       plan_inputs_key() const;
    // Warning when the plan doesn't fit PrusaSlicer's painting colors.
    wxString                     plan_cap_message() const;
    ColorMix::PlanReport         m_plan_report;
    void create_plate();
    void create_slope_test();
    void save_calibration();
    void load_csv();
    void read_photo();
    bool m_photo_guide_shown{false};
    // Plan rows printed on the plate (plate order); empty = the whole plan.
    std::vector<int> m_plate_rows;
    // Measurements restored with the plate settings, applied when the plan is rebuilt (plan row -> value).
    std::map<int, std::string> m_pending_measured;
    wxCheckBox             *m_only_missing{nullptr};
    // Position of plan row `i` on the plate (plate order), -1 = not on the plate.
    int                     plate_position(size_t i) const;
    int                     plate_swatch_count() const { return m_plate_rows.empty() ? int(m_plan.size()) : int(m_plate_rows.size()); }
    // Keeps the measurements with the plate (project and plate file), so a later session, e.g. after
    // printing the missing swatches, starts from them.
    void                    store_measurements();
    // Plate file: the plate's settings, its swatch list and the measurements entered so far. Saved when
    // the plate is created and opened again to read photos; while one is open the settings are locked,
    // so photos are always read with the settings the plate was made with.
    std::string             m_plate_file;
    wxStaticText           *m_plate_file_label{nullptr};
    wxButton               *m_plate_file_close{nullptr};
    // Restores the controls from a plate settings JSON (ModelData::plate_json); false if it isn't for this bundle.
    bool                    apply_plate_settings(const std::string &json);
    // `quiet`: no message when the file can't be used (opening the last file automatically).
    bool                    open_plate_file(const std::string &path, bool quiet = false);
    wxStaticText           *m_progress{nullptr};
    // Plan size and how many swatches are measured, per board.
    void                    update_progress();
    // Newest calibration of the selected filament set printed with the current layer heights: its name is
    // the default, so measuring missing swatches updates it.
    const ColorMix::Calibration *matching_calibration() const;
    void                    update_default_name();
    bool                    m_name_from_plate{false};
    bool                    write_plate_file(const std::string &path);
    // Asks where to save the plate file and writes it; true when saved.
    bool                    ask_save_plate_file();
    void                    set_plate_file(const std::string &path);
    void                    lock_settings(bool lock);
    void save_legend();
    const ColorMix::Bundle *bundle() const;
    ColorMix::LayerSchedule schedule() const;
    double                  bed_depth() const;
    Vec2d                   bed_center() const;
    void                    update_core_default();
    // The typed layer heights to apply to the plate: false (after a message) when the text is not valid.
    // `out` stays empty when the plate prints with the profile's uniform layer height.
    bool                    checked_schedule(std::vector<double> &out);
    // Parameters of the plate / slope test (a JSON object for ModelData::plate_json).
    std::string             plate_settings_json(const std::string &kind, const std::vector<unsigned int> &tools) const;
    // Restores the controls from the plate on the plater; returns a status message, empty if nothing was restored.
    wxString                restore_plate_settings();
    void                    after_model_replaced();

    Plater             *m_plater{nullptr};
    ColorMixContext     m_ctx;
    wxChoice           *m_bundle_choice{nullptr};
    ColorMixToolPanel  *m_tools{nullptr};
    wxTextCtrl         *m_schedule{nullptr};
    wxSpinCtrl         *m_max_len{nullptr};
    wxSpinCtrl         *m_held_out{nullptr};
    wxChoice           *m_dual{nullptr};
    ColorMixCorePicker  m_core;
    bool                m_core_touched{false};
    wxGrid             *m_grid{nullptr};
    wxTextCtrl         *m_cal_name{nullptr};
    wxStaticText       *m_status{nullptr};
    std::vector<ColorMix::SwatchSpec> m_plan;
    ColorMix::BoardLayout             m_layout;
    std::string                       m_legend;
    wxButton                         *m_legend_btn{nullptr};
    bool                              m_model_replaced{false};
    std::vector<bool>                 m_boards_read;      // boards read from photos in this session
    int                               m_last_board{-1};
};

// Searches the public filament databases (filamentcolors.xyz measured swatches, SpoolmanDB catalog),
// downloaded once into the data folder.
class ColorMixFilamentLookupDialog : public DPIDialog
{
public:
    ColorMixFilamentLookupDialog(wxWindow *parent, const wxString &query);
    ~ColorMixFilamentLookupDialog() override;
    const ColorMix::FilamentRecord *selected() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void refill();
    bool download();
    std::vector<ColorMix::FilamentRecord> m_records;
    std::vector<size_t>                   m_hits;
    std::string                           m_downloaded;
    wxTextCtrl                           *m_search{nullptr};
    wxListCtrl                           *m_list{nullptr};
    wxImageList                          *m_images{nullptr};
    wxSize                                m_swatch;
    wxStaticText                         *m_info{nullptr};
    wxButton                             *m_ok{nullptr};
    int                                   m_sel{-1};
};

// Creates a filament set (bundle) or edits one: names, colors, tools, which filaments mix, which one
// is the White (core) and the dark one (board frames), and the striping rules. Saved as a user bundle.
class ColorMixBundleDialog : public DPIDialog
{
public:
    // `base`: the set to edit (`edit`), or nullptr for a new set (it starts with the printer's spools).
    ColorMixBundleDialog(wxWindow *parent, const ColorMixContext &ctx, const ColorMix::Bundle *base, bool edit);
    // Id of the saved set (after wxID_OK).
    const std::string &saved_id() const { return m_saved_id; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void          add_row(const ColorMix::BundleFilament &f);
    void          refresh_color(int row);
    void          pick_color(int row);
    void          fill_from_printer();
    void          look_up(int row);
    bool          save();
    wxArrayString role_names() const;

    const ColorMixContext   &m_ctx;
    ColorMix::Bundle         m_base;
    bool                     m_edit{false};
    std::vector<std::string> m_row_ids; // filament id of each row ("" = new)
    std::string              m_saved_id;
    wxTextCtrl              *m_name{nullptr};
    wxTextCtrl              *m_material{nullptr};
    wxGrid                  *m_grid{nullptr};
    wxSpinCtrl              *m_max_run{nullptr};
    wxSpinCtrl              *m_window{nullptr};
    wxSpinCtrl              *m_max_len{nullptr};
    wxSpinCtrl              *m_max_fil{nullptr};
};

// Reads one calibration board from a photo: the user clicks the board's corners.
class ColorMixPhotoDialog : public DPIDialog
{
public:
    ColorMixPhotoDialog(wxWindow *parent, int num_boards, const ColorMix::BoardLayout &layout, bool scanner, int initial_board = 0);
    bool                                 scanner() const;
    int                                  board() const { return m_board; }
    const std::vector<ColorMix::Lab>    &cells() const { return m_cells; }
    // Per cell: read reliably (inside the photo, uniform enough); unreliable cells must stay empty.
    const std::vector<bool>             &cells_ok() const { return m_cells_ok; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void open_photo();
    void read();
    void on_paint();
    void on_click(const wxPoint &pt);
    // Panel <-> image pixel transform of the displayed photo.
    double                 m_scale{1.}, m_off_x{0.}, m_off_y{0.};
    wxPanel               *m_canvas{nullptr};
    wxChoice              *m_board_choice{nullptr};
    wxChoice              *m_source{nullptr};
    wxSpinCtrlDouble      *m_paper{nullptr};
    wxStaticText          *m_status{nullptr};
    wxButton              *m_ok{nullptr};
    wxImage               *m_image{nullptr};
    wxBitmap              *m_preview{nullptr};
    ColorMix::BoardLayout  m_layout;
    std::vector<std::array<double, 2>> m_corners;
    std::vector<ColorMix::Lab>         m_cells;
    std::vector<bool>                  m_cells_ok;
    int                    m_board{0};
public:
    ~ColorMixPhotoDialog() override;
};

// Undo/redo across a ColorMix conversion: the printer's extruder colors (a preset setting, not part
// of the undo stack) follow the project data. Call after the Model was restored, with the ColorMix
// options before and after the jump.
void colormix_sync_extruder_colors_after_undo(Plater *plater, const std::string &options_before, const std::string &options_after);

} // namespace Slic3r::GUI
