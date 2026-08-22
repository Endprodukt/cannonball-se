#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
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

    // Missing exact palette variants are resolved once per run to another PNG
    // with the same stable graphics identity. This prevents palette animation or
    // fades from alternating between the HD texture and the original artwork.
    inline std::unordered_map<std::string, std::string> stable_path_cache;

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

    inline std::string resolve_palette_variant(
        const std::filesystem::path& exact_path,
        const std::string& stable_filename_prefix,
        const std::string& required_suffix = {})
    {
        if (texture_replacement_frame::replacement_exists(exact_path.string()))
            return exact_path.string();

        const std::string cache_key =
            (exact_path.parent_path() / stable_filename_prefix).string() +
            "|" + required_suffix;
        const auto cached = stable_path_cache.find(cache_key);
        if (cached != stable_path_cache.end())
            return cached->second;

        std::string resolved;
        std::error_code ec;
        const std::filesystem::path directory = exact_path.parent_path();
        if (std::filesystem::is_directory(directory, ec) && !ec)
        {
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                if (ec || !entry.is_regular_file())
                    continue;

                const std::string name = entry.path().filename().string();
                if (!name.starts_with(stable_filename_prefix) ||
                    entry.path().extension() != ".png")
                {
                    continue;
                }

                if (!required_suffix.empty())
                {
                    if (!name.ends_with(required_suffix + ".png"))
                        continue;
                }
                else if (name.find("_shadow.png") != std::string::npos)
                {
                    continue;
                }

                if (resolved.empty() || entry.path().string() < resolved)
                    resolved = entry.path().string();
            }
        }

        stable_path_cache.emplace(cache_key, resolved);
        if (!resolved.empty())
        {
            std::cout << "Using stable texture replacement across palette variants: "
                      << resolved << "\n";
        }
        return resolved;
    }

    inline void finalize_and_publish(uint16_t* final_pixels)
    {
        if (!pending_frame_active || !final_pixels)
            return;

        const int frame_width = pending_frame.logical_width;
        const int frame_height = pending_frame.logical_height;

        // Capture all ownership masks from the untouched final native frame.
        // Commands may be full-screen layers or small sprite rectangles.
        for (auto& command : pending_frame.commands)
        {
            if (command.width <= 0 || command.height <= 0)
            {
                command.visibility_mask.clear();
                continue;
            }

            const size_t command_pixels =
                static_cast<size_t>(command.width) * command.height;
            if (command.restore_pixels.size() != command_pixels ||
                (!command.sprite_palette_ownership &&
                 command.expected_pixels.size() != command_pixels))
            {
                command.visibility_mask.clear();
                continue;
            }

            command.visibility_mask.assign(command_pixels, 0);

            for (int local_y = 0; local_y < command.height; ++local_y)
            {
                const int screen_y = command.y + local_y;
                if (screen_y < 0 || screen_y >= frame_height)
                    continue;

                for (int local_x = 0; local_x < command.width; ++local_x)
                {
                    const int screen_x = command.x + local_x;
                    if (screen_x < 0 || screen_x >= frame_width)
                        continue;

                    const size_t local_index =
                        static_cast<size_t>(local_y) * command.width + local_x;
                    const size_t screen_index =
                        static_cast<size_t>(screen_y) * frame_width + screen_x;
                    const uint16_t indexed = final_pixels[screen_index];

                    bool visible = false;
                    if (command.sprite_palette_ownership)
                    {
                        visible =
                            (indexed & 0x0ff0u) == command.sprite_palette_base;
                        if (command.sprite_shadow && (indexed & 0x1000u) != 0)
                            visible = true;
                    }
                    else
                    {
                        const uint16_t expected = command.expected_pixels[local_index];
                        visible = expected != 0xffffu && indexed == expected;
                    }

                    if (visible)
                        command.visibility_mask[local_index] = 255;
                }
            }
        }

        // Remove only original pixels that are still owned by each replacement.
        // Later native layers remain untouched because their ownership mask is 0.
        for (const auto& command : pending_frame.commands)
        {
            const size_t command_pixels =
                static_cast<size_t>(std::max(0, command.width)) *
                static_cast<size_t>(std::max(0, command.height));
            if (command.visibility_mask.size() != command_pixels ||
                command.restore_pixels.size() != command_pixels)
            {
                continue;
            }

            for (int local_y = 0; local_y < command.height; ++local_y)
            {
                const int screen_y = command.y + local_y;
                if (screen_y < 0 || screen_y >= frame_height)
                    continue;

                for (int local_x = 0; local_x < command.width; ++local_x)
                {
                    const int screen_x = command.x + local_x;
                    if (screen_x < 0 || screen_x >= frame_width)
                        continue;

                    const size_t local_index =
                        static_cast<size_t>(local_y) * command.width + local_x;
                    if (command.visibility_mask[local_index] == 0)
                        continue;

                    const size_t screen_index =
                        static_cast<size_t>(screen_y) * frame_width + screen_x;
                    final_pixels[screen_index] = command.restore_pixels[local_index];
                }
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

        std::ostringstream stable_prefix;
        stable_prefix << "road_background"
                      << "_w" << export_width
                      << "_g" << std::hex << std::setfill('0') << std::setw(8)
                      << graphics_hash
                      << "_pal";

        const std::filesystem::path exact_path =
            std::filesystem::path("textures") / (key.str() + ".png");
        const std::string resolved_path = resolve_palette_variant(
            exact_path, stable_prefix.str());
        if (resolved_path.empty())
            return false;

        const int logical_width = config.s16_width;
        const int logical_height = config.s16_height;
        const int scale = config.video.hires ? 2 : 1;
        const size_t frame_pixels =
            static_cast<size_t>(logical_width) * logical_height;

        command = {};
        command.path = resolved_path;
        command.width = logical_width;
        command.height = logical_height;
        command.base_texture_width = export_width;
        command.base_texture_height = export_height;
        command.expected_pixels.assign(frame_pixels, 0xffffu);
        command.restore_pixels.assign(frame_pixels, 0u);

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

// Legacy full-map wrapper retained for source compatibility. The active branch
// redirects Video::prepare_frame() to the compact wrapper in compact_tilemap.hpp.
inline void hwtiles::render_tile_layer_with_replacements(
    uint16_t* buf,
    uint8_t page_index,
    uint8_t priority_draw)
{
    render_tile_layer(buf, page_index, priority_draw);
}

inline void hwtiles::render_text_layer_with_replacements(
    uint16_t* buf,
    uint8_t priority_draw)
{
    render_text_layer(buf, priority_draw);

    if (priority_draw == 1)
        texture_replacement::finalize_and_publish(buf);
}
