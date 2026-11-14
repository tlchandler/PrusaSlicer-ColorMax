///|/ ColorMax engine - color space helpers.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <array>
#include <optional>
#include <string>

namespace Slic3r::ColorMix {

// Linear-light sRGB, components nominally in [0, 1].
using LinRGB = std::array<double, 3>;

// CIELAB (D65), L in [0, 100].
struct Lab
{
    double L{0.}, a{0.}, b{0.};
    bool operator==(const Lab &o) const { return L == o.L && a == o.a && b == o.b; }
};

// Oklab, L in [0, 1].
struct Oklab
{
    double L{0.}, a{0.}, b{0.};
};

double srgb8_to_linear(double c255);
double linear_to_srgb8(double c);

// "#rrggbb" or "rrggbb" -> linear RGB; std::nullopt when malformed.
std::optional<LinRGB> hex_to_linear(const std::string &hex);
std::string           linear_to_hex(const LinRGB &rgb);
bool                  is_valid_hex(const std::string &hex);
// Normalizes to "#RRGGBB" (upper case). Returns empty on malformed input.
std::string           normalize_hex(const std::string &hex);

Lab    linear_to_lab(const LinRGB &rgb);
LinRGB lab_to_linear(const Lab &lab);
Oklab  linear_to_oklab(const LinRGB &rgb);
LinRGB oklab_to_linear(const Oklab &ok);
Oklab  lab_to_oklab(const Lab &lab);
Lab    oklab_to_lab(const Oklab &ok);

Lab         hex_to_lab(const std::string &hex);
std::string lab_to_hex(const Lab &lab);

// CIEDE2000 color difference.
double delta_e_2000(const Lab &x, const Lab &y);
// Euclidean distance in Oklab, multiplied by 100.
double delta_e_ok(const Lab &x, const Lab &y);

double chroma(const Lab &lab);
// Hue angle in degrees [0, 360).
double hue_deg(const Lab &lab);

} // namespace Slic3r::ColorMix
