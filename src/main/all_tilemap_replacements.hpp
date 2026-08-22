#pragma once

#include <array>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

// Replacement wrapper for both scrolling System-16 tilemap layers. It uses the
// compact repeat unit derived by compact_tilemap.hpp, but ownership is still
// calculated against the complete virtual 1024x512 map.
inline void hwtiles::render_tile_layer_with_all_replacements(
    uint16_t* buf,
    uint8_t page_index,
    uint8_t priority_draw)
{
    if (page_index > 1 || priority_draw != 0 ||
        !texture_replacement::pending_frame_active)
    {
        render_tile_layer(buf, page_index, priority_draw);
        return;
    }

    const char* layer_name = page_index == 0 ? "foreground" : "background";
    const uint16_t page_select = page[page_index];
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

    auto make_key = [&](const char* name, bool include_palette)
    {
        std::ostringstream stream;
        stream << name
               << "_pages" << std::hex << std::setfill('0') << std::setw(4)
               << page_select
               << "_g" << std::setw(8) << graphics_hash
               << "_pal";
        if (include_palette)
            stream << std::setw(8) << palette_hash;
        return stream.str();
    };

    const std::string exact_key = make_key(layer_name, true);
    const std::string stable_prefix = make_key(layer_name, false);
    std::filesystem::path exact_path =
        std::filesystem::path("textures") / (exact_key + ".png");
    std::string resolved_path = texture_replacement::resolve_palette_variant(
        exact_path, stable_prefix);

    // The exporter deduplicates identical foreground/background maps by keeping
    // the background filename. Preserve support for an older foreground file,
    // but transparently use the shared background asset when both page selectors
    // are identical and no foreground-specific replacement exists.
    if (resolved_path.empty() && page_index == 0 && page[0] == page[1])
    {
        const std::string shared_exact = make_key("background", true);
        const std::string shared_prefix = make_key("background", false);
        exact_path = std::filesystem::path("textures") /
                     (shared_exact + ".png");
        resolved_path = texture_replacement::resolve_palette_variant(
            exact_path, shared_prefix);
    }

    if (resolved_path.empty())
    {
        render_tile_layer(buf, page_index, priority_draw);
        return;
    }

    uint16_t xScroll = scroll_x[page_index];
    uint16_t yScroll = scroll_y[page_index];

    if ((xScroll & 0x8000) != 0)
        xScroll = (text_ram[0xf80 + (0x40 * page_index) + 0] << 8) |
                  text_ram[0xf80 + (0x40 * page_index) + 1];
    if ((yScroll & 0x8000) != 0)
        yScroll = (text_ram[0xf16 + (0x40 * page_index) + 0] << 8) |
                  text_ram[0xf16 + (0x40 * page_index) + 1];

    const int x_decrement = (x_clamp - xScroll) & 0x3ff;
    const int y_decrement = yScroll & 0x1ff;
    const int logical_width = config.s16_width;
    const int logical_height = config.s16_height;
    const int scale = config.video.hires ? 2 : 1;
    const size_t frame_pixels =
        static_cast<size_t>(logical_width) * logical_height;

    texture_replacement_frame::DrawCommand command;
    command.path = resolved_path;
    command.width = logical_width;
    command.height = logical_height;
    command.base_texture_width = layout.width;
    command.base_texture_height = layout.height;
    command.repeat = true;
    command.expected_pixels.assign(frame_pixels, 0xffffu);
    command.restore_pixels.assign(buf, buf + frame_pixels);

    int source_left_internal = 0;
    int source_width_internal = logical_width;
    if (config.video.widescreen == 2 && s16_width_noscale > 512)
    {
        const int overscan = 20 * scale;
        source_left_internal = overscan;
        source_width_internal = logical_width - (overscan << 1);
    }

    const float native_left =
        static_cast<float>(source_left_internal) / scale;
    const float native_span =
        static_cast<float>(source_width_internal) / scale;

    command.u0 = (x_decrement + native_left) /
                 static_cast<float>(layout.width);
    command.u1 = (x_decrement + native_left + native_span) /
                 static_cast<float>(layout.width);
    command.v0 = y_decrement / static_cast<float>(layout.height);
    command.v1 = (y_decrement +
        (logical_height / static_cast<float>(scale))) /
        static_cast<float>(layout.height);

    for (int screen_y = 0; screen_y < logical_height; ++screen_y)
    {
        const int native_y = screen_y / scale;
        const uint32_t map_y =
            static_cast<uint32_t>(native_y + y_decrement) & 0x1ffu;

        for (int screen_x = 0; screen_x < logical_width; ++screen_x)
        {
            int source_x_internal = screen_x;
            if (config.video.widescreen == 2 &&
                s16_width_noscale > 512 && logical_width > 1)
            {
                source_x_internal = source_left_internal +
                    ((source_width_internal - 1) * screen_x) /
                    (logical_width - 1);
            }

            const int native_x = source_x_internal / scale;
            const uint32_t map_x =
                static_cast<uint32_t>(native_x + x_decrement) & 0x3ffu;

            const uint32_t mx = map_x >> 3;
            const uint32_t my = map_y >> 3;
            const uint32_t quad =
                ((my >= 32u) ? 2u : 0u) |
                ((mx >= 64u) ? 1u : 0u);

            const uint8_t page_id = selected_pages[quad];
            const uint32_t tile_index =
                (static_cast<uint32_t>(page_id) << 12) |
                ((my & 31u) << 7) |
                ((mx & 63u) << 1);

            const uint16_t data = static_cast<uint16_t>(
                (static_cast<uint16_t>(tile_ram[tile_index]) << 8) |
                tile_ram[tile_index + 1]);

            if ((data & 0x8000u) != 0)
                continue;

            uint32_t code = data & 0x1fffu;
            code = (static_cast<uint32_t>(tile_banks[code >> 12]) << 12) |
                   (code & 0x0fffu);
            code &= (NUM_TILES - 1);
            if (code == 0)
                continue;

            const uint32_t row = tiles[(code << 3) + (map_y & 7u)];
            const uint8_t pixel = static_cast<uint8_t>(
                (row >> (28 - ((map_x & 7u) * 4))) & 0x0fu);
            if (pixel == 0)
                continue;

            const uint16_t colour =
                static_cast<uint16_t>((data >> 6) & 0x7fu);
            command.expected_pixels[
                static_cast<size_t>(screen_y) * logical_width + screen_x] =
                    static_cast<uint16_t>((colour << 3) + pixel);
        }
    }

    render_tile_layer(buf, page_index, priority_draw);
    texture_replacement::pending_frame.commands.push_back(std::move(command));
}
