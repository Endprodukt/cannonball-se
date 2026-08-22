#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "frontend/config.hpp"
#include "texture_export.hpp"

// Export the road chip's solid-fill background separately from tilemaps.
// This is where OutRun's large sky areas come from; they are generated per
// scanline from road RAM and therefore never appear in the 8x8 tile graphics.
inline void HWRoad::export_background_layer()
{
    constexpr int STABLE_FRAMES = 15;

    static uint64_t pending_fingerprint = 0;
    static int stable_frames = 0;
    static std::string last_exported_key;

    const uint32_t width = config.video.hires
        ? static_cast<uint32_t>(config.s16_width >> 1)
        : static_cast<uint32_t>(config.s16_width);
    const uint32_t height = S16_HEIGHT;

    if (width == 0 || height == 0)
        return;

    std::array<int16_t, S16_HEIGHT> row_colours = {};
    std::array<uint16_t, S16_HEIGHT> palette_words = {};

    uint32_t graphics_hash = 2166136261u;
    uint32_t palette_hash = 2166136261u;

    for (uint32_t y = 0; y < height; ++y)
    {
        const int data0 = ramBuff[0x000 + y];
        const int data1 = ramBuff[0x100 + y];

        int color = -1;

        switch (road_control & 3)
        {
            case 0:
                if (data0 & 0x800)
                    color = data0 & 0x7F;
                break;

            case 1:
                if (data0 & 0x800)
                    color = data0 & 0x7F;
                else if (data1 & 0x800)
                    color = data1 & 0x7F;
                break;

            case 2:
                if (data1 & 0x800)
                    color = data1 & 0x7F;
                else if (data0 & 0x800)
                    color = data0 & 0x7F;
                break;

            case 3:
                if (data1 & 0x800)
                    color = data1 & 0x7F;
                break;
        }

        row_colours[y] = static_cast<int16_t>(color);
        graphics_hash = texture_export::fnv1a_word(
            graphics_hash,
            static_cast<uint16_t>(color + 1));

        if (color >= 0)
        {
            const uint16_t palette_index = static_cast<uint16_t>(
                color | color_offset3);
            const uint16_t word = video.read_pal16(
                static_cast<uint32_t>(palette_index) * 2);
            palette_words[y] = word;
            palette_hash = texture_export::fnv1a_word(palette_hash, word);
        }
        else
        {
            palette_hash = texture_export::fnv1a_word(palette_hash, 0xFFFFu);
        }
    }

    graphics_hash = texture_export::fnv1a_word(
        graphics_hash,
        static_cast<uint16_t>(road_control & 3));

    const uint64_t fingerprint =
        (static_cast<uint64_t>(graphics_hash) << 32) | palette_hash;

    if (pending_fingerprint != fingerprint)
    {
        pending_fingerprint = fingerprint;
        stable_frames = 1;
        return;
    }

    if (stable_frames < STABLE_FRAMES)
    {
        ++stable_frames;
        if (stable_frames < STABLE_FRAMES)
            return;
    }

    std::ostringstream key_stream;
    key_stream << "road_background"
               << "_w" << width
               << "_g" << std::hex << std::setfill('0') << std::setw(8) << graphics_hash
               << "_pal" << std::setw(8) << palette_hash;
    const std::string key = key_stream.str();

    if (key == last_exported_key)
        return;

    const std::filesystem::path export_dir =
        std::filesystem::path("background_export") / "road";

    std::error_code ec;
    std::filesystem::create_directories(export_dir, ec);
    if (ec)
    {
        std::cerr << "Unable to create road background export directory: "
                  << ec.message() << "\n";
        return;
    }

    const std::filesystem::path filename = export_dir / (key + ".png");
    if (std::filesystem::exists(filename))
    {
        last_exported_key = key;
        return;
    }

    std::vector<texture_export::rgba_t> rgba(
        static_cast<size_t>(width) * height,
        { 0, 0, 0, 0 });

    for (uint32_t y = 0; y < height; ++y)
    {
        if (row_colours[y] < 0)
            continue;

        const texture_export::rgba_t colour =
            texture_export::palette_word_to_rgba(palette_words[y]);

        auto* row = rgba.data() + static_cast<size_t>(y) * width;
        std::fill_n(row, width, colour);
    }

    if (!texture_export::write_rgba_png(filename, width, height, rgba))
    {
        std::cerr << "Failed to export road background PNG: "
                  << filename.string() << "\n";
        return;
    }

    const std::filesystem::path manifest_path = export_dir / "manifest.csv";
    const bool new_manifest =
        !std::filesystem::exists(manifest_path) ||
        std::filesystem::file_size(manifest_path) == 0;

    std::ofstream manifest(manifest_path, std::ios::app);
    if (manifest)
    {
        if (new_manifest)
            manifest << "file,width,height,road_control,graphics_hash,palette_hash\n";

        manifest << filename.filename().string() << ','
                 << width << ',' << height << ','
                 << static_cast<unsigned>(road_control & 3) << ','
                 << std::hex << std::setw(8) << std::setfill('0') << graphics_hash << ','
                 << std::setw(8) << palette_hash << std::dec << '\n';
    }

    last_exported_key = key;
    std::cout << "Exported road background: " << filename.string() << "\n";
}
