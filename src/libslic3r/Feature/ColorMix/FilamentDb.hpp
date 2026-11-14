///|/ ColorMax engine - looking up filaments in public filament databases.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#pragma once

#include <string>
#include <vector>

namespace Slic3r::ColorMix {

// One filament of a public database, reduced to what a filament set needs.
// Sources:
//  - filamentcolors.xyz (https://filamentcolors.xyz, data CC BY 4.0): printed swatches measured with a
//    colorimeter (CIE Lab), many with a TD (transmission distance, as used by HueForge).
//  - SpoolmanDB (https://github.com/Donkie/SpoolmanDB, MIT): a large catalog with the manufacturers'
//    colors (not measured), a translucent flag and finish / pattern.
struct FilamentRecord
{
    std::string source;        // "filamentcolors.xyz" or "SpoolmanDB"
    std::string manufacturer;
    std::string name;          // color name, e.g. "Galaxy Black"
    std::string material;      // e.g. "PLA", "PETG"
    std::string color;         // "#RRGGBB"
    bool        measured{false}; // the color was measured on a printed swatch
    double      td{0.};        // transmission distance in mm (0 = unknown)
    bool        translucent{false};
    bool        effect{false}; // glitter / sparkle / silk / marble / multicolor: prints solid only
    std::string url;           // where the record comes from (shown to the user)
};

// One page of the filamentcolors.xyz swatch API ({"next": ..., "results": [...]}) or a plain array of
// swatches. `next_page` (optional) receives the URL of the next page ("" = last page).
std::vector<FilamentRecord> parse_filamentcolors(const std::string &json, std::string *next_page = nullptr);
// SpoolmanDB's compiled filaments.json. Records that differ only in spool weight or diameter are merged.
std::vector<FilamentRecord> parse_spoolmandb(const std::string &json);
// Both lists in one: a catalog record of a filament that was also measured (same manufacturer, name and
// material) is dropped, measured records first.
std::vector<FilamentRecord> merge_filament_records(std::vector<FilamentRecord> measured, const std::vector<FilamentRecord> &catalog);

// The local copy of the database (records plus the date it was downloaded).
std::string                 filament_db_to_json(const std::vector<FilamentRecord> &records, const std::string &downloaded);
std::vector<FilamentRecord> filament_db_from_json(const std::string &json, std::string *downloaded = nullptr);

// Indices of the records matching every word of `query` (case-insensitive, in manufacturer, name,
// material or "measured"), best first: measured before catalog, then names starting with a word.
std::vector<size_t> search_filaments(const std::vector<FilamentRecord> &records, const std::string &query, size_t max_results = 300);

// URLs of the sources.
inline const char *FILAMENTCOLORS_SWATCH_URL = "https://filamentcolors.xyz/api/swatch/?format=json&page_size=100";
inline const char *SPOOLMANDB_URL            = "https://donkie.github.io/SpoolmanDB/filaments.json";

} // namespace Slic3r::ColorMix
