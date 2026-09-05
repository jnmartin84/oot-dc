#ifndef MAIN_H
#define MAIN_H

#include "ultra64.h"
#include <stdint.h>
#include <dc/pvr.h>



#define TEX_HASH_SIZE 2048



typedef pvr_ptr_t TexHandle;

typedef struct {
    float u_scale; // original_width / padded_width
    float v_scale; // original_height / padded_height
    int padded_w;
    int padded_h;
} TexPow2Info;

typedef struct __attribute__((aligned(64))) TextureCacheEntry_s {
    // 0
    struct TextureCacheEntry_s* next;
    // 4, 8, 12, 16
    u32 addr, fmt, siz, palette;
    // 20, 24, 28
    u32 width, height, line;
    // 32    
    u32 tlutVersion;
    // 36
    TexHandle id;
    // 40
    uint8_t tlutType;
    // 41
    uint8_t dirty;                   // Set by gfx_texture_cache_invalidate
    // 42, 43
    uint8_t pad[2];
    // 44
    TexPow2Info pow2Info;
    // 44+16 -> 60
    /* Packed lookup key: (phys_addr>>5)<<4 | (palette&0xF). Single-compare cache
       match (SF64-style) — address uniquely identifies fmt/siz/dims, so those are
       NOT in the key. CI textures additionally check tlutType/tlutVersion. */
    u32 key;
} TextureCacheEntry;

/* ---- Shared types ---- */
typedef struct {
    u32 fmt, siz, line, tmem, palette;
    u32 cmt, maskt, shiftt, cms, masks, shifts;
//    TexHandle textureID;
    TextureCacheEntry* texture;
    int width, height;
    u32 uls, ult, texAddr;
    u32 lrs, lrt;
    u32 src_stride; // source image stride for LOADTILE sub-rects
    u32 texCms, texCmt; // effective clamp modes for deferred get_texture
    int texDirty;       // 1 = tile params changed, needs get_texture before use
} TileDescriptor;


extern TileDescriptor sTiles[8];
extern float sPrimColor[4];
extern u16 sTexDataBuf[];
//extern u16 sTexDataBuf16[];
extern int sPvrCurrentList;
extern pvr_dr_state_t sDrState;
extern u32 sPvrFrameBytes;

void flush_triangles(void);
void* pc_resolve_addr(u32 addr);
void gfx_texture_cache_invalidate(void* addr);

static inline uint32_t next_pot(uint32_t v) {
    if (v <= 8) return 8;
    if (v > 256) return 256;
    v--;
    v |= v >> 1; v |= v >> 2;
    v |= v >> 4; v |= v >> 8;
    v++;
    return v;
}

#endif
