#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "compact_tilemap.hpp"

namespace tilemap_export_dedup
{
    inline void redirect_manifest_entry(const std::filesystem::path& manifest_path,
                                        const std::string& old_filename,
                                        const std::string& new_filename)
    {
        std::ifstream input(manifest_path);
        if (!input)
            return;

        std::vector<std::string> lines;
        std::string line;
        const std::string old_prefix = old_filename + ",foreground,";

        while (std::getline(input, line))
        {
            if (line.rfind(old_prefix, 0) == 0)
                line.replace(0, old_filename.size(), new_filename);
            lines.push_back(std::move(line));
        }
        input.close();

        std::ofstream output(manifest_path, std::ios::trunc);
        if (!output)
            return;

        for (const std::string& manifest_line : lines)
            output << manifest_line << '\n';
    }
}

// Keep the compact exporter as the source of truth, then collapse the special
// case where foreground and background select exactly the same four pages. Both
// layers share tile RAM, tile banks and palette data, so equal page-select words
// guarantee an identical exported image. The background filename is retained as
// the canonical asset because the replacement renderer already consumes it.
inline void hwtiles::export_deduplicated_layers()
{
    export_compact_layers();

    if (page[0] != page[1])
        return;

    const uint16_t page_select = page[0];
    const std::array<uint8_t, 4> selected_pages = {
        static_cast<uint8_t>((page_select >> 0) & 0x0F),
        static_cast<uint8_t>((page_select >> 4) & 0x0F),
        static_cast<uint8_t>((page_select >> 8) & 0x0F),
        static_cast<uint8_t>((page_select >> 12) & 0x0F)
    };
    const compact_tilemap::layout_t layout =
        compact_tilemap::derive_layout(selected_pages);

    uint32_t palette_hash = 2166136261u;
    for (uint32_t colour = 0; colour < 128; ++colour)
    {
        for (uint32_t pixel = 0; pixel < 8; ++pixel)
        {
            const uint16_t word =
                video.read_pal16(((colour << 3) + pixel) * 2);
            palette_hash = texture_export::fnv1a_word(palette_hash, word);
        }
    }

    uint32_t graphics_hash = 2166136261u;
    graphics_hash = texture_export::fnv1a_word(graphics_hash, page_select);
    graphics_hash = texture_export::fnv1a_bytes(
        tile_banks, sizeof(tile_banks), graphics_hash);

    for (uint8_t page_id : selected_pages)
    {
        const size_t page_offset = static_cast<size_t>(page_id) << 12;
        graphics_hash = texture_export::fnv1a_bytes(
            tile_ram + page_offset, 0x1000, graphics_hash);
    }

    std::ostringstream suffix_stream;
    suffix_stream << "_pages" << std::hex << std::setfill('0')
                  << std::setw(4) << page_select
                  << "_g" << std::setw(8) << graphics_hash
                  << "_pal" << std::setw(8) << palette_hash;
    const std::string suffix = suffix_stream.str();

    static std::unordered_set<std::string> deduplicated;
    if (deduplicated.contains(suffix))
        return;

    const std::filesystem::path export_dir = "texture_export";
    const std::filesystem::path foreground_path =
        export_dir / ("foreground" + suffix + ".png");
    const std::filesystem::path background_path =
        export_dir / ("background" + suffix + ".png");

    // The compact exporter may not have reached its 15-frame stability guard
    // yet. Retry on later frames until both candidate files actually exist.
    if (!std::filesystem::exists(foreground_path) ||
        !std::filesystem::exists(background_path))
    {
        return;
    }

    // Refuse to collapse stale files from an older exporter layout.
    if (!compact_tilemap::png_has_dimensions(
            foreground_path, layout.width, layout.height) ||
        !compact_tilemap::png_has_dimensions(
            background_path, layout.width, layout.height))
    {
        return;
    }

    std::error_code ec;
    std::filesystem::remove(foreground_path, ec);
    if (ec)
    {
        std::cerr << "Unable to remove duplicate foreground tilemap export: "
                  << foreground_path.string() << "\n";
        return;
    }

    // Preserve both logical layer mappings in the CSV, but point the foreground
    // row at the one canonical background PNG instead of a now-deleted duplicate.
    tilemap_export_dedup::redirect_manifest_entry(
        export_dir / "tilemap_manifest.csv",
        foreground_path.filename().string(),
        background_path.filename().string());

    deduplicated.insert(suffix);
    std::cout << "Deduplicated identical tilemap export: "
              << foreground_path.filename().string() << " -> "
              << background_path.filename().string() << "\n";
}
