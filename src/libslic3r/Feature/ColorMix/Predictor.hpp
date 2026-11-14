///|/ ColorMax engine - color prediction for layer-mixed recipes.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <string>
#include <vector>

#include "Bundle.hpp"
#include "Recipes.hpp"

namespace Slic3r::ColorMix {

// Port of Prusa's prusa-fdm-mixer (MIT, https://github.com/prusa3d/prusa-fdm-mixer) working
// on Lab inputs, so measured filament colors can be used without 8-bit rounding.
// parts: (color, share) with shares summing to 1.
Lab fdm_mixer_predict(const std::vector<std::pair<Lab, double>> &parts);

struct Prediction
{
    Lab    color;
    bool   measured{false};    // exact match of a measured swatch
    double sigma{0.};          // rough 1-sigma uncertainty, Oklab x 100 (0 when unknown)
};

struct FitReport
{
    int    swatches_used{0};
    int    held_out{0};
    // Errors in CIEDE2000.
    double loo_median{0.}, loo_p90{0.};     // leave-one-out over fitting swatches, corrected model
    double base_median{0.};                 // same swatches, uncorrected base mixer
    double held_out_median{0.}, held_out_p90{0.};
    double length2{0.}, noise{0.};          // chosen GP hyper-parameters
    // Base model fitted to the calibration (before the GP correction): "prusa" = prusa-fdm-mixer on the
    // layer shares, "prusa+strength" = the same on strength-weighted shares, "subtractive" = geometric
    // mean of the filament colors in linear light on strength-weighted shares (light filtered by
    // translucent layers). Strengths: how strongly each filament shows in a mix, relative to the others.
    std::string base_model{"prusa"};
    std::vector<std::pair<std::string, double>> strengths; // (filament name, strength), fitted filaments only
    std::vector<std::pair<std::string, double>> td_strengths; // filaments not on the swatches, strength estimated from their TD
    double outer_share{0.};
    double nominal_base_median{0.};          // the uncalibrated mixer on the same swatches
    std::string summary() const;
};

// Predicts recipe colors for one bundle under one calibration (or none):
//  1. measured swatch with identical weights -> its measured color,
//  2. otherwise prusa-fdm-mixer on the filament colors (measured pure swatches when present),
//  3. plus a Gaussian-process correction fitted to the calibration residuals.
class Predictor
{
public:
    Predictor(const Bundle &bundle, const Calibration *calibration, const LayerSchedule &schedule);

    // `weights`: per bundle filament (single wall), or outer weights followed by inner weights (dual walls).
    // `with_sigma`: also compute the uncertainty (O(N^2) in the calibration size).
    Prediction predict(const std::vector<double> &weights, bool with_sigma = false) const;
    Prediction predict(const Recipe &r) const;

    // Effective color of each bundle filament (measured if available).
    const std::vector<Lab> &filament_colors() const { return m_filament_lab; }
    bool                    calibrated() const { return m_calibrated; }
    const FitReport        &report() const { return m_report; }

    // Features of a swatch on this predictor's schedule: outer weights followed by inner weights.
    static std::vector<double> swatch_weights(const Bundle &b, const Swatch &s, const LayerSchedule &schedule);
    // Share of the outer wall in the color of a dual-wall recipe (uncalibrated model).
    static constexpr double DUAL_OUTER_SHARE = 0.69;
    // Strength of a filament nobody has calibrated, from its TD (HueForge transmission distance, mm):
    // translucent filaments show less in a layer mix. (TD_REF / td)^TD_POWER, 1 when the TD is unknown.
    // Fitted on Prusa's measured mixes (prusa-fdm-mixer fitting and holdout sets, Prusament TDs):
    // mean error 7.8 -> 7.3 dE00 on the holdout set.
    static constexpr double TD_REF   = 3.;
    static constexpr double TD_POWER = 0.25;
    static double           td_strength(double td);
    // Strength of each filament: fitted to the calibration, else borrowed from the calibrated filaments of
    // similar TD, else from the TD alone, else 1.
    const std::vector<double> &strengths() const { return m_strength; }

    enum class BaseModel { Prusa, Subtractive };

private:
    Lab    base(const std::vector<double> &weights) const;
    Lab    base(const std::vector<double> &weights, BaseModel model, const std::vector<double> &strength, double share) const;
    // Chooses the base model and fits filament strengths and the dual-wall outer share to the
    // calibration swatches (`train`: features and measured colors).
    void   fit_base(const Bundle &bundle, const std::vector<std::pair<std::vector<double>, Lab>> &train);
    // Base mixer + GP correction, without the exact-match shortcut.
    Prediction model_predict(const std::vector<double> &features, bool with_sigma) const;
    void   fit(const Bundle &bundle, const Calibration &cal, const LayerSchedule &schedule);
    Oklab  correction(const std::vector<double> &w, double *var) const;

    std::vector<Lab> m_filament_lab;
    std::vector<LinRGB> m_filament_lin;      // linear sRGB of m_filament_lab (subtractive model)
    BaseModel        m_base_model{BaseModel::Prusa};
    std::vector<double> m_strength;          // per bundle filament, 1 = neutral
    double           m_share{DUAL_OUTER_SHARE};
    std::vector<double> m_td;                // per bundle filament, 0 = unknown
    bool             m_calibrated{false};
    FitReport        m_report;

    struct Measured { std::vector<double> w; Lab lab; };
    std::vector<Measured> m_measured;
    // Measured colors stand for a recipe only on the schedule they were printed on (the same
    // weights on another schedule are another layer stack).
    bool                  m_measured_apply{true};

    // GP state.
    std::vector<std::vector<double>> m_X;
    std::vector<double>              m_alpha[3];
    std::vector<double>              m_Kinv;   // row-major inverse, for variance
    double                           m_len2{0.4}, m_amp{0.}, m_noise{0.02};
};

} // namespace Slic3r::ColorMix
