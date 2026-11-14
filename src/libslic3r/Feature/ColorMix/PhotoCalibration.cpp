///|/ ColorMax engine - measuring calibration boards from a photo with a white sheet of paper.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "PhotoCalibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <map>

#include <miniz.h>

#include <Eigen/Dense>

namespace Slic3r::ColorMix {

// ---------------------------------------------------------------- layout

std::array<double, 4> BoardLayout::cell_rect(int row, int col) const
{
    const double x0 = margin + col * cell_w;
    const double y1 = base + (rows - row) * cell_h;
    return {x0, y1 - cell_h, x0 + cell_w, y1};
}

std::vector<std::array<int, 2>> BoardLayout::swatch_cells() const
{
    std::vector<std::array<int, 2>> out;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            out.push_back({r, c});
    return out;
}

int BoardLayout::swatches_per_board() const { return int(swatch_cells().size()); }

// ---------------------------------------------------------------- geometry

namespace {

struct Homography
{
    double h[8];
    std::array<double, 2> map(double x, double y) const
    {
        const double w = h[6] * x + h[7] * y + 1.;
        return {(h[0] * x + h[1] * y + h[2]) / w, (h[3] * x + h[4] * y + h[5]) / w};
    }
};

Homography make_homography(const BoardCorners &clicked, const BoardLayout &L)
{
    const double W = L.width(), H = L.height();
    auto dist = [](const std::array<double, 2> &a, const std::array<double, 2> &b) { return std::hypot(a[0] - b[0], a[1] - b[1]); };
    BoardCorners c = clicked;
    // Back face (see BoardCorners): dotted corner = front top-left, then front bottom-left,
    // bottom-right, top-right.
    if ((W > H) == (dist(c[0], c[1]) < dist(c[1], c[2])))
        c = {clicked[0], clicked[3], clicked[2], clicked[1]};
    const double src[4][2] = {{0., H}, {W, H}, {W, 0.}, {0., 0.}};
    Eigen::Matrix<double, 8, 8> A;
    Eigen::Matrix<double, 8, 1> b;
    for (int i = 0; i < 4; ++i) {
        const double x = src[i][0], y = src[i][1], u = c[i][0], v = c[i][1];
        A.row(2 * i) << x, y, 1., 0., 0., 0., -x * u, -y * u;
        A.row(2 * i + 1) << 0., 0., 0., x, y, 1., -x * v, -y * v;
        b(2 * i)     = u;
        b(2 * i + 1) = v;
    }
    const Eigen::Matrix<double, 8, 1> s = A.fullPivLu().solve(b);
    if (!s.allFinite() || (A * s - b).norm() > 1e-6 * (1. + b.norm()))
        throw std::runtime_error("The four corners do not form a usable quadrilateral.");
    Homography h;
    for (int i = 0; i < 8; ++i)
        h.h[i] = s(i);
    return h;
}

struct LinPixel { double r, g, b; };

class Sampler
{
public:
    explicit Sampler(const PhotoImage &img) : m_img(img)
    {
        for (int i = 0; i < 256; ++i)
            m_lut[i] = srgb8_to_linear(double(i));
    }
    bool inside(double u, double v, double border = 2.) const
    {
        return u >= border && v >= border && u < m_img.width - 1 - border && v < m_img.height - 1 - border;
    }
    // Bilinear sample in linear light; `clipped` set when any source channel is 254+; `black` counts the
    // source pixels with a channel at 0 (out of `count`).
    LinPixel at(double u, double v, bool *clipped = nullptr, long *black = nullptr, long *count = nullptr) const
    {
        const int    x0 = std::clamp(int(std::floor(u)), 0, m_img.width - 2);
        const int    y0 = std::clamp(int(std::floor(v)), 0, m_img.height - 2);
        const double fx = std::clamp(u - x0, 0., 1.), fy = std::clamp(v - y0, 0., 1.);
        double       acc[3] = {0., 0., 0.};
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const double         w = (dx ? fx : 1. - fx) * (dy ? fy : 1. - fy);
                const unsigned char *p = &m_img.rgb[(size_t(y0 + dy) * size_t(m_img.width) + size_t(x0 + dx)) * 3];
                if (count != nullptr) {
                    ++*count;
                    if (black != nullptr && (p[0] == 0 || p[1] == 0 || p[2] == 0))
                        ++*black;
                }
                for (int c = 0; c < 3; ++c) {
                    acc[c] += w * m_lut[p[c]];
                    if (clipped && p[c] >= 254)
                        *clipped = true;
                }
            }
        return {acc[0], acc[1], acc[2]};
    }

private:
    const PhotoImage &m_img;
    double            m_lut[256];
};

double median_of(std::vector<double> v)
{
    if (v.empty())
        return 0.;
    const size_t n = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + long(n), v.end());
    return v[n];
}

LinPixel median_pixel(const std::vector<LinPixel> &px)
{
    std::vector<double> r, g, b;
    for (const LinPixel &p : px) {
        r.push_back(p.r);
        g.push_back(p.g);
        b.push_back(p.b);
    }
    return {median_of(r), median_of(g), median_of(b)};
}

double luminance(const LinPixel &p) { return 0.2126 * p.r + 0.7152 * p.g + 0.0722 * p.b; }

// A swatch is a stack of differently colored layers (horizontal stripes on the face). A sharp photo
// resolves them, so point samples + a median would pick one filament instead of the blend the eye
// sees from a distance. Instead every column of the central part of the cell is averaged in linear
// light over its whole height (many layer periods, sampled finer than a pixel); the robust
// statistics (median, spread) then work across columns, which glare or specks affect unevenly.
std::vector<LinPixel> cell_columns(const Sampler &s, const Homography &h, const std::array<double, 4> &r, double fraction,
                                   int n_cols, bool *clipped, bool *outside, long *black = nullptr, long *count = nullptr)
{
    std::vector<LinPixel> out;
    const double cx = 0.5 * (r[0] + r[2]), cy = 0.5 * (r[1] + r[3]);
    const double hw = 0.5 * (r[2] - r[0]) * fraction;
    const double hh = 0.5 * (r[3] - r[1]) * std::max(fraction, 0.6);
    for (int i = 0; i < n_cols; ++i) {
        const double x   = cx - hw + 2. * hw * (i + 0.5) / n_cols;
        const auto   top = h.map(x, cy + hh), bot = h.map(x, cy - hh);
        const double len = std::hypot(top[0] - bot[0], top[1] - bot[1]);
        const int    n   = std::clamp(int(std::ceil(3. * len)), 64, 6000); // 1/3 px steps
        LinPixel     acc{0., 0., 0.};
        int          cnt = 0;
        for (int j = 0; j < n; ++j) {
            const double t = (j + 0.5) / n;
            const double u = bot[0] + t * (top[0] - bot[0]), v = bot[1] + t * (top[1] - bot[1]);
            if (!s.inside(u, v)) {
                if (outside)
                    *outside = true;
                continue;
            }
            const LinPixel p = s.at(u, v, clipped, black, count);
            acc.r += p.r;
            acc.g += p.g;
            acc.b += p.b;
            ++cnt;
        }
        if (cnt > n / 2)
            out.push_back({acc.r / cnt, acc.g / cnt, acc.b / cnt});
    }
    return out;
}

} // namespace

std::array<double, 2> board_to_image(const BoardCorners &corners, const BoardLayout &layout, double x_mm, double y_mm)
{
    return make_homography(corners, layout).map(x_mm, y_mm);
}

// ---------------------------------------------------------------- reading

PhotoReading read_board_photo(const PhotoImage &img, const BoardCorners &corners, const BoardLayout &L, const PhotoReadOptions &opt)
{
    if (img.width < 16 || img.height < 16 || img.rgb.size() < size_t(img.width) * size_t(img.height) * 3)
        throw std::runtime_error("The photo could not be read.");
    const Homography h = make_homography(corners, L);
    const Sampler    s(img);
    PhotoReading     out;
    const double     W = L.width(), H = L.height();
    bool clipped = false, outside = false;

    // White references: face position (mm), weight, pixel, distance from the face, side, clipped.
    struct Ref { double x, y, w; LinPixel p; double dist; int side; bool clip; };
    // Paper candidates: points 3 to 40 mm outside the face, every 6 mm along each side. Each is the
    // median of a 3 x 3 patch (a speck of dust or a fibre doesn't count).
    std::vector<Ref> cand;
    for (double d : {3., 5., 8., 12., 16., 20., 25., 30., 35., 40.}) {
        auto add = [&](double x, double y, int side) {
            const auto uv = h.map(x, y);
            if (!s.inside(uv[0], uv[1], 4.))
                return;
            std::vector<LinPixel> px;
            bool clip = false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    px.push_back(s.at(uv[0] + 2. * dx, uv[1] + 2. * dy, &clip));
            cand.push_back({x, y, 1., median_pixel(px), d, side, clip});
        };
        for (double x = -d; x <= W + d + 1e-9; x += 6.) {
            add(x, H + d, 0);
            add(x, -d, 1);
        }
        for (double y = 0.; y <= H + 1e-9; y += 6.) {
            add(-d, y, 2);
            add(W + d, y, 3);
        }
    }
    // Illumination field per channel: a + b x + c y + d x y (normalized face coordinates), fewer
    // terms when the points don't surround the board.
    auto fit_field = [&](const std::vector<Ref> &pts, int np) {
        Eigen::MatrixXd A(pts.size(), np), B(pts.size(), 3);
        for (size_t i = 0; i < pts.size(); ++i) {
            const double x = pts[i].x / W, y = pts[i].y / H, sw = std::sqrt(pts[i].w);
            A(i, 0) = sw;
            if (np > 1) A(i, 1) = sw * x;
            if (np > 2) A(i, 2) = sw * y;
            if (np > 3) A(i, 3) = sw * x * y;
            B(i, 0) = sw * pts[i].p.r;
            B(i, 1) = sw * pts[i].p.g;
            B(i, 2) = sw * pts[i].p.b;
        }
        return Eigen::MatrixXd(A.colPivHouseholderQr().solve(B));
    };
    auto eval_field = [&](const Eigen::MatrixXd &coef, double x_mm, double y_mm) {
        const double x = x_mm / W, y = y_mm / H;
        const int    np = int(coef.rows());
        Eigen::RowVectorXd f(np);
        f(0) = 1.;
        if (np > 1) f(1) = x;
        if (np > 2) f(2) = y;
        if (np > 3) f(3) = x * y;
        const Eigen::RowVectorXd v = f * coef;
        return LinPixel{std::max(v(0), 1e-4), std::max(v(1), 1e-4), std::max(v(2), 1e-4)};
    };
    auto terms_for = [](const std::vector<Ref> &pts) {
        int sides[4] = {0, 0, 0, 0};
        for (const Ref &r : pts)
            ++sides[r.side];
        if (sides[2] >= 2 && sides[3] >= 2 && sides[0] >= 2 && sides[1] >= 2)
            return 4;
        return sides[0] + sides[1] >= 4 ? 2 : 1;
    };

    // The paper is the brightest large, neutral, smoothly lit area around the board. Candidates with the
    // paper's tint (that of the brighter half) are fitted with the light field; for two rounds only
    // the points at or above the fit are kept (the fit climbs onto the upper envelope: the paper,
    // even under a strong light gradient), then every point within 6 % of it. The board's own shadow,
    // a scanner lid seen through a gap between board and paper (both darker), the table beyond the
    // sheet and colored things drop out.
    std::vector<Ref> refs;
    Eigen::MatrixXd  coef;
    if (cand.size() >= 4) {
        std::vector<double> l;
        for (const Ref &r : cand)
            l.push_back(luminance(r.p));
        const double mid = median_of(l);
        std::vector<double> tr, tb;
        for (const Ref &r : cand)
            if (luminance(r.p) >= mid) {
                tr.push_back(r.p.r / std::max(r.p.g, 1e-6));
                tb.push_back(r.p.b / std::max(r.p.g, 1e-6));
            }
        const double tint_r = median_of(tr), tint_b = median_of(tb);
        std::vector<Ref> pool;
        for (const Ref &r : cand) {
            const double trr = (r.p.r / std::max(r.p.g, 1e-6)) / tint_r, tbb = (r.p.b / std::max(r.p.g, 1e-6)) / tint_b;
            if (std::abs(trr - 1.) < 0.08 && std::abs(tbb - 1.) < 0.08 && luminance(r.p) > 0.01)
                pool.push_back(r);
        }
        std::vector<Ref> cur = pool;
        for (int it = 0; it < 7 && cur.size() >= 4; ++it) {
            coef = fit_field(cur, terms_for(cur));
            std::vector<Ref> next;
            for (const Ref &r : pool) {
                const double rel = luminance(r.p) / std::max(luminance(eval_field(coef, r.x, r.y)), 1e-6) - 1.;
                if (it < 3 ? rel >= -0.01 : std::abs(rel) < 0.06)
                    next.push_back(r);
            }
            if (next.size() < 4)
                break;
            cur.swap(next);
        }
        if (cur.size() >= 4) {
            refs = cur;
            coef = fit_field(refs, terms_for(refs));
        }
    }
    int    clipped_refs = 0;
    double gap          = 1e9;
    for (const Ref &r : refs) {
        clipped_refs += r.clip ? 1 : 0;
        gap = std::min(gap, r.dist);
    }
    out.paper_samples = int(refs.size());
    out.paper_clipped = refs.empty() ? 0. : double(clipped_refs) / double(refs.size());
    out.paper_gap     = refs.empty() ? 0. : gap;

    constexpr int MIN_PAPER = 8;
    if (opt.scanner && out.paper_samples < MIN_PAPER)
        throw std::runtime_error("No white paper found around the board. Lay the board in a hole cut into a sheet of plain white "
                                 "paper (or between strips of paper), flat on the glass and close to the board, and turn off "
                                 "automatic exposure.");
    if (!opt.scanner && out.paper_samples < MIN_PAPER)
        throw std::runtime_error("No white paper found around the board. Lay the board face-up on a sheet of plain white paper "
                                 "that sticks out at least 2 cm on every side, and include that paper in the photo.");
    // A scanner maps white paper on the glass close to full scale (linear ~0.8 or more); a lid held
    // 4 mm up by the board, or an open lid, reads much darker.
    if (opt.scanner && luminance(eval_field(coef, 0.5 * W, 0.5 * H)) < 0.5)
        throw std::runtime_error("No white paper found around the board (what is around it is gray, like the scanner lid seen through a "
                                 "gap). Lay the board in a hole cut into a sheet of plain white paper, flat on the glass, and turn off "
                                 "automatic exposure.");
    if (luminance(eval_field(coef, 0.5 * W, 0.5 * H)) < 0.03)
        throw std::runtime_error("The paper around the board is almost black in the image: the photo is far too dark.");
    double lmin = 1e9, lmax = 0.;
    for (double x : {0., W})
        for (double y : {0., H}) {
            const double l = luminance(eval_field(coef, x, y));
            lmin = std::min(lmin, l);
            lmax = std::max(lmax, l);
        }
    out.light_falloff = lmax / std::max(lmin, 1e-6);

    // Swatch cells.
    const double k = opt.paper_reflectance;
    for (const auto &rc : L.swatch_cells()) {
        const auto r   = L.cell_rect(rc[0], rc[1]);
        long       black = 0, count = 0;
        const auto px  = cell_columns(s, h, r, opt.cell_fraction, 15, &clipped, &outside, &black, &count);
        // Shadows crushed to 0: real swatches (even black ones) don't read 0 in a normal scan.
        if (count > 0 && double(black) > 0.05 * double(count))
            ++out.crushed_cells;
        if (px.empty()) {
            out.cells.push_back({0., 0., 0.});
            out.valid.push_back(false);
            out.spread.push_back(99.);
            continue;
        }
        const LinPixel ill = eval_field(coef, 0.5 * (r[0] + r[2]), 0.5 * (r[1] + r[3]));
        auto correct = [&](const LinPixel &p) {
            return linear_to_lab({std::clamp(p.r / ill.r * k, 0., 1.2), std::clamp(p.g / ill.g * k, 0., 1.2),
                                  std::clamp(p.b / ill.b * k, 0., 1.2)});
        };
        const Lab lab = correct(median_pixel(px));
        std::vector<double> de;
        for (const LinPixel &p : px)
            de.push_back(delta_e_2000(lab, correct(p)));
        out.cells.push_back(lab);
        out.valid.push_back(true);
        out.spread.push_back(median_of(de));
    }

    std::string warn;
    char        buf[700];
    if (out.crushed_cells > 0) {
        std::snprintf(buf, sizeof(buf),
                      "%d swatches have pure black pixels: the image is too dark. %s ", out.crushed_cells,
                      opt.scanner ? "This usually means the scanner's Brightness was lowered: Brightness subtracts from every pixel, which "
                                    "ColorMax can't correct (dark colors come out too dark). Scan with Brightness at its default." :
                                    "Raise the exposure (EV +0.3 to +0.7).");
        warn += buf;
    }
    if (out.paper_clipped > 0.2) {
        std::snprintf(buf, sizeof(buf),
                      "The white paper is pure white in the image (%.0f%% of it), so the colors may come out a little too light. %s ",
                      100. * out.paper_clipped,
                      opt.scanner ? "If the scanner has an Exposure or Levels setting (output highlight / white point about 240), lower "
                                    "it a little. Don't lower Brightness: it subtracts from every pixel and makes dark colors wrong. "
                                    "Without such a setting, scan as is (when the paper is only just pure white the error is about 1 dE)." :
                                    "Lower the exposure (EV -0.3 to -1).");
        warn += buf;
    } else if (clipped)
        warn += "Some swatch pixels are pure white (overexposed); lower the exposure. ";
    if (outside)
        warn += "Part of the board is outside the image or the corner clicks are off. ";
    if (out.light_falloff > 1.6)
        warn += "The light is very uneven across the board; use two diffuse lights. ";
    if (opt.scanner && out.paper_gap >= 10.) {
        std::snprintf(buf, sizeof(buf),
                      "The paper starts about %.0f mm from the board (the gap reads darker because the lid is held 4 mm up). That works, "
                      "but a hole cut to fit the board is better. ",
                      out.paper_gap);
        warn += buf;
    }
    const double worst_spread = out.spread.empty() ? 0. : *std::max_element(out.spread.begin(), out.spread.end());
    if (worst_spread > 6.) {
        std::snprintf(buf, sizeof(buf), "Some cells are not uniform (spread up to %.0f dE): glare, shadows or misplaced corners. ", worst_spread);
        warn += buf;
    }
    out.warning = warn;
    return out;
}

// ---------------------------------------------------------------- TIFF

namespace {

struct TiffReader
{
    const unsigned char *d;
    size_t               n;
    bool                 le;
    bool     ok(size_t off, size_t len) const { return off <= n && len <= n - off; }
    uint32_t u16(size_t off) const { return le ? uint32_t(d[off]) | uint32_t(d[off + 1]) << 8 : uint32_t(d[off]) << 8 | uint32_t(d[off + 1]); }
    uint32_t u32(size_t off) const
    {
        return le ? uint32_t(d[off]) | uint32_t(d[off + 1]) << 8 | uint32_t(d[off + 2]) << 16 | uint32_t(d[off + 3]) << 24 :
                    uint32_t(d[off]) << 24 | uint32_t(d[off + 1]) << 16 | uint32_t(d[off + 2]) << 8 | uint32_t(d[off + 3]);
    }
};

// TIFF LZW: MSB-first codes, 9..12 bits, 256 = clear, 257 = end, code width grows one code early.
bool lzw_decode(const unsigned char *src, size_t len, std::vector<unsigned char> &out, size_t expected)
{
    std::vector<std::vector<unsigned char>> table;
    auto reset = [&]() {
        table.assign(258, {});
        for (int i = 0; i < 256; ++i)
            table[size_t(i)] = {static_cast<unsigned char>(i)};
    };
    reset();
    size_t   bitpos = 0;
    int      width  = 9;
    int      prev   = -1;
    auto next_code = [&]() -> int {
        if ((bitpos + size_t(width)) > len * 8)
            return 257;
        uint32_t v = 0;
        for (int i = 0; i < width; ++i, ++bitpos)
            v = (v << 1) | ((src[bitpos >> 3] >> (7 - (bitpos & 7))) & 1u);
        return int(v);
    };
    while (out.size() < expected) {
        const int code = next_code();
        if (code == 257)
            break;
        if (code == 256) {
            reset();
            width = 9;
            prev  = -1;
            continue;
        }
        std::vector<unsigned char> entry;
        if (code < int(table.size()))
            entry = table[size_t(code)];
        else if (code == int(table.size()) && prev >= 0) {
            entry = table[size_t(prev)];
            entry.push_back(table[size_t(prev)][0]);
        } else
            return false;
        out.insert(out.end(), entry.begin(), entry.end());
        if (prev >= 0 && table.size() < 4096) {
            std::vector<unsigned char> e = table[size_t(prev)];
            e.push_back(entry[0]);
            table.push_back(std::move(e));
        }
        prev = code;
        if (table.size() + 1 >= (size_t(1) << width) && width < 12)
            ++width;
    }
    return true;
}

bool packbits_decode(const unsigned char *src, size_t len, std::vector<unsigned char> &out, size_t expected)
{
    size_t i = 0;
    while (i < len && out.size() < expected) {
        const int c = int(static_cast<signed char>(src[i++]));
        if (c >= 0) {
            if (i + size_t(c) + 1 > len)
                return false;
            out.insert(out.end(), src + i, src + i + c + 1);
            i += size_t(c) + 1;
        } else if (c != -128) {
            if (i >= len)
                return false;
            out.insert(out.end(), size_t(1 - c), src[i++]);
        }
    }
    return true;
}

} // namespace

bool is_tiff(const unsigned char *data, size_t size)
{
    return size >= 8 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
                         (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42));
}

bool decode_tiff(const unsigned char *data, size_t size, PhotoImage &out, std::string *error, const JpegDecoder &decode_jpeg)
{
    auto fail = [&](const std::string &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!is_tiff(data, size))
        return fail("Not a TIFF file (or a BigTIFF, which isn't supported).");
    const TiffReader R{data, size, data[0] == 'I'};
    const size_t     ifd = R.u32(4);
    if (!R.ok(ifd, 2))
        return fail("The TIFF file is damaged.");
    const uint32_t count = R.u16(ifd);
    if (!R.ok(ifd + 2, size_t(count) * 12))
        return fail("The TIFF file is damaged.");
    // Tag values as numbers (BYTE, SHORT, LONG) or raw bytes (UNDEFINED, e.g. JPEGTables).
    std::map<uint32_t, std::vector<uint32_t>>             num;
    std::map<uint32_t, std::pair<size_t, size_t>>         raw; // offset, length
    for (uint32_t i = 0; i < count; ++i) {
        const size_t   e = ifd + 2 + size_t(i) * 12;
        const uint32_t tag = R.u16(e), type = R.u16(e + 2), cnt = R.u32(e + 4);
        const size_t   unit = type == 3 ? 2 : (type == 4 ? 4 : (type == 1 || type == 7 || type == 2 ? 1 : 0));
        if (unit == 0 || cnt > (1u << 28))
            continue; // RATIONAL, DOUBLE, ... aren't needed
        const size_t bytes = unit * size_t(cnt);
        const size_t off   = bytes <= 4 ? e + 8 : size_t(R.u32(e + 8));
        if (!R.ok(off, bytes))
            return fail("The TIFF file is damaged.");
        raw[tag] = {off, bytes};
        std::vector<uint32_t> v;
        v.reserve(cnt);
        for (uint32_t k = 0; k < cnt; ++k)
            v.push_back(unit == 1 ? uint32_t(data[off + k]) : unit == 2 ? R.u16(off + 2 * size_t(k)) : R.u32(off + 4 * size_t(k)));
        num[tag] = std::move(v);
    }
    auto get = [&](uint32_t tag, uint32_t def) { auto it = num.find(tag); return it == num.end() || it->second.empty() ? def : it->second.front(); };
    const uint32_t width = get(256, 0), height = get(257, 0), compression = get(259, 1), photometric = get(262, 2),
                   spp = get(277, 1), planar = get(284, 1), predictor = get(317, 1);
    const uint32_t bps = get(258, 1);
    if (width == 0 || height == 0 || width > 40000 || height > 40000)
        return fail("The TIFF image has no usable size.");
    if (planar != 1 && spp > 1)
        return fail("TIFF files with separate color planes aren't supported. Save the scan as PNG.");
    const bool jpeg = compression == 7;
    if (!jpeg && (bps != 8 && bps != 16))
        return fail("Only 8 or 16 bits per channel TIFF files can be read.");
    const bool gray = photometric == 0 || photometric == 1;
    if (!jpeg && !(photometric == 2 && spp >= 3) && !(gray && spp >= 1))
        return fail("Only RGB or grayscale TIFF files can be read. Save the scan as PNG.");
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32946 && compression != 32773 && compression != 7)
        return fail("This TIFF compression isn't supported (" + std::to_string(compression) + "). Save the scan as PNG or as an uncompressed TIFF.");
    if (jpeg && !decode_jpeg)
        return fail("JPEG-compressed TIFF files can't be decoded here.");

    // Chunks: strips (full width) or tiles.
    struct Chunk { uint32_t x, y, w, h; size_t off, len; };
    std::vector<Chunk> chunks;
    if (num.count(324) && num.count(325)) {
        const uint32_t tw = get(322, 0), th = get(323, 0);
        const auto    &offs = num[324], &lens = num[325];
        if (tw == 0 || th == 0 || offs.size() != lens.size())
            return fail("The TIFF file is damaged.");
        const uint32_t across = (width + tw - 1) / tw;
        for (size_t i = 0; i < offs.size(); ++i)
            chunks.push_back({uint32_t(i % across) * tw, uint32_t(i / across) * th, tw, th, offs[i], lens[i]});
    } else {
        const uint32_t rps  = std::min(get(278, height), height);
        const auto    &offs = num[273], &lens = num[279];
        if (offs.empty() || offs.size() != lens.size() || rps == 0)
            return fail("The TIFF file is damaged.");
        for (size_t i = 0; i < offs.size(); ++i)
            chunks.push_back({0, uint32_t(i) * rps, width, rps, offs[i], lens[i]});
    }
    out.width  = int(width);
    out.height = int(height);
    out.rgb.assign(size_t(width) * size_t(height) * 3, 0);
    std::vector<unsigned char> tables;
    if (auto it = raw.find(347); it != raw.end() && it->second.second >= 4)
        tables.assign(data + it->second.first, data + it->second.first + it->second.second);
    const size_t bytes_per_px = jpeg ? 3 : size_t(spp) * (bps / 8);
    for (const Chunk &c : chunks) {
        if (c.y >= height || c.x >= width)
            continue;
        if (!R.ok(c.off, c.len))
            return fail("The TIFF file is damaged (truncated).");
        const unsigned char *src = data + c.off;
        const uint32_t       rows = std::min(c.h, height - c.y), cols = std::min(c.w, width - c.x);
        if (jpeg) {
            // An abbreviated JPEG stream; the shared tables (JPEGTables) go in front of it.
            std::vector<unsigned char> stream;
            if (!tables.empty() && c.len >= 2) {
                stream.assign(tables.begin(), tables.end() - 2);   // without EOI
                stream.insert(stream.end(), src + 2, src + c.len); // without SOI
            } else
                stream.assign(src, src + c.len);
            PhotoImage part;
            if (!decode_jpeg(stream, part) || part.width < int(cols) || part.height < int(rows))
                return fail("A JPEG-compressed part of the TIFF file could not be decoded. Save the scan as PNG.");
            for (uint32_t y = 0; y < rows; ++y)
                std::copy_n(&part.rgb[(size_t(y) * size_t(part.width)) * 3], size_t(cols) * 3,
                            &out.rgb[((size_t(c.y + y) * width) + c.x) * 3]);
            continue;
        }
        const size_t expected = size_t(c.w) * size_t(c.h) * bytes_per_px;
        std::vector<unsigned char> buf;
        bool ok = true;
        if (compression == 1)
            buf.assign(src, src + std::min(c.len, expected));
        else if (compression == 5)
            ok = lzw_decode(src, c.len, buf, expected);
        else if (compression == 32773)
            ok = packbits_decode(src, c.len, buf, expected);
        else {
            size_t out_len = 0;
            void  *p = tinfl_decompress_mem_to_heap(src, c.len, &out_len, TINFL_FLAG_PARSE_ZLIB_HEADER);
            ok       = p != nullptr;
            if (ok) {
                buf.assign(static_cast<unsigned char *>(p), static_cast<unsigned char *>(p) + out_len);
                mz_free(p);
            }
        }
        if (!ok)
            return fail("The TIFF file could not be decompressed.");
        // Strips may be shorter than rows-per-strip at the bottom of the image.
        buf.resize(expected, 0);
        // Samples as 16-bit or 8-bit values, undoing the horizontal predictor per row.
        const size_t row_samples = size_t(c.w) * spp;
        std::vector<uint32_t> line(row_samples);
        for (uint32_t y = 0; y < rows; ++y) {
            for (size_t i = 0; i < row_samples; ++i) {
                const size_t o = (size_t(y) * row_samples + i) * (bps / 8);
                line[i] = bps == 16 ? (R.le ? uint32_t(buf[o]) | uint32_t(buf[o + 1]) << 8 : uint32_t(buf[o]) << 8 | uint32_t(buf[o + 1])) : buf[o];
            }
            if (predictor == 2)
                for (size_t i = spp; i < row_samples; ++i)
                    line[i] = (line[i] + line[i - spp]) & (bps == 16 ? 0xFFFFu : 0xFFu);
            for (uint32_t x = 0; x < cols; ++x) {
                unsigned char *px = &out.rgb[((size_t(c.y + y) * width) + c.x + x) * 3];
                for (int ch = 0; ch < 3; ++ch) {
                    uint32_t v = line[size_t(x) * spp + (gray ? 0 : size_t(ch))];
                    if (bps == 16)
                        v >>= 8;
                    if (photometric == 0)
                        v = 255 - v;
                    px[ch] = static_cast<unsigned char>(v);
                }
            }
        }
    }
    return true;
}

} // namespace Slic3r::ColorMix
