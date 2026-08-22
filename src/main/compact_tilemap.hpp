#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace compact_tilemap
{
    constexpr uint32_t PAGE_WIDTH = 512;
    constexpr uint32_t PAGE_HEIGHT = 256;
    constexpr uint32_t VIRTUAL_WIDTH = 1024;
    constexpr uint32_t VIRTUAL_HEIGHT = 512;

    struct layout_t
    {
        uint32_t pages_x = 2;
        uint32_t pages_y = 2;
        uint32_t width = VIRTUAL_WIDTH;
        uint32_t height = VIRTUAL_HEIGHT;
    };

    inline layout_t derive_layout(const std::array<uint8_t, 4>& pages)
    {
        const bool repeats_x = pages[0] == pages[1] && pages[2] == pages[3];
        const bool repeats_y = pages[0] == pages[2] && pages[1] == pages[3];

        layout_t layout;
        layout.pages_x = repeats_x ? 1u : 2u;
        layout.pages_y = repeats_y ? 1u : 2u;
        layout.width = layout.pages_x * PAGE_WIDTH;
        layout.height = layout.pages_y * PAGE_HEIGHT;
        return layout;
    }

    inline uint32_t read_be32(const uint8_t* p)
    {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }

    inline bool png_has_dimensions(const std::filesystem::path& path,
                                   uint32_t width,
                                   uint32_t height)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;

        std::array<uint8_t, 24> header = {};
        file.read(reinterpret_cast<char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
        if (file.gcount() != static_cast<std::streamsize>(header.size()))
            return false;

        static constexpr uint8_t signature[8] = {
            137, 80, 78, 71, 13, 10, 26, 10
        };
        for (size_t i = 0; i < 8; ++i)
        {
            if (header[i] != signature[i])
                return false;
        }

        return read_be32(header.data() + 16) == width &&
               read_be32(header.data() + 20) == height;
    }
}

// Export the smallest axis-aligned repeat unit of the four-page System-16 map.
// A page-select such as ffff therefore becomes one 512x256 image instead of four
// identical quadrants. The filename/hash stays compatible with the existing
// exporter; only the PNG dimensions are compacted when repetition permits it.
inline void hwtiles::export_compact_layers()
{
    constexpr int STABLE_FRAMES = 15;

    static bool export_dir_ready = false;
    static std::unordered_set<std::string> exported;
    static std::array<uint64_t, 2> pending_fingerprint = { 0, 0 };
    static std::array<int, 2> stable_frames = { 0, 0 };

    if (!export_dir_ready)
    {
        std::error_code ec;
        std::filesystem::create_directories("texture_export", ec);
        if (ec)
        {
            std::cerr << "Unable to create texture_export directory: "
                      << ec.message() << "\n";
            return;
        }
        export_dir_ready = true;
    }

    std::array<std::array<texture_export::rgba_t, 8>, 128> palette_rgba = {};
    uint32_t palette_hash = 2166136261u;
    for (uint32_t colour = 0; colour < 128; ++colour)
    {
        for (uint32_t pixel = 0; pixel < 8; ++pixel)
        {
            const uint16_t word = video.read_pal16(((colour << 3) + pixel) * 2);
            palette_rgba[colour][pixel] = texture_export::palette_word_to_rgba(word);
            palette_hash = texture_export::fnv1a_word(palette_hash, word);
        }
    }

    for (uint32_t layer_index = 0; layer_index < 2; ++layer_index)
    {
        const char* layer_name = layer_index == 0 ? "foreground" : "background";
        const uint16_t page_select = page[layer_index];

        const std::array<uint8_t, 4> selected_pages = {
            static_cast<uint8_t>((page_select >> 0) & 0x0F),
            static_cast<uint8_t>((page_select >> 4) & 0x0F),
            static_cast<uint8_t>((page_select >> 8) & 0x0F),
            static_cast<uint8_t>((page_select >> 12) & 0x0F)
        };
        const compact_tilemap::layout_t layout =
            compact_tilemap::derive_layout(selected_pages);

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

        const uint64_t fingerprint =
            (static_cast<uint64_t>(graphics_hash) << 32) | palette_hash;

        if (pending_fingerprint[layer_index] != fingerprint)
        {
            pending_fingerprint[layer_index] = fingerprint;
            stable_frames[layer_index] = 1;
            continue;
        }

        if (stable_frames[layer_index] < STABLE_FRAMES)
        {
            ++stable_frames[layer_index];
            if (stable_frames[layer_index] < STABLE_FRAMES)
                continue;
        }

        std::ostringstream key_stream;
        key_stream << layer_name
                   << "_pages" << std::hex << std::setfill('0')
                   << std::setw(4) << page_select
                   << "_g" << std::setw(8) << graphics_hash
                   << "_pal" << std::setw(8) << palette_hash;
        const std::string key = key_stream.str();

        if (!exported.insert(key).second)
            continue;

        const std::filesystem::path filename =
            std::filesystem::path("texture_export") / (key + ".png");

        if (std::filesystem::exists(filename))
        {
            if (compact_tilemap::png_has_dimensions(
                    filename, layout.width, layout.height))
            {
                continue;
            }

            // Migrate an older 1024x512 four-quadrant export in place.
            std::error_code remove_ec;
            std::filesystem::remove(filename, remove_ec);
            if (remove_ec)
            {
                std::cerr << "Unable to replace old tilemap export: "
                          << filename.string() << "\n";
                continue;
            }
        }

        std::vector<texture_export::rgba_t> rgba(
            static_cast<size_t>(layout.width) * layout.height,
            { 0, 0, 0, 0 });

        for (uint32_t page_y = 0; page_y < layout.pages_y; ++page_y)
        {
            for (uint32_t page_x = 0; page_x < layout.pages_x; ++page_x)
            {
                const uint32_t source_quad = page_y * 2u + page_x;
                const uint8_t page_id = selected_pages[source_quad];
                const uint32_t dst_base_x =
                    page_x * compact_tilemap::PAGE_WIDTH;
                const uint32_t dst_base_y =
                    page_y * compact_tilemap::PAGE_HEIGHT;

                for (uint32_t tile_y = 0; tile_y < 32; ++tile_y)
                {
                    for (uint32_t tile_x = 0; tile_x < 64; ++tile_x)
                    {
                        const uint32_t tile_index =
                            (static_cast<uint32_t>(page_id) << 12) |
                            (tile_y << 7) |
                            (tile_x << 1);

                        const uint16_t data = static_cast<uint16_t>(
                            (static_cast<uint16_t>(tile_ram[tile_index]) << 8) |
                            tile_ram[tile_index + 1]);

                        if ((data & 0x8000u) != 0)
                            continue;

                        uint32_t code = data & 0x1FFFu;
                        code = (static_cast<uint32_t>(tile_banks[code >> 12]) << 12) |
                               (code & 0x0FFFu);
                        code &= (NUM_TILES - 1);
                        if (code == 0)
                            continue;

                        const uint8_t colour =
                            static_cast<uint8_t>((data >> 6) & 0x7Fu);
                        const uint32_t* tile_data = tiles + (code << 3);
                        const uint32_t pixel_base_x = dst_base_x + tile_x * 8;
                        const uint32_t pixel_base_y = dst_base_y + tile_y * 8;

                        for (uint32_t y = 0; y < 8; ++y)
                        {
                            const uint32_t row = tile_data[y];
                            for (uint32_t x = 0; x < 8; ++x)
                            {
                                const uint8_t pixel = static_cast<uint8_t>(
                                    (row >> (28 - x * 4)) & 0x0Fu);
                                if (pixel == 0)
                                    continue;

                                const size_t out_index =
                                    static_cast<size_t>(pixel_base_y + y) *
                                    layout.width + (pixel_base_x + x);
                                rgba[out_index] =
                                    palette_rgba[colour][pixel & 7u];
                            }
                        }
                    }
                }
            }
        }

        if (!texture_export::write_rgba_png(
                filename, layout.width, layout.height, rgba))
        {
            std::cerr << "Failed to export compact tilemap PNG: "
                      << filename.string() << "\n";
            continue;
        }

        const std::filesystem::path manifest_path =
            std::filesystem::path("texture_export") / "tilemap_manifest.csv";
        const bool new_manifest =
            !std::filesystem::exists(manifest_path) ||
            std::filesystem::file_size(manifest_path) == 0;
        std::ofstream manifest(manifest_path, std::ios::app);
        if (manifest)
        {
            if (new_manifest)
            {
                manifest << "file,layer,page_select,top_left,top_right,bottom_left,bottom_right,graphics_hash,palette_hash,width,height\n";
            }

            manifest << filename.filename().string() << ','
                     << layer_name << ','
                     << std::hex << std::setw(4) << std::setfill('0')
                     << page_select << ','
                     << static_cast<unsigned>(selected_pages[0]) << ','
                     << static_cast<unsigned>(selected_pages[1]) << ','
                     << static_cast<unsigned>(selected_pages[2]) << ','
                     << static_cast<unsigned>(selected_pages[3]) << ','
                     << std::setw(8) << graphics_hash << ','
                     << std::setw(8) << palette_hash << std::dec << ','
                     << layout.width << ',' << layout.height << '\n';
        }

        std::cout << "Exported compact " << layer_name << " tilemap: "
                  << filename.string() << " (" << layout.width << 'x'
                  << layout.height << ")\n";
    }
}

// Replacement equivalent of render_tile_layer_with_replacements(), but using
// the same compact repeat unit as the exporter. Ownership is still calculated
// against the full virtual 1024x512 map so native layer ordering is unchanged.
inline void hwtiles::render_tile_layer_with_compact_replacements(
    uint16_t* buf,
    uint8_t page_index,
    uint8_t priority_draw)
{
    if (page_index != 1 || priority_draw != 0 ||
        !texture_replacement::pending_frame_active)
    {
        render_tile_layer(buf, page_index, priority_draw);
        return;
    }

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
