///|/ ColorMax engine - measuring calibration boards from a photo with a white sheet of paper.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "ColorSpace.hpp"

namespace Slic3r::ColorMix {

// Geometry of one calibration board's front face, in mm. The face spans x in [0, width()] and
// y in [0, height()] with y pointing up (y = print Z). Swatch cells sit in a grid above a plain
// base strip, inside a frame.
// Default: 16 x 3 cells of 12 x 6 mm (48 swatches per board, 196 x 23 mm). 6 mm is ~60 layers at
// 0.1 mm, so a reading still averages many repeats of every layer word.
// The paper around the board is the white reference. (See-through windows in the boards were tried
// for that: the paper in those 4 mm deep holes read 25-45 % darker than next to the board and tinted
// by the colored cell walls, and their tops printed as sagging bridges.)
struct BoardLayout
{
    int    cols{16};
    int    rows{3};
    double cell_w{12.};
    double cell_h{6.};
    double margin{2.};     // frame at the left, right and top
    double base{3.};       // frame strip at the bottom (first layers)
    double thickness{4.};
    double gap{12.};       // between neighbouring boards on the bed

    double width() const { return 2. * margin + cols * cell_w; }
    double pitch() const { return thickness + gap; }
    double height() const { return base + rows * cell_h + margin; }
    // Cell rectangle {x0, y0, x1, y1}; row 0 is the top row.
    std::array<double, 4> cell_rect(int row, int col) const;
    int                   swatches_per_board() const;
    // Swatch slots in reading order (top-left to bottom-right): {row, col}.
    std::vector<std::array<int, 2>> swatch_cells() const;
};

// An 8-bit sRGB photo, row-major, 3 bytes per pixel.
struct PhotoImage
{
    int                        width{0};
    int                        height{0};
    std::vector<unsigned char> rgb;
};

struct PhotoReadOptions
{
    // Reflectance of the white sheet (plain office paper ~0.88). Its color is taken as neutral.
    double paper_reflectance{0.88};
    // Central part of each cell that is sampled (cells: at least 60 % of the height, so the average
    // covers many layer periods).
    double cell_fraction{0.5};
    // Flatbed scan: the board lies face-down on the glass (front or back face), inside a hole cut in a
    // sheet of white paper (or between paper strips).
    bool scanner{false};
};

struct PhotoReading
{
    std::vector<Lab>    cells;       // one per swatch slot, BoardLayout::swatch_cells() order
    std::vector<bool>   valid;       // per cell: false = not read (outside the photo); its Lab is meaningless
    std::vector<double> spread;      // per cell: spread of the sampled columns (CIEDE2000 to the median)
    int                 paper_samples{0};   // white paper points found around the board
    double              paper_gap{0.};      // mm between the board and the nearest usable paper
    double              paper_clipped{0.};  // share of the paper points that are pure white (255) in the image
    double              light_falloff{0.};  // brightest / darkest white reference
    int                 crushed_cells{0};   // swatch cells with many pure-black (0) pixel values: image too dark
    std::string         warning;            // non-empty when the photo looks unreliable
};

// Pixel positions of the face's corners: the corner next to the board's marker dots first, then
// clockwise (top-left, top-right, bottom-right, bottom-left as the board was printed).
// The back face works too: the cells go through the board, so it shows the same swatches mirrored.
// Seen from the back the dots are at the right end, so clicking from the dotted corner clockwise
// follows a short edge first; that order is recognized and mapped onto the front face.
using BoardCorners = std::array<std::array<double, 2>, 4>;

// Reads the swatch cells of one board. White reference: flat white paper around the board, found
// 3 to 40 mm from it (the brightest consistent, neutral points; the board's shadow, a scanner lid seen
// through a gap and colored things are left out). A bilinear illumination field fitted to it
// corrects exposure, white balance and uneven light. Throws std::runtime_error on unusable input
// (e.g. too little paper found).
PhotoReading read_board_photo(const PhotoImage &img, const BoardCorners &corners, const BoardLayout &layout,
                              const PhotoReadOptions &opt = {});

// TIFF (what many flatbed scanners save): baseline strips or tiles, 8 or 16 bits per sample, RGB(A) or
// grayscale, uncompressed, PackBits, LZW, Deflate (with or without the horizontal predictor) or JPEG
// (`decode_jpeg` decodes one JPEG stream, e.g. with the GUI toolkit; without it JPEG-compressed TIFFs
// are refused). Only the first image of the file is read.
using JpegDecoder = std::function<bool(const std::vector<unsigned char> &jpeg, PhotoImage &out)>;
bool is_tiff(const unsigned char *data, size_t size);
bool decode_tiff(const unsigned char *data, size_t size, PhotoImage &out, std::string *error = nullptr,
                 const JpegDecoder &decode_jpeg = {});

// Face coordinates (mm) -> image pixels.
std::array<double, 2> board_to_image(const BoardCorners &corners, const BoardLayout &layout, double x_mm, double y_mm);

} // namespace Slic3r::ColorMix
