///|/ ColorMax engine - color prediction for layer-mixed recipes.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "Predictor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <Eigen/Dense>

namespace Slic3r::ColorMix {

namespace {
constexpr double PI = 3.14159265358979323846;

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double percentile(std::vector<double> v, double p)
{
    if (v.empty())
        return 0.;
    std::sort(v.begin(), v.end());
    const double pos = p * double(v.size() - 1);
    const size_t lo  = size_t(std::floor(pos));
    const size_t hi  = std::min(v.size() - 1, lo + 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - double(lo));
}

double sq_dist(const std::vector<double> &a, const std::vector<double> &b)
{
    double s = 0.;
    for (size_t i = 0; i < a.size(); ++i)
        s += (a[i] - b[i]) * (a[i] - b[i]);
    return s;
}
} // namespace

Lab fdm_mixer_predict(const std::vector<std::pair<Lab, double>> &parts_in)
{
    // Constants of prusa-fdm-mixer (bundled_deps/prusa_fdm_mixer/prusa_fdm_mixer.cpp).
    constexpr double YN_EXPONENT = 3.0, L_SLOPE = -0.0477, L_INTERCEPT = -2.112, L_KINK_THRESHOLD = 15.0,
                     L_KINK_SLOPE = -0.060, C_SLOPE = 0.2780, C_INTERCEPT = -15.580, PEAK_STRENGTH = 1.375,
                     HUE_PEAK_DEG = 10.38, HUE_CENTER_DEG = 210.0, HUE_HALF_WIDTH_DEG = 30.0;

    std::vector<std::pair<Lab, double>> parts;
    double total = 0.;
    for (const auto &p : parts_in)
        if (p.second > 0.) {
            parts.push_back(p);
            total += p.second;
        }
    if (parts.empty())
        return {50., 0., 0.};
    for (auto &p : parts)
        p.second /= total;
    for (const auto &p : parts)
        if (p.second >= 0.9999)
            return p.first;

    LinRGB acc{0., 0., 0.};
    double Lmin = 1e9, Lmax = -1e9;
    for (const auto &[lab, r] : parts) {
        const LinRGB lin = lab_to_linear(lab);
        for (int i = 0; i < 3; ++i)
            acc[i] += std::pow(std::clamp(lin[i], 0., 1.), 1. / YN_EXPONENT) * r;
        Lmin = std::min(Lmin, lab.L);
        Lmax = std::max(Lmax, lab.L);
    }
    LinRGB yn;
    for (int i = 0; i < 3; ++i)
        yn[i] = std::pow(std::max(0., acc[i]), YN_EXPONENT);
    const Lab base = linear_to_lab(yn);

    double w = 1.;
    for (const auto &p : parts)
        w *= p.second;
    w = std::clamp(w * std::pow(double(parts.size()), double(parts.size())), 0., 1.);
    const double cs = w * PEAK_STRENGTH;

    const double gap = Lmax - Lmin;
    double Lc = L_SLOPE * gap + L_INTERCEPT;
    if (gap > L_KINK_THRESHOLD)
        Lc += L_KINK_SLOPE * (gap - L_KINK_THRESHOLD);
    const double Ln = base.L + Lc * cs;

    double a = base.a, b = base.b;
    const double C = std::hypot(a, b);
    if (C >= 0.01) {
        const double nC = std::max(0., C + (C_SLOPE * Ln + C_INTERCEPT) * cs);
        a *= nC / C;
        b *= nC / C;
    }
    const double C2 = std::hypot(a, b);
    if (C2 >= 1.) {
        double h = std::atan2(b, a) * 180. / PI;
        if (h < 0.)
            h += 360.;
        if (h >= HUE_CENTER_DEG - HUE_HALF_WIDTH_DEG && h < HUE_CENTER_DEG + HUE_HALF_WIDTH_DEG) {
            const double hc = HUE_PEAK_DEG * std::max(0., 1. - std::abs(h - HUE_CENTER_DEG) / HUE_HALF_WIDTH_DEG) * w;
            if (hc != 0.) {
                const double nh = std::fmod(h + hc, 360.) * PI / 180.;
                a = C2 * std::cos(nh);
                b = C2 * std::sin(nh);
            }
        }
    }
    return {Ln, a, b};
}

namespace {
std::vector<double> word_or_ratio_weights(const Bundle &bundle, const std::vector<std::string> &word,
                                          const std::vector<std::pair<std::string, double>> &ratios, const LayerSchedule &schedule)
{
    const size_t n = bundle.filaments.size();
    if (!word.empty()) {
        Word w;
        for (const std::string &fid : word)
            w.push_back(bundle.filament_index(fid));
        if (std::any_of(w.begin(), w.end(), [](int i) { return i < 0; }))
            return {};
        return word_weights(w, n, schedule);
    }
    std::vector<double> out(n, 0.);
    double              tot = 0.;
    for (const auto &[fid, r] : ratios) {
        const int i = bundle.filament_index(fid);
        if (i < 0 || r < 0.)
            return {};
        out[size_t(i)] += r;
        tot += r;
    }
    if (tot <= 0.)
        return {};
    for (double &v : out)
        v /= tot;
    return out;
}

// [w] -> [w, w]; [outer, inner] unchanged.
std::vector<double> to_features(const std::vector<double> &w, size_t n)
{
    if (w.size() == 2 * n)
        return w;
    std::vector<double> f(w);
    f.resize(2 * n, 0.);
    std::copy(f.begin(), f.begin() + long(n), f.begin() + long(n));
    return f;
}
} // namespace

std::vector<double> Predictor::swatch_weights(const Bundle &bundle, const Swatch &s, const LayerSchedule &schedule)
{
    const std::vector<double> outer = word_or_ratio_weights(bundle, s.word, s.ratios, schedule);
    if (outer.empty())
        return {};
    std::vector<double> inner = s.inner_word.empty() ? outer : word_or_ratio_weights(bundle, s.inner_word, {}, schedule);
    if (inner.empty())
        return {};
    std::vector<double> f(outer);
    f.insert(f.end(), inner.begin(), inner.end());
    return f;
}

Prediction Predictor::predict(const Recipe &r) const
{
    if (!r.dual())
        return predict(r.weights);
    std::vector<double> f(r.weights);
    f.insert(f.end(), r.inner_weights.begin(), r.inner_weights.end());
    return predict(f);
}

double Predictor::td_strength(double td)
{
    return td > 0. ? std::clamp(std::pow(TD_REF / std::max(td, 0.1), TD_POWER), 0.25, 4.) : 1.;
}

Predictor::Predictor(const Bundle &bundle, const Calibration *cal, const LayerSchedule &schedule)
{
    for (const BundleFilament &f : bundle.filaments) {
        m_filament_lab.push_back(hex_to_lab(f.color));
        m_td.push_back(f.td);
        m_strength.push_back(td_strength(f.td));
    }
    auto update_lin = [this]() {
        m_filament_lin.clear();
        for (const Lab &l : m_filament_lab)
            m_filament_lin.push_back(lab_to_linear(l));
    };
    update_lin();
    if (cal == nullptr)
        return;
    // Swatches are weighed on the layer schedule they were printed with, not the current one.
    LayerSchedule cal_schedule = schedule;
    if (!cal->layer_schedule.empty()) {
        cal_schedule.first_layer_height = cal->first_layer_height;
        cal_schedule.heights            = cal->layer_schedule;
    }
    m_measured_apply = cal_schedule.heights == schedule.heights;

    // Measured pure filaments replace the nominal colors.
    std::vector<std::vector<Lab>> pure(bundle.filaments.size());
    for (const Swatch &s : cal->swatches) {
        if (s.held_out)
            continue;
        const std::vector<double> w = swatch_weights(bundle, s, cal_schedule);
        if (w.empty())
            continue;
        const size_t n = bundle.filaments.size();
        for (size_t i = 0; i < n; ++i)
            if (w[i] > 0.9999 && w[n + i] > 0.9999)
                pure[i].push_back(s.measured);
        m_measured.push_back({w, s.measured});
    }
    for (size_t i = 0; i < pure.size(); ++i)
        if (!pure[i].empty()) {
            Lab avg{};
            for (const Lab &l : pure[i]) {
                avg.L += l.L / double(pure[i].size());
                avg.a += l.a / double(pure[i].size());
                avg.b += l.b / double(pure[i].size());
            }
            m_filament_lab[i] = avg;
        }
    update_lin();
    m_calibrated = !m_measured.empty();
    fit(bundle, *cal, cal_schedule);
}

Lab Predictor::base(const std::vector<double> &features) const { return base(features, m_base_model, m_strength, m_share); }

Lab Predictor::base(const std::vector<double> &features, BaseModel model, const std::vector<double> &strength, double share) const
{
    const size_t n = m_filament_lab.size();
    const std::vector<double> f = to_features(features, n);
    bool same = true;
    for (size_t i = 0; i < n && same; ++i)
        same = std::abs(f[i] - f[n + i]) < 1e-9;
    // Strength-weighted shares of the outer and inner walls, then the walls' shares of the color.
    auto shares = [&](size_t off) {
        std::vector<double> w(n, 0.);
        double tot = 0.;
        for (size_t i = 0; i < n; ++i) {
            w[i] = f[off + i] * (i < strength.size() ? strength[i] : 1.);
            tot += w[i];
        }
        if (tot > 0.)
            for (double &v : w)
                v /= tot;
        return w;
    };
    const std::vector<double> wo = shares(0), wi = same ? wo : shares(n);
    std::vector<double>       w(n);
    for (size_t i = 0; i < n; ++i)
        w[i] = same ? wo[i] : share * wo[i] + (1. - share) * wi[i];
    if (model == BaseModel::Subtractive) {
        // Light filtered by translucent layers: a weighted geometric mean of the filaments' colors.
        LinRGB acc{0., 0., 0.};
        for (size_t i = 0; i < n; ++i)
            if (w[i] > 0.)
                for (int c = 0; c < 3; ++c)
                    acc[c] += w[i] * std::log(std::max(m_filament_lin[i][c], 1e-4));
        return linear_to_lab({std::exp(acc[0]), std::exp(acc[1]), std::exp(acc[2])});
    }
    std::vector<std::pair<Lab, double>> parts;
    for (size_t i = 0; i < n; ++i)
        if (w[i] > 0.)
            parts.emplace_back(m_filament_lab[i], w[i]);
    return fdm_mixer_predict(parts);
}

void Predictor::fit_base(const Bundle &bundle, const std::vector<std::pair<std::vector<double>, Lab>> &train)
{
    const size_t n = m_filament_lab.size();
    // Uncalibrated: strengths from the TDs (1 where unknown).
    std::vector<double> prior(n, 1.);
    for (size_t i = 0; i < n && i < m_td.size(); ++i)
        prior[i] = td_strength(m_td[i]);
    m_strength   = prior;
    m_share      = DUAL_OUTER_SHARE;
    m_base_model = BaseModel::Prusa;
    m_report.base_model = "prusa";
    m_report.outer_share = m_share;
    m_report.strengths.clear();
    m_report.td_strengths.clear();
    // Too few swatches to fit a handful of parameters: keep Prusa's mixer as it is.
    if (train.size() < 12)
        return;
    auto mean_error = [&](BaseModel model, const std::vector<double> &st, double share) {
        double e = 0.;
        for (const auto &t : train)
            e += delta_e_2000(t.second, base(t.first, model, st, share));
        return e / double(train.size());
    };
    double best = mean_error(BaseModel::Prusa, m_strength, m_share);
    // Filaments that appear in the swatches (others keep strength 1). The first one stays fixed:
    // only ratios of strengths matter.
    std::vector<size_t> used;
    for (size_t i = 0; i < n; ++i)
        if (std::any_of(train.begin(), train.end(), [&](const auto &t) { return t.first[i] > 0. || t.first[n + i] > 0.; }))
            used.push_back(i);
    const bool any_dual = std::any_of(train.begin(), train.end(), [&](const auto &t) {
        for (size_t i = 0; i < n; ++i)
            if (std::abs(t.first[i] - t.first[n + i]) > 1e-9)
                return true;
        return false;
    });
    bool fitted = false;
    for (BaseModel model : {BaseModel::Prusa, BaseModel::Subtractive}) {
        // Coordinate descent on log strengths and the outer share, halving the step when stuck. Prusa's
        // mixer starts from the TD strengths (they improve it); the subtractive model from neutral.
        std::vector<double> logs(n, 0.);
        if (model == BaseModel::Prusa)
            for (size_t i = 0; i < n; ++i)
                logs[i] = std::log(prior[i]);
        double share = DUAL_OUTER_SHARE;
        auto   err   = [&]() {
            std::vector<double> st(n);
            for (size_t i = 0; i < n; ++i)
                st[i] = std::exp(logs[i]);
            return mean_error(model, st, share);
        };
        double cur = err();
        for (double step = 0.5; step > 0.02; step *= 0.5) {
            for (bool improved = true; improved;) {
                improved = false;
                for (size_t k = 1; k < used.size(); ++k)
                    for (double d : {step, -step}) {
                        logs[used[k]] += d;
                        if (const double e = err(); e < cur - 1e-6 && std::abs(logs[used[k]]) < 3.) {
                            cur      = e;
                            improved = true;
                            break;
                        }
                        logs[used[k]] -= d;
                    }
                if (any_dual)
                    for (double d : {0.25 * step, -0.25 * step}) {
                        share += d;
                        if (const double e = err(); e < cur - 1e-6 && share > 0.2 && share < 1.) {
                            cur      = e;
                            improved = true;
                            break;
                        }
                        share -= d;
                    }
            }
        }
        if (cur < best - 1e-6) {
            best         = cur;
            fitted       = true;
            m_base_model = model;
            m_share      = share;
            // Strengths relative to their geometric mean (easier to read; the model doesn't change).
            double g = 0.;
            for (size_t i : used)
                g += logs[i] / double(used.size());
            for (size_t i = 0; i < n; ++i)
                m_strength[i] = std::exp(logs[i] - g);
        }
    }
    if (fitted) {
        // Filaments not on the calibration: the strength of the calibrated filaments of similar TD
        // (interpolated in log TD, the nearest one beyond the ends), else the TD alone, else neutral.
        std::vector<std::pair<double, double>> pts; // (log TD, log strength) of calibrated filaments with a TD
        double g = 0.;
        for (size_t i : used) {
            if (i < m_td.size() && m_td[i] > 0.)
                pts.emplace_back(std::log(m_td[i]), std::log(m_strength[i]));
            g += std::log(prior[i]) / double(used.size());
        }
        std::sort(pts.begin(), pts.end());
        for (size_t i = 0; i < n; ++i) {
            if (std::find(used.begin(), used.end(), i) != used.end())
                continue;
            const double td = i < m_td.size() ? m_td[i] : 0.;
            if (td > 0. && !pts.empty()) {
                const double x = std::log(td);
                double       y = x <= pts.front().first ? pts.front().second : pts.back().second;
                for (size_t k = 0; k + 1 < pts.size(); ++k)
                    if (x >= pts[k].first && x <= pts[k + 1].first) {
                        const double t = pts[k + 1].first > pts[k].first ? (x - pts[k].first) / (pts[k + 1].first - pts[k].first) : 0.;
                        y = pts[k].second + t * (pts[k + 1].second - pts[k].second);
                        break;
                    }
                m_strength[i] = std::exp(y);
                if (i < bundle.filaments.size() && bundle.filaments[i].mixes())
                    m_report.td_strengths.emplace_back(bundle.filaments[i].name, m_strength[i]);
            } else
                m_strength[i] = m_base_model == BaseModel::Prusa ? prior[i] / std::exp(g) : 1.;
        }
    }
    m_report.base_model  = m_base_model == BaseModel::Subtractive ? "subtractive" : fitted ? "prusa+strength" : "prusa";
    m_report.outer_share = m_share;
    if (fitted || m_base_model == BaseModel::Subtractive)
        for (size_t i : used)
            m_report.strengths.emplace_back(i < bundle.filaments.size() ? bundle.filaments[i].name : std::string(), m_strength[i]);
}

void Predictor::fit(const Bundle &bundle, const Calibration &cal, const LayerSchedule &schedule)
{
    struct Sample { std::vector<double> w; Oklab resid; Lab measured; Lab base; };
    std::vector<Sample> train, test;
    {
        // The base model first: fitted to the mixed, fitting (not held-out) swatches.
        std::vector<std::pair<std::vector<double>, Lab>> base_train;
        std::vector<double>                              nominal_err;
        for (const Swatch &s : cal.swatches) {
            const std::vector<double> w = swatch_weights(bundle, s, schedule);
            if (w.empty() || s.held_out)
                continue;
            const size_t nb = bundle.filaments.size();
            int          nf = 0;
            for (size_t i = 0; i < nb; ++i)
                nf += (w[i] > 0. || w[nb + i] > 0.) ? 1 : 0;
            if (nf >= 2) {
                base_train.emplace_back(w, s.measured);
                nominal_err.push_back(delta_e_2000(s.measured, base(w, BaseModel::Prusa, {}, DUAL_OUTER_SHARE)));
            }
        }
        fit_base(bundle, base_train);
        m_report.nominal_base_median = median(nominal_err);
    }
    for (const Swatch &s : cal.swatches) {
        const std::vector<double> w = swatch_weights(bundle, s, schedule);
        if (w.empty())
            continue;
        const size_t nb = bundle.filaments.size();
        int          nf = 0;
        for (size_t i = 0; i < nb; ++i)
            nf += (w[i] > 0. || w[nb + i] > 0.) ? 1 : 0;
        if (nf < 2)
            continue;
        const Lab   b  = base(w);
        const Oklab mo = lab_to_oklab(s.measured), bo = lab_to_oklab(b);
        Sample smp{w, {mo.L - bo.L, mo.a - bo.a, mo.b - bo.b}, s.measured, b};
        (s.held_out ? test : train).push_back(std::move(smp));
    }
    m_report.swatches_used = int(train.size());
    m_report.held_out      = int(test.size());
    {
        std::vector<double> e;
        for (const Sample &s : train)
            e.push_back(delta_e_2000(s.measured, s.base));
        m_report.base_median = median(e);
    }
    const size_t n = train.size();
    if (n < 3)
        return;

    double amp = 0.;
    for (const Sample &s : train)
        amp += (s.resid.L * s.resid.L + s.resid.a * s.resid.a + s.resid.b * s.resid.b) / (3. * double(n));
    amp = std::max(amp, 1e-8);

    auto build = [&](double len2, double noise, Eigen::MatrixXd &Kinv) {
        Eigen::MatrixXd K(n, n);
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j)
                K(i, j) = amp * std::exp(-sq_dist(train[i].w, train[j].w) / len2) + (i == j ? noise * amp : 0.);
        Kinv = K.inverse();
    };
    auto Y = [&](int c) {
        Eigen::VectorXd y(n);
        for (size_t i = 0; i < n; ++i)
            y(i) = c == 0 ? train[i].resid.L : c == 1 ? train[i].resid.a : train[i].resid.b;
        return y;
    };

    double          best = 1e300;
    Eigen::MatrixXd best_Kinv;
    std::vector<Lab> best_loo;
    for (double len2 : {0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2, 6.4})
        for (double noise : {0.01, 0.03, 0.1, 0.3, 1.0}) {
            Eigen::MatrixXd Kinv;
            build(len2, noise, Kinv);
            // Closed-form leave-one-out: residual_i = alpha_i / Kinv_ii.
            std::vector<Oklab> loo_pred(n);
            for (int c = 0; c < 3; ++c) {
                const Eigen::VectorXd y = Y(c), alpha = Kinv * y;
                for (size_t i = 0; i < n; ++i) {
                    const double pred = y(i) - alpha(i) / Kinv(i, i);
                    (c == 0 ? loo_pred[i].L : c == 1 ? loo_pred[i].a : loo_pred[i].b) = pred;
                }
            }
            std::vector<Lab> loo(n);
            double sse = 0.;
            for (size_t i = 0; i < n; ++i) {
                const Oklab bo = lab_to_oklab(train[i].base);
                loo[i] = oklab_to_lab({bo.L + loo_pred[i].L, bo.a + loo_pred[i].a, bo.b + loo_pred[i].b});
                const double e = delta_e_2000(train[i].measured, loo[i]);
                sse += e * e;
            }
            if (sse < best) {
                best      = sse;
                best_Kinv = Kinv;
                best_loo  = loo;
                m_len2    = len2;
                m_noise   = noise;
            }
        }
    m_amp = amp;
    m_X.clear();
    for (const Sample &s : train)
        m_X.push_back(s.w);
    for (int c = 0; c < 3; ++c) {
        const Eigen::VectorXd alpha = best_Kinv * Y(c);
        m_alpha[c].assign(alpha.data(), alpha.data() + n);
    }
    m_Kinv.assign(n * n, 0.);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            m_Kinv[i * n + j] = best_Kinv(i, j);

    std::vector<double> e;
    for (size_t i = 0; i < n; ++i)
        e.push_back(delta_e_2000(train[i].measured, best_loo[i]));
    m_report.loo_median = median(e);
    m_report.loo_p90    = percentile(e, 0.9);
    m_report.length2    = m_len2;
    m_report.noise      = m_noise;
    e.clear();
    for (const Sample &s : test) {
        // The model itself, not the exact-match shortcut (a held-out swatch may repeat a fitted one).
        const Prediction p = model_predict(to_features(s.w, m_filament_lab.size()), false);
        e.push_back(delta_e_2000(s.measured, p.color));
    }
    m_report.held_out_median = median(e);
    m_report.held_out_p90    = percentile(e, 0.9);
}

Oklab Predictor::correction(const std::vector<double> &w, double *var) const
{
    const size_t n = m_X.size();
    if (n == 0) {
        if (var)
            *var = 0.;
        return {};
    }
    std::vector<double> k(n);
    for (size_t i = 0; i < n; ++i)
        k[i] = m_amp * std::exp(-sq_dist(w, m_X[i]) / m_len2);
    Oklab out{};
    for (size_t i = 0; i < n; ++i) {
        out.L += k[i] * m_alpha[0][i];
        out.a += k[i] * m_alpha[1][i];
        out.b += k[i] * m_alpha[2][i];
    }
    if (var) {
        double q = 0.;
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j)
                q += k[i] * m_Kinv[i * n + j] * k[j];
        *var = std::max(0., m_amp - q);
    }
    return out;
}

Prediction Predictor::predict(const std::vector<double> &weights_in, bool with_sigma) const
{
    const std::vector<double> weights = to_features(weights_in, m_filament_lab.size());
    for (const Measured &m : m_measured_apply ? m_measured : std::vector<Measured>()) {
        bool same = m.w.size() == weights.size();
        for (size_t i = 0; same && i < weights.size(); ++i)
            same = std::abs(m.w[i] - weights[i]) < 1e-3;
        if (same)
            return {m.lab, true, 0.};
    }
    return model_predict(weights, with_sigma);
}

Prediction Predictor::model_predict(const std::vector<double> &weights, bool with_sigma) const
{
    const size_t nb = m_filament_lab.size();
    const Lab    b  = base(weights);
    int          nf = 0;
    for (size_t i = 0; i < nb; ++i)
        nf += (weights[i] > 0. || weights[nb + i] > 0.) ? 1 : 0;
    if (nf < 2)
        return {b, false, 0.};
    double      var = 0.;
    const Oklab c   = correction(weights, with_sigma ? &var : nullptr);
    const Oklab bo  = lab_to_oklab(b);
    return {oklab_to_lab({bo.L + c.L, bo.a + c.a, bo.b + c.b}), false, 100. * std::sqrt(var)};
}

std::string FitReport::summary() const
{
    char buf[512];
    if (swatches_used < 3)
        std::snprintf(buf, sizeof(buf), "%d mixed swatches measured (need 3+ to fit a correction).", swatches_used);
    else
        std::snprintf(buf, sizeof(buf),
                      "%d mixed swatches: uncorrected median dE2000 %.1f, corrected (leave-one-out) median %.1f, P90 %.1f.%s",
                      swatches_used, base_median, loo_median, loo_p90,
                      held_out > 0 ? "" : " No held-out swatches.");
    std::string s = buf;
    if (swatches_used >= 3 && base_model != "prusa") {
        std::string st;
        for (const auto &[name, v] : strengths) {
            char b2[96];
            std::snprintf(b2, sizeof(b2), "%s%s %.2f", st.empty() ? "" : ", ", name.c_str(), v);
            st += b2;
        }
        std::snprintf(buf, sizeof(buf), " Mixing model fitted to the swatches: %s (the uncalibrated mixer: median %.1f), filament strengths ",
                      base_model == "subtractive" ? "subtractive" : "Prusa mixer", nominal_base_median);
        s += buf + st;
        std::snprintf(buf, sizeof(buf), ", outer wall %.0f%%.", 100. * outer_share);
        s += buf;
        if (!td_strengths.empty()) {
            s += " Not on the swatches, estimated from TD:";
            for (size_t k = 0; k < td_strengths.size(); ++k) {
                std::snprintf(buf, sizeof(buf), "%s %s %.2f", k ? "," : "", td_strengths[k].first.c_str(), td_strengths[k].second);
                s += buf;
            }
            s += ".";
        }
    }
    if (held_out > 0) {
        std::snprintf(buf, sizeof(buf), " Held-out (%d): median %.1f, P90 %.1f.", held_out, held_out_median, held_out_p90);
        s += buf;
    }
    return s;
}

} // namespace Slic3r::ColorMix
