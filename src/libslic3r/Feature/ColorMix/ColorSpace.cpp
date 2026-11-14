///|/ ColorMax engine - color space helpers.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "ColorSpace.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Slic3r::ColorMix {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double XN = 95.047, YN = 100.0, ZN = 108.883;

double lab_f(double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16. / 116.; }
double lab_finv(double t) { return t > 0.206893 ? t * t * t : (t - 16. / 116.) / 7.787; }
double deg(double r) { return r * 180. / PI; }
double rad(double d) { return d * PI / 180.; }
} // namespace

double srgb8_to_linear(double c255)
{
    const double c = c255 / 255.;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double linear_to_srgb8(double c)
{
    c = std::clamp(c, 0., 1.);
    const double v = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1. / 2.4) - 0.055;
    return v * 255.;
}

bool is_valid_hex(const std::string &hex)
{
    std::string s = hex;
    if (!s.empty() && s.front() == '#')
        s.erase(0, 1);
    if (s.size() != 6)
        return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string normalize_hex(const std::string &hex)
{
    if (!is_valid_hex(hex))
        return {};
    std::string s = hex.front() == '#' ? hex.substr(1) : hex;
    for (char &c : s)
        c = char(std::toupper((unsigned char) c));
    return "#" + s;
}

std::optional<LinRGB> hex_to_linear(const std::string &hex)
{
    if (!is_valid_hex(hex))
        return std::nullopt;
    const std::string s = hex.front() == '#' ? hex.substr(1) : hex;
    LinRGB            out;
    for (int i = 0; i < 3; ++i)
        out[i] = srgb8_to_linear(double(std::stoi(s.substr(2 * i, 2), nullptr, 16)));
    return out;
}

std::string linear_to_hex(const LinRGB &rgb)
{
    char buf[8];
    auto q = [](double c) { return int(std::lround(std::clamp(linear_to_srgb8(c), 0., 255.))); };
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", q(rgb[0]), q(rgb[1]), q(rgb[2]));
    return buf;
}

Lab linear_to_lab(const LinRGB &c)
{
    const double x = (c[0] * 0.4124564 + c[1] * 0.3575761 + c[2] * 0.1804375) * 100.;
    const double y = (c[0] * 0.2126729 + c[1] * 0.7151522 + c[2] * 0.0721750) * 100.;
    const double z = (c[0] * 0.0193339 + c[1] * 0.1191920 + c[2] * 0.9503041) * 100.;
    const double fx = lab_f(x / XN), fy = lab_f(y / YN), fz = lab_f(z / ZN);
    return {116. * fy - 16., 500. * (fx - fy), 200. * (fy - fz)};
}

LinRGB lab_to_linear(const Lab &lab)
{
    const double fy = (lab.L + 16.) / 116.;
    const double fx = lab.a / 500. + fy;
    const double fz = fy - lab.b / 200.;
    const double x = XN * lab_finv(fx) / 100., y = YN * lab_finv(fy) / 100., z = ZN * lab_finv(fz) / 100.;
    return {x * 3.2404542 + y * -1.5371385 + z * -0.4985314,
            x * -0.9692660 + y * 1.8760108 + z * 0.0415560,
            x * 0.0556434 + y * -0.2040259 + z * 1.0572252};
}

Oklab linear_to_oklab(const LinRGB &c)
{
    const double r = std::max(c[0], 0.), g = std::max(c[1], 0.), b = std::max(c[2], 0.);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

LinRGB oklab_to_linear(const Oklab &o)
{
    const double l = std::pow(o.L + 0.3963377774 * o.a + 0.2158037573 * o.b, 3.);
    const double m = std::pow(o.L - 0.1055613458 * o.a - 0.0638541728 * o.b, 3.);
    const double s = std::pow(o.L - 0.0894841775 * o.a - 1.2914855480 * o.b, 3.);
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

Oklab lab_to_oklab(const Lab &lab) { return linear_to_oklab(lab_to_linear(lab)); }
Lab   oklab_to_lab(const Oklab &ok) { return linear_to_lab(oklab_to_linear(ok)); }

Lab hex_to_lab(const std::string &hex)
{
    const auto lin = hex_to_linear(hex);
    return lin ? linear_to_lab(*lin) : Lab{50., 0., 0.};
}

std::string lab_to_hex(const Lab &lab) { return linear_to_hex(lab_to_linear(lab)); }

double chroma(const Lab &lab) { return std::hypot(lab.a, lab.b); }

double hue_deg(const Lab &lab)
{
    double h = deg(std::atan2(lab.b, lab.a));
    return h < 0. ? h + 360. : h;
}

double delta_e_ok(const Lab &x, const Lab &y)
{
    const Oklab a = lab_to_oklab(x), b = lab_to_oklab(y);
    return 100. * std::sqrt((a.L - b.L) * (a.L - b.L) + (a.a - b.a) * (a.a - b.a) + (a.b - b.b) * (a.b - b.b));
}

double delta_e_2000(const Lab &x, const Lab &y)
{
    const double L1 = x.L, a1 = x.a, b1 = x.b, L2 = y.L, a2 = y.a, b2 = y.b;
    const double avgL = (L1 + L2) / 2.;
    const double C1 = std::hypot(a1, b1), C2 = std::hypot(a2, b2);
    const double avgC = (C1 + C2) / 2.;
    const double p25  = std::pow(25., 7.);
    const double G    = 0.5 * (1. - std::sqrt(std::pow(avgC, 7.) / (std::pow(avgC, 7.) + p25)));
    const double a1p = a1 * (1. + G), a2p = a2 * (1. + G);
    const double C1p = std::hypot(a1p, b1), C2p = std::hypot(a2p, b2);
    const double avgCp = (C1p + C2p) / 2.;
    auto hp = [](double b, double ap) { double h = deg(std::atan2(b, ap)); return h < 0. ? h + 360. : h; };
    const double h1p = hp(b1, a1p), h2p = hp(b2, a2p);
    double dhp = 0.;
    if (C1p * C2p != 0.) {
        const double d = h2p - h1p;
        dhp = std::abs(d) <= 180. ? d : (d > 180. ? d - 360. : d + 360.);
    }
    double avgHp;
    if (C1p * C2p == 0.)
        avgHp = h1p + h2p;
    else if (std::abs(h1p - h2p) <= 180.)
        avgHp = (h1p + h2p) / 2.;
    else if (h1p + h2p < 360.)
        avgHp = (h1p + h2p + 360.) / 2.;
    else
        avgHp = (h1p + h2p - 360.) / 2.;
    const double T = 1. - 0.17 * std::cos(rad(avgHp - 30.)) + 0.24 * std::cos(rad(2. * avgHp))
                   + 0.32 * std::cos(rad(3. * avgHp + 6.)) - 0.20 * std::cos(rad(4. * avgHp - 63.));
    const double dLp = L2 - L1, dCp = C2p - C1p;
    const double dHp = 2. * std::sqrt(C1p * C2p) * std::sin(rad(dhp / 2.));
    const double SL  = 1. + 0.015 * (avgL - 50.) * (avgL - 50.) / std::sqrt(20. + (avgL - 50.) * (avgL - 50.));
    const double SC  = 1. + 0.045 * avgCp;
    const double SH  = 1. + 0.015 * avgCp * T;
    const double dTheta = 30. * std::exp(-std::pow((avgHp - 275.) / 25., 2.));
    const double RC     = 2. * std::sqrt(std::pow(avgCp, 7.) / (std::pow(avgCp, 7.) + p25));
    const double RT     = -RC * std::sin(rad(2. * dTheta));
    return std::sqrt((dLp / SL) * (dLp / SL) + (dCp / SC) * (dCp / SC) + (dHp / SH) * (dHp / SH)
                     + RT * (dCp / SC) * (dHp / SH));
}

} // namespace Slic3r::ColorMix
