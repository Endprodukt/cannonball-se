#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

#include "frontend/config.hpp"
#include "globals.hpp"
#include "sdl2/pixel_scaler_state.hpp"
#include "texture_export.hpp"
#include "texture_replacement_frame.hpp"

namespace texture_replacement
{
    inline texture_replacement_frame::Frame pending_frame;
    inline bool pending_frame_active = false;

    inline bool supported_by_current_renderer()
    {
        return !pixel_scaler::active(
            pixel_scaler::mode.load(std::memory_order_relaxed));
    }

    inline void begin_frame()
    {
        pending_frame = {};
        pending_frame.logical_width = config.s16_width;
        pending_frame.logical_height = config.s16_height;
        pending_frame_active = true;
    }

    inline void finalize_and_publish(uint16_t* final_pixels)
    {
        if (!pending_frame_active || !final_pixels)
            return;

        const size_t frame_pixels =
            static_cast<size_t>(pending_frame.logical_width) *
            pending_frame.logical_height;

        // Capture visibility from the untouched final native frame first.
        for (auto& command : pending_frame.commands)
        {
            if (command.width != pending_frame.logical_width ||
                command.height != pending_frame.logical_height ||
                command.expected_pixels.size() != frame_pixels ||
                command.restore_pixels.size() != frame_pixels)
            {
                command.visibility_mask.clear();
                continue;
            }

            command.visibility_mask.assign(frame_pixels, 0);
            for (size_t i = 0; i < frame_pixels; ++i)
            {
                const uint16_t expected = command.expected_pixels[i];
                if (expected != 0xffffu && final_pixels[i] == expected)
                    command.visibility_mask[i] = 255;
            }
        }

        // Remove only original pixels that still belong to a replacement. This
        // reveals the exact lower native layer underneath transparent HD pixels.
        for (const auto& command : pending_frame.commands)
        {
            if (command.visibility_mask.size() != frame_pixels ||
                command.restore_pixels.size() != frame_pixels)
            {
                continue;
            }

            for (size_t i = 0; i < frame_pixels; ++i)
            {
                if (command.visibility_mask[i] != 0)
                    final_pixels[i] = command.restore_pixels[i];
            }
        }

        texture_replacement_frame::publish_prepared(
            video.current_pixel_buffer,
            std::move(pending_frame));
        pending_frame = {};
        pending_frame_active = false;
    }

    inline bool prepare_road_background(
        texture_replacement_frame::DrawCommand& command,
        const uint16_t* restore_pixels,
        const uint16_t* ramBuff,
        uint8_t road_control,
        uint16_t color_offset3)
    {
        const uint32_t export_width = config.video.hires
            ? static_cast<uint32_t>(config.s16_width >> 1)
            : static_cast<uint32_t>(config.s16_width);
        const uint32_t export_height = S16_HEIGHT;

        if (!restore_pixels || export_width == 0 || export_height == 0)
            return false;

        std::array<int16_t, S16_HEIGHT> row_colours = {};
        uint32_t graphics_hash = 2166136261u;
        uint32_t palette_hash = 2166136261u;

        for (uint32_t y = 0; y < export_height; ++y)
        {
            const int data0 = ramBuff[0x000 + y];
            const int data1 = ramBuff[0x100 + y];
            int color = -1;

            switch (road_control & 3)
            {
                case 0:
                    if (data0 & 0x800) color = data0 & 0x7F;
                    break;
                case 1:
                    if (data0 & 0x800) color = data0 & 0x7F;
                    else if (data1 & 0x800) color = data1 & 0x7F;
                    break;
                case 2:
                    if (data1 & 0x800) color = data1 & 0x7F;
                    else if (data0 & 0x800) color = data0 & 0x7F;
                    break;
                case 3:
                    if (data1 & 0x800) color = data1 & 0x7F;
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
                palette_hash = texture_export::fnv1a_word(palette_hash, word);
            }
            else
            {
                palette_hash = texture_export::fnv1a_word(
                    palette_hash, 0xffffu);
            }
        }

        graphics_hash = texture_export::fnv1a_word(
            graphics_hash,
            static_cast<uint16_t>(road_control & 3));

        std::ostringstream key;
        key << "road_background"
            << "_w" << export_width
            << "_g" << std::hex << std::setfill('0') << std::setw(8)
            << graphics_hash
            << "_pal" << std::setw(8) << palette_hash;

        const std::filesystem::path path =
            std::filesystem::path("textures") / (key.str() + ".png");

        if (!texture_replacement_frame::replacement_exists(path.string()))
            return false;

        const int logical_width = config.s16_width;
        const int logical_height = config.s16_height;
        const int scale = config.video.hires ? 2 : 1;
        const size_t frame_pixels =
            static_cast<size_t>(logical_width) * logical_height;

        command = {};
        command.path = path.string();
        command.width = logical_width;
        command.height = logical_height;
        command.base_texture_width = export_width;
        command.base_texture_height = export_height;
        command.expected_pixels.assign(frame_pixels, 0xffffu);
        command.restore_pixels.assign(
            restore_pixels, restore_pixels + frame_pixels);

        // 21:9 stretches a centred strip of the completed road/tile background.
        if (config.video.widescreen == 2 && logical_width > 0)
        {
            const int overscan = 20 * scale;
            command.u0 = static_cast<float>(overscan) / logical_width;
            command.u1 = static_cast<float>(logical_width - overscan) /
                         logical_width;
        }

        for (int y = 0; y < logical_height; ++y)
        {
            const uint32_t native_y = static_cast<uint32_t>(y / scale);
            if (native_y >= export_height || row_colours[native_y] < 0)
                continue;

            const uint16_t expected = static_cast<uint16_t>(
                row_colours[native_y] | color_offset3);
            uint16_t* row = command.expected_pixels.data() +
                static_cast<size_t>(y) * logical_width;
            std::fill_n(row, logical_width, expected);
        }

        return true;
    }
}

inline void HWRoad::render_background_replacement_wrapper(uint16_t* buf)
{
    if (!texture_replacement::supported_by_current_renderer())
    {
        texture_replacement::pending_frame_active = false;
        texture_replacement::pending_frame = {};
        texture_replacement_frame::reset();
        (this->*render_background)(buf);
        return;
    }

    texture_replacement::begin_frame();

    texture_replacement_frame::DrawCommand command;
    const bool replacement = texture_replacement::prepare_road_background(
        command,
        buf,
        ramBuff,
        road_control,
        color_offset3);

    (this->*render_background)(buf);

    if (replacement)
        texture_replacement::pending_frame.commands.push_back(
            std::move(command));
}

inline void hwtiles::render_tile_layer_with_replacements(
    uint16_t* buf,
    uint8_t page_index,
    uint8_t priority_draw)
{
    // Only the large native background tilemap is wired for this first test.
    if (page_index != 1 || priority_draw != 0 ||
        !texture_replacement::pending_frame_active)
    {
        render_tile_layer(buf, page_index, priority_draw);
        return;
    }

    constexpr uint32_t MAP_WIDTH = 1024;
    constexpr uint32_t MAP_HEIGHT = 512;

    const uint16_t page_select = page[page_index];
    const std::array<uint8_t, 4> selected_pages = {
        static_cast<uint8_t>((page_select >> 0) & 0x0F),
        static_cast<uint8_t>((page_select >> 4) & 0x0F),
        static_cast<uint8_t>((page_select >> 8) & 0x0F),
        static_cast<uint8_t>((page_select >> 12) & 0x0F)
    };

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

    std::ostringstream key;
    key << "background"
        << "_pages" << std::hex << std::setfill('0') << std::setw(4)
        << page_select
        << "_g" << std::setw(8) << graphics_hash
        << "_pal" << std::setw(8) << palette_hash;

    const std::filesystem::path path =
        std::filesystem::path("textures") / (key.str() + ".png");

    if (!texture_replacement_frame::replacement_exists(path.string()))
    {
        render_tile_layer(buf, page_index, priority_draw);
        return;
    }

    uint16_t xScroll = scroll_x[page_index];
    uint16_t yScroll = scroll_y[page_index];

    // Match the current native renderer: it resolves the first row/column entry
    // once for the whole pass when those scroll modes are enabled.
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
    command.path = path.string();
    command.width = logical_width;
    command.height = logical_height;
    command.base_texture_width = MAP_WIDTH;
    command.base_texture_height = MAP_HEIGHT;
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
                 static_cast<float>(MAP_WIDTH);
    command.u1 = (x_decrement + native_left + native_span) /
                 static_cast<float>(MAP_WIDTH);
    command.v0 = y_decrement / static_cast<float>(MAP_HEIGHT);
    command.v1 = (y_decrement +
        (logical_height / static_cast<float>(scale))) /
        static_cast<float>(MAP_HEIGHT);

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
    texture_replacement::pending_frame.commands.push_back(
        std::move(command));
}

inline void hwtiles::render_text_layer_with_replacements(
    uint16_t* buf,
    uint8_t priority_draw)
{
    render_text_layer(buf, priority_draw);

    // This is the final native layer in Video::prepare_frame().
    if (priority_draw == 1)
        texture_replacement::finalize_and_publish(buf);
}
