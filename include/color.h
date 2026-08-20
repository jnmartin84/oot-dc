#ifndef COLOR_H
#define COLOR_H

#include "ultra64/ultratypes.h"

typedef struct Color_RGB8 {
    u8 r, g, b;
} Color_RGB8;

typedef struct Color_RGBA8 {
    u8 r, g, b, a;
} Color_RGBA8;

typedef union Color_RGBA8_u32 {
    struct {
#ifdef LINUX
        u8 a, b, g, r; // LE memory order of 0xRRGGBBAA
#else
        u8 r, g, b, a; // BE memory order of 0xRRGGBBAA
#endif
    };
    u32 rgba;
} Color_RGBA8_u32;

typedef struct Color_RGBAf {
    f32 r, g, b, a;
} Color_RGBAf;

/*
 * IMPORTANT:
 *  code relies on px.r / px.g / px.b / px.a existing.
 * So we must keep bitfields here.
 *
 */
typedef union Color_RGBA16 {
    struct {
#ifdef LINUX
        u16 a : 1;
        u16 b : 5;
        u16 g : 5;
        u16 r : 5;
#else
        u16 r : 5;
        u16 g : 5;
        u16 b : 5;
        u16 a : 1;
#endif
    };
    u16 rgba;
} Color_RGBA16;

/*
 * This creates a canonical 0xRRGGBBAA integer value.
 * (Endianness only matters if you treat it as raw bytes.)
 */
#define RGBA8(r, g, b, a)                                                                     \
    ((((u32)((r) & 0xFF)) << 24) | (((u32)((g) & 0xFF)) << 16) | (((u32)((b) & 0xFF)) << 8) | \
     (((u32)((a) & 0xFF)) << 0))

/* Compile-time sanity checks (catch padding/packing issues instantly) */
typedef char _color_rgba8_size_check[(sizeof(Color_RGBA8) == 4) ? 1 : -1];
typedef char _color_rgba8u32_size_check[(sizeof(Color_RGBA8_u32) == 4) ? 1 : -1];
typedef char _color_rgba16_size_check[(sizeof(Color_RGBA16) == 2) ? 1 : -1];

#endif
