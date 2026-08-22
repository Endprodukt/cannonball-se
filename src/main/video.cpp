/***************************************************************************
    Video Rendering.

    - Renders the System 16 Video Layers
    - Handles Reads and Writes to these layers from the main game code
    - Interfaces with platform specific rendering code

    Copyright Chris White.
    See license.txt for more details.

    Revisions for CannonBall-SE Copyright (c) 2025 James Pearce:
    - Removed Boost alignment helpers.
    - Use C++23 for:
      - aligned new/delete for pixel buffers (portable on x86/ARM)
      - byteswap (single instruction big-to-little-endian conversion
***************************************************************************/

// Aligned Memory Allocation (std, not Boost)
#include <new>          // std::align_val_t, ::operator new/delete
#include <cstddef>      // std::size_t
#include <cstdint>
#include <cstring>      // std::memset
#include <iostream>
#include <bit>          // std::byteswap (C++20/23)
#include <cstring>      // std::memcpy

#include "video.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"
#include "engine/oroad.hpp"
#include "texture_export.hpp"
#include "road_background_export.hpp"

#include "sdl2/pixelscaler_renderer.hpp"

Video video;

Video::Video(void)
{
    renderer     = new PixelScalerRenderer();
    pixels       = NULL;
    sprite_layer = new hwsprites();
    tile_layer   = new hwtiles();

    set_shadow_intensity(shadow::ORIGINAL);

    enabled      = false;
}

Video::~Video(void)
{
    video.disable(); // JJP
    delete sprite_layer;
    delete tile_layer;
    // JJP - moved to disable - if (pixels) delete[] pixels;
    //renderer->disable();
    delete renderer;
}

int Video::init(Roms* roms, video_settings_t* settings)
{
    if (!set_video_mode(settings))
        return false;

    // Internal pixel arrays.
    // JJP - add 128 bytes to each video buffer so that we can then avoid testing for x>0 in the sprite rendering loop
//    std::size_t size = ((config.s16_width * config.s16_height) + alignment) * sizeof(uint16_t);
    std::size_t size = ((config.s16_width * (config.s16_height+2)) + alignment) * sizeof(uint16_t);
    // Initialise two buffers. This is used to allow the renderer to read from one buffer while the main thread writes to the other.
    pixel_buffers[0] = static_cast<uint16_t*>(::operator new(size, std::align_val_t(alignment)));
    pixel_buffers[1] = static_cast<uint16_t*>(::operator new(size, std::align_val_t(alignment)));
    current_pixel_buffer = 0;
    pixels = pixel_buffers[current_pixel_buffer] + alignment;
    // Initialize both buffers to all zeros using std::memset
    std::memset(pixel_buffers[0], 0, size);
    std::memset(pixel_buffers[1], 0, size);

    // Convert S16 tiles to a more useable format
    if (!roms->tiles.rom || !roms->sprites.rom || !roms->road.rom) {
        std::cerr << "ROM buffers missing at Video::init() — cannot build graphics subsystem.\n";
        return false;
    }
    tile_layer->init(roms->tiles.rom, config.video.hires != 0);
    sprite_layer->init(roms->sprites.rom);
    hwroad.init(roms->road.rom, config.video.hires != 0);

    clear_tile_ram();
    clear_text_ram();

//    renderer->init(config.s16_width, config.s16_height, settings->scale, settings->mode, settings->scanlines);

    enabled = true;
    return true;
}

void Video::swap_buffers()
{
//std::cout << std::hex << "Video::swap_buffers: pixel_buffers[0/1]: " << pixel_buffers[0] << "/" << pixel_buffers[1] << std::dec << "\n";
    current_pixel_buffer ^= 1;
    pixels = pixel_buffers[current_pixel_buffer] + alignment;
    renderer->swap_buffers();
}

void Video::disable()
{
    renderer->disable();
    if (pixels)
    {
        if (pixel_buffers[0]) { ::operator delete(pixel_buffers[0], std::align_val_t(alignment)); pixel_buffers[0] = nullptr; }
        if (pixel_buffers[1]) { ::operator delete(pixel_buffers[1], std::align_val_t(alignment)); pixel_buffers[1] = nullptr; }
        pixels = nullptr;
    }
    enabled = false;
}

// ------------------------------------------------------------------------------------------------
// Configure video settings from config file
// ------------------------------------------------------------------------------------------------

int Video::set_video_mode(video_settings_t* settings)
{
    switch (settings->widescreen)
    {
    case 2: // 21:9
        config.s16_width = S16_WIDTH_ULTRAWIDE;
        config.s16_x_off = (S16_WIDTH_ULTRAWIDE - S16_WIDTH) / 2;
        break;

    case 1: // 16:9
        config.s16_width = S16_WIDTH_WIDE;
        config.s16_x_off = (S16_WIDTH_WIDE - S16_WIDTH) / 2;
        break;

    default: // 4:3
        config.s16_width = S16_WIDTH;
        config.s16_x_off = 0;
        break;
    }

    config.s16_height = S16_HEIGHT;

    // Internal video buffer is doubled in hi-res mode.
    if (settings->hires)
    {
        config.s16_width  <<= 1;
        config.s16_height <<= 1;
    }

    if (settings->scanlines < 0) settings->scanlines = 0;
    else if (settings->scanlines > 100) settings->scanlines = 100;

    if (settings->scale < 1)
        settings->scale = 1;

    set_shadow_intensity(settings->shadow == 0 ? shadow::ORIGINAL : shadow::MAME);
    //renderer->init_palette(config.video.red_curve, config.video.green_curve, config.video.blue_curve);
    renderer->init_palette(100, 100, 100);

    return renderer->init(config.s16_width, config.s16_height, settings->scale, settings->mode, settings->scanlines);
//    return true;
}

// --------------------------------------------------------------------------------------------
// Shadow Colours
// 63% Intensity is the correct value derived from hardware as follows:
//
// 1/ Shadows are just an extra 220 ohm resistor that goes to ground when enabled.
// 2/ This is in parallel with the resistor-"DAC" (3.9k, 2k, 1k, 0.5k, 0.25k),
//    and otherwise left floating.
//
// Static calculation example:
//
// const float rDAC   = 1.f / (1.f/3900.f + 1.f/2000.f + 1.f/1000.f + 1.f/500.f + 1.f/250.f);
// const float rShade = 220.f;
// const float shadeAttenuation = rShade / (rShade + rDAC); // 0.63f
//
// (MAME uses an incorrect value which is closer to 78% Intensity)
// --------------------------------------------------------------------------------------------

void Video::set_shadow_intensity(float f)
{
    renderer->set_shadow_intensity(f);
}

void Video::prepare_frame()
{
    // Renderer Specific Frame Setup
    if (!renderer->start_frame())
        return;

    if (!enabled)
    {
        // Fill with black pixels
        int i = config.s16_width * config.s16_height; // JJP optimisation
        while (i--)
            pixels[i] = 0;
    }
    else
    {
        // OutRun Hardware Video Emulation
        tile_layer->update_tile_values();
        tile_layer->export_composite_layers();
        hwroad.export_background_layer();

        (hwroad.*hwroad.render_background)(pixels);
        tile_layer->render_tile_layer(pixels, 1, 0);      // background layer
        tile_layer->render_tile_layer(pixels, 0, 0);      // foreground layer

        if (!config.engine.fix_bugs || oroad.horizon_base != ORoad::HORIZON_OFF)
            (hwroad.*hwroad.render_foreground)(pixels);

        sprite_layer->export_visible_sprites();
        sprite_layer->render(pixels, 8);
        tile_layer->render_text_layer(pixels, 1);
    }
}

void Video::render_frame(int fastpass)
{
    // draw the frame from the pixel buffer not in use for writing
    uint16_t* renderer_pixels = pixel_buffers[current_pixel_buffer ^ 1] + alignment;
    renderer->draw_frame(renderer_pixels, fastpass);
}

void Video::present_frame()
{
	renderer->finalize_frame();
}

bool Video::supports_window()
{
    return renderer->supports_window();
}

bool Video::supports_vsync()
{
    return renderer->supports_vsync();
}

// ---------------------------------------------------------------------------
// Text Handling Code
// ---------------------------------------------------------------------------

void Video::clear_text_ram()
{
    for (uint32_t i = 0; i <= 0xFFF; i++)
        tile_layer->text_ram[i] = 0;
}

void Video::write_text8(uint32_t addr, const uint8_t data)
{
    tile_layer->text_ram[addr & 0xFFF] = data;
}

void Video::write_text16(uint32_t* addr, const uint16_t data)
{
    const uint32_t base = (*addr) & 0x0FFFu;      // 4 KiB text RAM
    const uint16_t le = std::byteswap(data);
    std::memcpy(&tile_layer->text_ram[base], &le, sizeof(le));
    *addr += 2;
}

/*
void Video::write_text16(uint32_t* addr, const uint16_t data)
{
    tile_layer->text_ram[*addr & 0xFFF] = (data >> 8) & 0xFF;
    tile_layer->text_ram[(*addr+1) & 0xFFF] = data & 0xFF;

    *addr += 2;
}
*/

void Video::write_text16(uint32_t addr, const uint16_t data)
{
    const uint32_t base = addr & 0x0FFFu;      // 4 KiB text RAM
    const uint16_t le = std::byteswap(data);
    std::memcpy(&tile_layer->text_ram[base], &le, sizeof(le));
}

/*
void Video::write_text16(uint32_t addr, const uint16_t data)
{
    tile_layer->text_ram[addr & 0xFFF] = (data >> 8) & 0xFF;
    tile_layer->text_ram[(addr+1) & 0xFFF] = data & 0xFF;
}
*/

void Video::write_text32(uint32_t* addr, const uint32_t data)
{
    const uint32_t base = (*addr) & 0x0FFFu;      // 4 KiB text RAM
    const uint32_t le = std::byteswap(data);
    std::memcpy(&tile_layer->text_ram[base], &le, sizeof(le));
    *addr += 4;
}

/*
void Video::write_text32(uint32_t* addr, const uint32_t data)
{
    tile_layer->text_ram[*addr & 0xFFF] = (data >> 24) & 0xFF;
    tile_layer->text_ram[(*addr+1) & 0xFFF] = (data >> 16) & 0xFF;
    tile_layer->text_ram[(*addr+2) & 0xFFF] = (data >> 8) & 0xFF;
    tile_layer->text_ram[(*addr+3) & 0xFFF] = data & 0xFF;

    *addr += 4;
}
*/

void Video::write_text32(uint32_t addr, const uint32_t data)
{
    const uint32_t base = addr & 0x0FFFu;      // 4 KiB text RAM
    const uint32_t le = std::byteswap(data);
    std::memcpy(&tile_layer->text_ram[base], &le, sizeof(le));
}

/*
void Video::write_text32(uint32_t addr, const uint32_t data)
{
    tile_layer->text_ram[addr & 0xFFF] = (data >> 24) & 0xFF;
    tile_layer->text_ram[(addr+1) & 0xFFF] = (data >> 16) & 0xFF;
    tile_layer->text_ram[(addr+2) & 0xFFF] = (data >> 8) & 0xFF;
    tile_layer->text_ram[(addr+3) & 0xFFF] = data & 0xFF;
}
*/

uint8_t Video::read_text8(uint32_t addr)
{
    return tile_layer->text_ram[addr & 0xFFF];
}

// ---------------------------------------------------------------------------
// Tile Handling Code
// ---------------------------------------------------------------------------

void Video::clear_tile_ram()
{
    for (uint32_t i = 0; i <= 0xFFFF; i++)
        tile_layer->tile_ram[i] = 0;
}

void Video::write_tile8(uint32_t addr, const uint8_t data)
{
    tile_layer->tile_ram[addr & 0xFFFF] = data;
}

void Video::write_tile16(uint32_t* addr, const uint16_t data)
{
    // The tile RAM is 64 kB; wrap the address into that range
    const uint32_t index = (*addr) & 0xFFFFU;
    const uint16_t le = std::byteswap(data);   // big-endian -> little-endian
    std::memcpy(&tile_layer->tile_ram[index], &le, sizeof(le));
    *addr += 2;
}

/*
void Video::write_tile16(uint32_t* addr, const uint16_t data)
{
    tile_layer->tile_ram[*addr & 0xFFFF] = (data >> 8) & 0xFF;
    tile_layer->tile_ram[(*addr+1) & 0xFFFF] = data & 0xFF;

    *addr += 2;
}
*/

void Video::write_tile16(uint32_t addr, const uint16_t data)
{
    // The tile RAM is 64 kB; wrap the address into that range
    const uint32_t index = addr & 0xFFFFU;
    const uint16_t le = std::byteswap(data);   // big-endian -> little-endian
    std::memcpy(&tile_layer->tile_ram[index], &le, sizeof(le));
}

/*
void Video::write_tile16(uint32_t addr, const uint16_t data)
{
    tile_layer->tile_ram[addr & 0xFFFF] = (data >> 8) & 0xFF;
    tile_layer->tile_ram[(addr+1) & 0xFFFF] = data & 0xFF;
}
*/

void Video::write_tile32(uint32_t* addr, const uint32_t data)
{
    // The tile RAM is 64 kB - wrap the supplied address.
    const uint32_t index = (*addr) & 0xFFFFU;
    const uint32_t le = std::byteswap(data);  // big-endian -> little-endian
    std::memcpy(&tile_layer->tile_ram[index], &le, sizeof(le));
    *addr += 4;
}

/*
void Video::write_tile32(uint32_t* addr, const uint32_t data)
{
    tile_layer->tile_ram[*addr & 0xFFFF] = (data >> 24) & 0xFF;
    tile_layer->tile_ram[(*addr+1) & 0xFFFF] = (data >> 16) & 0xFF;
    tile_layer->tile_ram[(*addr+2) & 0xFFFF] = (data >> 8) & 0xFF;
    tile_layer->tile_ram[(*addr+3) & 0xFFFF] = data & 0xFF;

    *addr += 4;
}
*/

void Video::write_tile32(uint32_t addr, const uint32_t data)
{
    // The tile RAM is 64 kB; wrap the address into that range
    const uint32_t index = addr & 0xFFFFU;
    const uint32_t le = std::byteswap(data);   // big-endian -> little-endian
    std::memcpy(&tile_layer->tile_ram[index], &le, sizeof(le));
}

/*
void Video::write_tile32(uint32_t addr, const uint32_t data)
{
    tile_layer->tile_ram[addr & 0xFFFF] = (data >> 24) & 0xFF;
    tile_layer->tile_ram[(addr+1) & 0xFFFF] = (data >> 16) & 0xFF;
    tile_layer->tile_ram[(addr+2) & 0xFFFF] = (data >> 8) & 0xFF;
    tile_layer->tile_ram[(addr+3) & 0xFFFF] = data & 0xFF;
}
*/

uint8_t Video::read_tile8(uint32_t addr)
{
    return tile_layer->tile_ram[addr & 0xFFFF];
}

// ---------------------------------------------------------------------------
// Sprite Handling Code
// ---------------------------------------------------------------------------

void Video::write_sprite16(uint32_t* addr, const uint16_t data)
{
    sprite_layer->write(*addr & 0xfff, data);
    *addr += 2;
}

// ---------------------------------------------------------------------------
// Palette Handling Code
// ---------------------------------------------------------------------------

void Video::write_pal8(uint32_t* palAddr, const uint8_t data)
{
    palette[*palAddr & 0x1fff] = data;
    refresh_palette(*palAddr & 0x1fff);
    *palAddr += 1;
}

void Video::write_pal16(uint32_t* palAddr, const uint16_t data)
{
    // Keep the index inside the 8 KB palette and aligned to a half-word
    uint32_t adr = (*palAddr) & (0x1fffu - 1u);   // 0x1fff - 1 = 8190

    // Reverse the byte order and write the whole (16-bit) word at once
    uint16_t word = std::byteswap(data);          // MSB->LSB
    std::memcpy(&palette[adr], &word, sizeof(word));

    refresh_palette(adr);
    *palAddr += 2;
}

/*
void Video::write_pal16(uint32_t* palAddr, const uint16_t data)
{
    uint32_t adr = *palAddr & (0x1fff - 1); // 0x1fff - 1 = 8190;
    palette[adr]   = (data >> 8) & 0xFF;
    palette[adr+1] = data & 0xFF;
    refresh_palette(adr);
    *palAddr += 2;
}
*/

void Video::write_pal32(uint32_t* palAddr, const uint32_t data)
{
    uint32_t adr = *palAddr & (0x1fff - 3); // 0x1fff - 3 = 8188;

    // Reverse the byte order and write the whole word at once
    uint32_t word = std::byteswap(data);   // big-endian -> little-endian
    std::memcpy(&palette[adr], &word, sizeof(word));

    refresh_palette(adr);
    refresh_palette(adr + 2);
    *palAddr += 4;
}

/*
void Video::write_pal32(uint32_t* palAddr, const uint32_t data)
{
    uint32_t adr = *palAddr & (0x1fff - 3); // 0x1fff - 3 = 8188;

    palette[adr]   = (data >> 24) & 0xFF;
    palette[adr+1] = (data >> 16) & 0xFF;
    palette[adr+2] = (data >> 8) & 0xFF;
    palette[adr+3] = data & 0xFF;

    refresh_palette(adr);
    refresh_palette(adr+2);

    *palAddr += 4;
}
*/

void Video::write_pal32(uint32_t adr, uint32_t data)
{
    // keep adr within the 8-KB palette, aligned to a 4-byte word
    adr &= (0x1fffu - 3u);          // 0x1fff - 3 = 8188

    // Reverse the byte order and write the whole word at once
    uint32_t word = std::byteswap(data);   // big-endian -> little-endian
    std::memcpy(&palette[adr], &word, sizeof(word));

    refresh_palette(adr);
    refresh_palette(adr + 2);
}

/*
void Video::write_pal32(uint32_t adr, const uint32_t data)
{
    adr &= (0x1fff - 3); // 0x1fff - 3 = 8188;

    palette[adr]   = (data >> 24) & 0xFF;
    palette[adr+1] = (data >> 16) & 0xFF;
    palette[adr+2] = (data >> 8) & 0xFF;
    palette[adr+3] = data & 0xFF;
    refresh_palette(adr);
    refresh_palette(adr+2);
}
*/

uint8_t Video::read_pal8(uint32_t palAddr)
{
    return palette[palAddr & 0x1fff];
}

uint16_t Video::read_pal16(uint32_t palAddr)
{
    uint32_t adr = palAddr & (0x1fffu - 1u);    // keep inside 8 KB, 16-bit aligned
    uint16_t w = 0;
    std::memcpy(&w, &palette[adr], sizeof(w));  // single 16-bit load

    return std::byteswap(w);                    // palette is big-endian
}

/*
uint16_t Video::read_pal16(uint32_t palAddr)
{
    uint32_t adr = palAddr & (0x1fff - 1); // 0x1fff - 1 = 8190;;
    return (palette[adr] << 8) | palette[adr+1];
}
*/

uint16_t Video::read_pal16(uint32_t* palAddr)
{
    uint32_t adr = (*palAddr) & (0x1fffu - 1u);

    *palAddr += 2;                      // advance the caller's address

    uint16_t w = 0;
    std::memcpy(&w, &palette[adr], sizeof(w));

    return std::byteswap(w);
}

/*
uint16_t Video::read_pal16(uint32_t* palAddr)
{
    uint32_t adr = *palAddr & (0x1fff - 1); // 0x1fff - 1 = 8190;;
    *palAddr += 2;
    return (palette[adr] << 8)| palette[adr+1];
}
*/

uint32_t Video::read_pal32(uint32_t* palAddr)
{
    // Keep the index inside the 8 KB palette and aligned to a 4-byte word
    uint32_t adr = (*palAddr) & (0x1fffu - 3u);   // 0x1fff - 3 = 8188

    // Advance the caller's address before we read
    *palAddr += 4;

    // Load the whole word at once
    uint32_t word = 0;
    std::memcpy(&word, &palette[adr], sizeof(word));

    // The palette is stored big-endian; convert to the host format
    return std::byteswap(word);
}

/*
uint32_t Video::read_pal32(uint32_t* palAddr)
{
    uint32_t adr = (*palAddr) & (0x1fff - 3); // 0x1fff - 3 = 8188;
    *palAddr += 4;
    return (palette[adr] << 24) | (palette[adr+1] << 16) | (palette[adr+2] << 8) | palette[adr+3];
}
*/

// Convert internal System 16 RRRR GGGG BBBB format palette to renderer output format
void Video::refresh_palette(uint32_t palAddr)
{
    // Ensure we address an even index - the palette is 16-bit entries.
    palAddr &= ~1u;

    /*  Read the 16-bit value once.
        The palette stores a big-endian word:  high byte first.  */
    uint16_t a;
    std::memcpy(&a, &palette[palAddr], sizeof a);   // one 16-bit copy
    a = std::byteswap(a);

    /*  Extract the 5-bit RGB components in a single operation each.
        The logic is equivalent to the original code but needs only
        one shift, one mask and one OR per component.  */
    uint8_t r = (((a >> 0) & 0x000Fu) << 1) | ((a >> 12) & 1u);   // bits 0-3, flag bit 12
    uint8_t g = (((a >> 4) & 0x000Fu) << 1) | ((a >> 13) & 1u); // bits 4-7, flag bit 13
    uint8_t b = (((a >> 8) & 0x000Fu) << 1) | ((a >> 14) & 1u); // bits 8-11, flag bit 14

    renderer->convert_palette(palAddr, r, g, b);
}
/*
void Video::refresh_palette(uint32_t palAddr)
{
    palAddr &= ~1;
    uint32_t a = (palette[palAddr] << 8) | palette[palAddr + 1];
    uint32_t r = (a & 0x000f) << 1; // r rrr0
    uint32_t g = (a & 0x00f0) >> 3; // g ggg0
    uint32_t b = (a & 0x0f00) >> 7; // b bbb0
    if ((a & 0x1000) != 0)
        r |= 1; // r rrrr
    if ((a & 0x2000) != 0)
        g |= 1; // g gggg
    if ((a & 0x4000) != 0)
        b |= 1; // b bbbb

    renderer->convert_palette(palAddr, r, g, b);
}
*/
