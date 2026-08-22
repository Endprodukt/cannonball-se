#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
    #include <windows.h>
    #include <objbase.h>
    #include <wincodec.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ole32.lib")
        #pragma comment(lib, "windowscodecs.lib")
    #endif
#endif

#include "video.hpp"

// Development-only texture export helpers. These deliberately do not take part
// in rendering or replacement. They only observe the same source data that the
// native renderer already uses and write PNG assets for inspection/reworking.
namespace texture_export
{
    struct rgba_t
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    constexpr uint8_t S16_RGB[32] = {
        0, 8, 16, 24, 31, 39, 47, 55, 62, 70, 78, 86, 94, 102, 109, 117,
        125, 133, 140, 148, 156, 164, 171, 179, 187, 195, 203, 211, 218, 226, 234, 242
    };

    inline rgba_t palette_word_to_rgba(uint16_t raw)
    {
        const uint8_t r = static_cast<uint8_t>((((raw >> 0) & 0x0F) << 1) | ((raw >> 12) & 1));
        const uint8_t g = static_cast<uint8_t>((((raw >> 4) & 0x0F) << 1) | ((raw >> 13) & 1));
        const uint8_t b = static_cast<uint8_t>((((raw >> 8) & 0x0F) << 1) | ((raw >> 14) & 1));
        return { S16_RGB[r], S16_RGB[g], S16_RGB[b], 255 };
    }

    inline uint32_t fnv1a_bytes(const uint8_t* bytes, size_t count, uint32_t hash = 2166136261u)
    {
        for (size_t i = 0; i < count; ++i)
        {
            hash ^= bytes[i];
            hash *= 16777619u;
        }
        return hash;
    }

    inline uint32_t fnv1a_word(uint32_t hash, uint16_t word)
    {
        const uint8_t bytes[2] = {
            static_cast<uint8_t>(word >> 8),
            static_cast<uint8_t>(word & 0xFF)
        };
        return fnv1a_bytes(bytes, 2, hash);
    }

    inline uint32_t png_crc32(const uint8_t* data, size_t length)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < length; ++i)
        {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return crc ^ 0xFFFFFFFFu;
    }

    inline uint32_t raw_adler32(const uint8_t* data, size_t length)
    {
        constexpr uint32_t MOD_ADLER = 65521u;
        uint32_t a = 1;
        uint32_t b = 0;
        for (size_t i = 0; i < length; ++i)
        {
            a = (a + data[i]) % MOD_ADLER;
            b = (b + a) % MOD_ADLER;
        }
        return (b << 16) | a;
    }

    inline void append_be32(std::vector<uint8_t>& out, uint32_t value)
    {
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    inline void append_chunk(std::vector<uint8_t>& png,
                             const char type[4],
                             const std::vector<uint8_t>& data)
    {
        append_be32(png, static_cast<uint32_t>(data.size()));
        const size_t crc_start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        append_be32(png, png_crc32(png.data() + crc_start, 4 + data.size()));
    }

#ifdef _WIN32
    inline bool write_rgba_png_wic(const std::filesystem::path& filename,
                                   uint32_t width,
                                   uint32_t height,
                                   const std::vector<rgba_t>& pixels)
    {
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(init_hr);
        if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE)
            return false;

        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* properties = nullptr;

        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));

        if (SUCCEEDED(hr))
            hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr))
            hr = stream->InitializeFromFilename(filename.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr))
            hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr))
            hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr))
            hr = encoder->CreateNewFrame(&frame, &properties);
        if (SUCCEEDED(hr))
            hr = frame->Initialize(properties);
        if (SUCCEEDED(hr))
            hr = frame->SetSize(width, height);

        WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppRGBA;
        if (SUCCEEDED(hr))
            hr = frame->SetPixelFormat(&pixel_format);
        if (SUCCEEDED(hr) && !IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppRGBA))
            hr = E_FAIL;

        if (SUCCEEDED(hr))
        {
            const UINT stride = width * 4;
            const size_t byte_count = pixels.size() * sizeof(rgba_t);
            if (byte_count > static_cast<size_t>(UINT_MAX))
            {
                hr = E_FAIL;
            }
            else
            {
                hr = frame->WritePixels(
                    height,
                    stride,
                    static_cast<UINT>(byte_count),
                    reinterpret_cast<BYTE*>(const_cast<rgba_t*>(pixels.data())));
            }
        }

        if (SUCCEEDED(hr))
            hr = frame->Commit();
        if (SUCCEEDED(hr))
            hr = encoder->Commit();

        if (properties) properties->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        if (uninitialize) CoUninitialize();

        return SUCCEEDED(hr);
    }
#endif

    inline bool write_rgba_png_fallback(const std::filesystem::path& filename,
                                        uint32_t width,
                                        uint32_t height,
                                        const std::vector<rgba_t>& pixels)
    {
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(height) *
                    (1 + static_cast<size_t>(width) * 4));

        for (uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0);
            for (uint32_t x = 0; x < width; ++x)
            {
                const rgba_t& p = pixels[static_cast<size_t>(y) * width + x];
                raw.push_back(p.r);
                raw.push_back(p.g);
                raw.push_back(p.b);
                raw.push_back(p.a);
            }
        }

        std::vector<uint8_t> zlib;
        zlib.reserve(raw.size() + (raw.size() / 65535 + 1) * 5 + 6);
        zlib.push_back(0x78);
        zlib.push_back(0x01);

        size_t offset = 0;
        while (offset < raw.size())
        {
            const size_t remaining = raw.size() - offset;
            const uint16_t block_len = static_cast<uint16_t>(
                remaining > 65535 ? 65535 : remaining);
            const bool final_block = offset + block_len == raw.size();

            zlib.push_back(final_block ? 0x01 : 0x00);
            zlib.push_back(static_cast<uint8_t>(block_len & 0xFF));
            zlib.push_back(static_cast<uint8_t>((block_len >> 8) & 0xFF));

            const uint16_t nlen = static_cast<uint16_t>(~block_len);
            zlib.push_back(static_cast<uint8_t>(nlen & 0xFF));
            zlib.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
            zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + block_len);
            offset += block_len;
        }

        append_be32(zlib, raw_adler32(raw.data(), raw.size()));

        std::vector<uint8_t> png = { 137, 80, 78, 71, 13, 10, 26, 10 };
        std::vector<uint8_t> ihdr;
        ihdr.reserve(13);
        append_be32(ihdr, width);
        append_be32(ihdr, height);
        ihdr.push_back(8);
        ihdr.push_back(6);
        ihdr.push_back(0);
        ihdr.push_back(0);
        ihdr.push_back(0);
        append_chunk(png, "IHDR", ihdr);
        append_chunk(png, "IDAT", zlib);
        append_chunk(png, "IEND", {});

        std::ofstream file(filename, std::ios::binary);
        if (!file)
            return false;
        file.write(reinterpret_cast<const char*>(png.data()),
                   static_cast<std::streamsize>(png.size()));
        return file.good();
    }

    inline bool write_rgba_png(const std::filesystem::path& filename,
                               uint32_t width,
                               uint32_t height,
                               const std::vector<rgba_t>& pixels)
    {
        if (width == 0 || height == 0 ||
            pixels.size() != static_cast<size_t>(width) * height)
        {
            return false;
        }

#ifdef _WIN32
        if (write_rgba_png_wic(filename, width, height, pixels))
            return true;
#endif
        return write_rgba_png_fallback(filename, width, height, pixels);
    }

    struct decoded_sprite_t
    {
        bool valid = false;
        uint32_t start_offset = 0;
        size_t max_width = 0;
        size_t visible_pixels = 0;
        std::vector<std::vector<uint8_t>> rows;
    };

    inline decoded_sprite_t decode_outrun_sprite_candidate(const hwsprites& layer,
                                                            uint32_t bank,
                                                            uint32_t start_offset,
                                                            uint32_t pitch,
                                                            uint32_t sprite_height)
    {
        decoded_sprite_t result;
        result.start_offset = start_offset;

        if (pitch == 0 || sprite_height == 0 || start_offset >= 0x10000)
            return result;

        result.rows.reserve(sprite_height);

        for (uint32_t y = 0; y < sprite_height; ++y)
        {
            const uint64_t row_addr64 = static_cast<uint64_t>(start_offset) +
                                        static_cast<uint64_t>(y) * pitch;
            if (row_addr64 >= 0x10000)
                return {};

            std::vector<uint8_t> row;
            row.reserve(static_cast<size_t>(pitch) * 8);

            for (uint32_t word_index = 0; word_index < pitch; ++word_index)
            {
                const uint64_t rom_addr64 = row_addr64 + word_index;
                if (rom_addr64 >= 0x10000)
                    return {};

                const uint32_t word = layer.export_read_sprite_word(
                    bank, static_cast<uint32_t>(rom_addr64));

                for (int shift = 28; shift >= 0; shift -= 4)
                {
                    const uint8_t pix = static_cast<uint8_t>((word >> shift) & 0x0F);
                    row.push_back(pix);
                    if (pix != 0 && pix != 0x0F)
                        ++result.visible_pixels;
                }

                if ((word & 0x000000F0u) == 0x000000F0u)
                    break;
            }

            result.max_width = std::max(result.max_width, row.size());
            result.rows.push_back(std::move(row));
        }

        result.valid = !result.rows.empty() && result.max_width != 0;
        return result;
    }

    inline decoded_sprite_t decode_outrun_sprite(const hwsprites& layer,
                                                  uint32_t bank,
                                                  uint32_t runtime_offset,
                                                  uint32_t pitch,
                                                  uint32_t sprite_height)
    {
        decoded_sprite_t best = decode_outrun_sprite_candidate(
            layer, bank, runtime_offset, pitch, sprite_height);

        const size_t full_row_capacity = static_cast<size_t>(pitch) * 8;
        const bool direct_looks_truncated = !best.valid ||
            (pitch > 1 && best.max_width * 2 < full_row_capacity);

        if (direct_looks_truncated && pitch > 1 && runtime_offset >= (pitch - 1))
        {
            decoded_sprite_t alternate = decode_outrun_sprite_candidate(
                layer, bank, runtime_offset - (pitch - 1), pitch, sprite_height);

            if (alternate.valid &&
                (!best.valid ||
                 alternate.max_width > best.max_width ||
                 (alternate.max_width == best.max_width &&
                  alternate.visible_pixels > best.visible_pixels)))
            {
                best = std::move(alternate);
            }
        }

        return best;
    }

    inline std::string make_sprite_key(uint32_t bank,
                                       uint32_t canonical_offset,
                                       uint32_t pitch,
                                       uint32_t sprite_height,
                                       uint32_t palette_hash,
                                       bool shadow)
    {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0')
               << "b" << std::setw(2) << bank
               << "_o" << std::setw(4) << canonical_offset
               << "_p" << std::setw(2) << pitch
               << "_h" << std::setw(2) << sprite_height
               << "_pal" << std::setw(8) << palette_hash
               << (shadow ? "_shadow" : "");
        return stream.str();
    }

    inline std::vector<rgba_t> build_sprite_rgba(const decoded_sprite_t& decoded,
                                                  const rgba_t colours[16],
                                                  bool shadow)
    {
        if (!decoded.valid || decoded.max_width == 0 || decoded.rows.empty())
            return {};

        std::vector<rgba_t> rgba(
            decoded.max_width * decoded.rows.size(),
            { 0, 0, 0, 0 });

        for (size_t y = 0; y < decoded.rows.size(); ++y)
        {
            for (size_t x = 0; x < decoded.rows[y].size(); ++x)
            {
                const uint8_t pix = decoded.rows[y][x];
                if (pix == 0 || pix == 0x0F)
                    continue;

                if (shadow && pix == 0x0A)
                    rgba[y * decoded.max_width + x] = { 0, 0, 0, 94 };
                else
                    rgba[y * decoded.max_width + x] = colours[pix];
            }
        }

        return rgba;
    }
}

// Restore the proven sprite exporter from the previous experiment, but without
// any replacement/render hooks. It only writes each unique source sprite once.
inline void hwsprites::export_visible_sprites()
{
    static std::unordered_set<std::string> exported;
    static bool export_dir_ready = false;

    const uint32_t numbanks = SPRITES_LENGTH / 0x10000;
    if (numbanks == 0)
        return;

    if (!export_dir_ready)
    {
        std::error_code ec;
        std::filesystem::create_directories("sprite_export", ec);
        if (ec)
        {
            std::cerr << "Unable to create sprite_export directory: " << ec.message() << "\n";
            return;
        }
        export_dir_ready = true;
    }

    for (uint16_t data = 0; data < SPRITE_RAM_SIZE; data += 16)
    {
        if ((ramBuff[data + 0] & 0x8000) != 0)
            break;

        const int16_t hide = ramBuff[data + 0] & 0x5000;
        const int32_t top = (ramBuff[data + 0] & 0x1FF) - 0x100;
        if (hide != 0 || top <= 0)
            continue;

        uint32_t bank = (ramBuff[data + 0] >> 9) & 7;
        bank %= numbanks;

        const uint32_t addr = ramBuff[data + 1];
        const uint32_t pitch = (((ramBuff[data + 2] >> 1) |
                                ((ramBuff[data + 4] & 0x1000) << 3)) >> 8);
        const uint32_t raw_height = ramBuff[data + 7];
        const uint8_t palette = static_cast<uint8_t>(ramBuff[data + 5] & 0x7F);
        const bool shadow = ((ramBuff[data + 3] >> 14) & 1) != 0;

        if (pitch == 0 || raw_height == 0 || addr >= 0x10000)
            continue;

        const texture_export::decoded_sprite_t decoded =
            texture_export::decode_outrun_sprite(*this, bank, addr, pitch, raw_height);
        if (!decoded.valid)
            continue;

        uint16_t palette_words[16];
        texture_export::rgba_t colours[16];
        uint32_t palette_hash = 2166136261u;
        for (uint32_t i = 0; i < 16; ++i)
        {
            const uint32_t palette_index = COLOR_BASE + static_cast<uint32_t>(palette) * 16 + i;
            palette_words[i] = video.read_pal16(palette_index * 2);
            colours[i] = texture_export::palette_word_to_rgba(palette_words[i]);
            palette_hash = texture_export::fnv1a_word(palette_hash, palette_words[i]);
        }

        const std::string key = texture_export::make_sprite_key(
            bank,
            decoded.start_offset,
            pitch,
            raw_height,
            palette_hash,
            shadow);

        if (!exported.insert(key).second)
            continue;

        const std::filesystem::path filename =
            std::filesystem::path("sprite_export") / ("spr_" + key + ".png");
        if (std::filesystem::exists(filename))
            continue;

        const std::vector<texture_export::rgba_t> rgba =
            texture_export::build_sprite_rgba(decoded, colours, shadow);
        if (rgba.empty())
            continue;

        if (!texture_export::write_rgba_png(
                filename,
                static_cast<uint32_t>(decoded.max_width),
                static_cast<uint32_t>(decoded.rows.size()),
                rgba))
        {
            std::cerr << "Failed to export sprite PNG: " << filename.string() << "\n";
            continue;
        }

        const std::filesystem::path manifest_path =
            std::filesystem::path("sprite_export") / "manifest.csv";
        const bool new_manifest =
            !std::filesystem::exists(manifest_path) ||
            std::filesystem::file_size(manifest_path) == 0;
        std::ofstream manifest(manifest_path, std::ios::app);
        if (manifest)
        {
            if (new_manifest)
                manifest << "file,bank,offset,pitch,height,width,palette_hash,shadow\n";

            manifest << filename.filename().string() << ','
                     << bank << ','
                     << decoded.start_offset << ','
                     << pitch << ','
                     << decoded.rows.size() << ','
                     << decoded.max_width << ','
                     << std::hex << std::setw(8) << std::setfill('0')
                     << palette_hash << std::dec << ','
                     << (shadow ? 1 : 0) << '\n';
        }

        std::cout << "Exported sprite: " << filename.string() << "\n";
    }
}

// Export the two native System-16 scrolling tilemap layers as already assembled
// 2x2 page compositions. Each PNG is 1024x512 at native source resolution:
// four 512x256 pages, with transparent tile colour 0 preserved as alpha.
//
// To avoid the old 8x8 exporter performance problem, this computes a cheap
// content fingerprint every frame and only builds/writes a PNG after the same
// tile/palette state has remained stable for a short time.
inline void hwtiles::export_composite_layers()
{
    constexpr uint32_t LAYER_WIDTH = 1024;
    constexpr uint32_t LAYER_HEIGHT = 512;
    constexpr uint32_t PAGE_WIDTH = 512;
    constexpr uint32_t PAGE_HEIGHT = 256;
    constexpr int STABLE_FRAMES = 15;

    static bool export_dir_ready = false;
    static std::unordered_set<std::string> exported;
    static std::array<uint64_t, 2> pending_fingerprint = { 0, 0 };
    static std::array<int, 2> stable_frames = { 0, 0 };

    if (!export_dir_ready)
    {
        std::error_code ec;
        std::filesystem::create_directories("background_export", ec);
        if (ec)
        {
            std::cerr << "Unable to create background_export directory: " << ec.message() << "\n";
            return;
        }
        export_dir_ready = true;
    }

    // Cache all 128 x 8 tile palette entries. Besides making PNG construction
    // cheap, hashing the palette here means real colour variants stay distinct.
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

        std::array<uint8_t, 4> selected_pages = {
            static_cast<uint8_t>((page_select >> 0) & 0x0F),
            static_cast<uint8_t>((page_select >> 4) & 0x0F),
            static_cast<uint8_t>((page_select >> 8) & 0x0F),
            static_cast<uint8_t>((page_select >> 12) & 0x0F)
        };

        uint32_t graphics_hash = 2166136261u;
        graphics_hash = texture_export::fnv1a_word(graphics_hash, page_select);
        graphics_hash = texture_export::fnv1a_bytes(tile_banks, sizeof(tile_banks), graphics_hash);

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
                   << "_pages" << std::hex << std::setfill('0') << std::setw(4) << page_select
                   << "_g" << std::setw(8) << graphics_hash
                   << "_pal" << std::setw(8) << palette_hash;
        const std::string key = key_stream.str();

        if (!exported.insert(key).second)
            continue;

        const std::filesystem::path filename =
            std::filesystem::path("background_export") / (key + ".png");
        if (std::filesystem::exists(filename))
            continue;

        std::vector<texture_export::rgba_t> rgba(
            static_cast<size_t>(LAYER_WIDTH) * LAYER_HEIGHT,
            { 0, 0, 0, 0 });

        for (uint32_t quad = 0; quad < 4; ++quad)
        {
            const uint8_t page_id = selected_pages[quad];
            const uint32_t dst_base_x = (quad & 1u) ? PAGE_WIDTH : 0;
            const uint32_t dst_base_y = (quad & 2u) ? PAGE_HEIGHT : 0;

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

                    // Match the native frame pipeline. These two scrolling
                    // tilemap passes currently draw priority 0 only; priority 1
                    // entries must not leak into an exported replacement source.
                    if ((data & 0x8000u) != 0)
                        continue;

                    uint32_t code = data & 0x1FFFu;
                    code = (static_cast<uint32_t>(tile_banks[code >> 12]) << 12) |
                           (code & 0x0FFFu);
                    code &= (NUM_TILES - 1);
                    if (code == 0)
                        continue;

                    const uint8_t colour = static_cast<uint8_t>((data >> 6) & 0x7Fu);
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
                                static_cast<size_t>(pixel_base_y + y) * LAYER_WIDTH +
                                (pixel_base_x + x);
                            rgba[out_index] = palette_rgba[colour][pixel & 7u];
                        }
                    }
                }
            }
        }

        if (!texture_export::write_rgba_png(filename, LAYER_WIDTH, LAYER_HEIGHT, rgba))
        {
            std::cerr << "Failed to export composite tilemap PNG: "
                      << filename.string() << "\n";
            continue;
        }

        const std::filesystem::path manifest_path =
            std::filesystem::path("background_export") / "manifest.csv";
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
                     << std::hex << std::setw(4) << std::setfill('0') << page_select << ','
                     << static_cast<unsigned>(selected_pages[0]) << ','
                     << static_cast<unsigned>(selected_pages[1]) << ','
                     << static_cast<unsigned>(selected_pages[2]) << ','
                     << static_cast<unsigned>(selected_pages[3]) << ','
                     << std::setw(8) << graphics_hash << ','
                     << std::setw(8) << palette_hash << std::dec << ','
                     << LAYER_WIDTH << ',' << LAYER_HEIGHT << '\n';
        }

        std::cout << "Exported composite " << layer_name
                  << " tilemap: " << filename.string() << "\n";
    }
}
