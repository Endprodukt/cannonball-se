#pragma once

#include "stdint.hpp"

class RomLoader;

class hwtiles
{
public:
    enum
    {
        LEFT,
        RIGHT,
        CENTRE,
    };

    alignas(64) uint8_t text_ram[0x1000+4];  // Text RAM; +4 to eliminate need to check addresses on 16/32 bit writes
    alignas(64) uint8_t tile_ram[0x10000+4]; // Tile RAM; +4 to eliminate need to check addresses on 16/32 bit writes
    alignas(16) uint32_t PL0[256], PL1[256], PL2[256];

    hwtiles(void);
    ~hwtiles(void);

    void init(uint8_t* src_tiles, const bool hires);
    void patch_tiles(RomLoader* patch);
    void restore_tiles();
    void set_x_clamp(const uint16_t);
    void update_tile_values();
    void render_tile_layer(uint16_t*, uint8_t, uint8_t);
    void render_text_layer(uint16_t*, uint8_t);
    void render_all_tiles(uint16_t*);

    // Development exporter: writes assembled 1024x512 tilemap layers rather
    // than individual 8x8 tiles.
    void export_composite_layers();

    // Development replacement wrappers. video.cpp calls these through macros
    // defined only after the original class declarations are complete.
    void render_tile_layer_with_replacements(uint16_t*, uint8_t, uint8_t);
    void render_text_layer_with_replacements(uint16_t*, uint8_t);

private:
    int16_t x_clamp;

    // S16 Width, ignoring widescreen related scaling.
    uint16_t s16_width_noscale;

    static const int TILES_LENGTH = 0x10000;
    alignas(64) uint32_t tiles[TILES_LENGTH];        // Converted tiles
    alignas(64) uint32_t tiles_backup[TILES_LENGTH]; // Converted tiles (backup without patch)

    uint16_t page[4];
    uint16_t scroll_x[4];
    uint16_t scroll_y[4];

    uint8_t tile_banks[2];

    static const uint16_t NUM_TILES = 0x2000; // Length of graphic rom / 24
    static const uint16_t TILEMAP_COLOUR_OFFSET = 0x1c00;
    
    void (hwtiles::*render8x8_tile_mask)(
        uint16_t *buf,
        uint16_t nTileNumber, 
        uint16_t StartX, 
        uint16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 
        
    void (hwtiles::*render8x8_tile_mask_clip)(
        uint16_t *buf,
        uint16_t nTileNumber, 
        int16_t StartX, 
        int16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 
        
    void render8x8_tile_mask_lores(
        uint16_t *buf,
        uint16_t nTileNumber, 
        uint16_t StartX, 
        uint16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 

    void render8x8_tile_mask_clip_lores(
        uint16_t *buf,
        uint16_t nTileNumber, 
        int16_t StartX, 
        int16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset);
        
    void render8x8_tile_mask_hires(
        uint16_t *buf,
        uint16_t nTileNumber, 
        uint16_t StartX, 
        uint16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 
        
    void render8x8_tile_mask_clip_hires(
        uint16_t *buf,
        uint16_t nTileNumber, 
        int16_t StartX, 
        int16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset);
        
};
