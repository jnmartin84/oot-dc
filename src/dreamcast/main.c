#include "ultra64.h"
#include "ultra64/gs2dex.h"
#include "gfx.h"
#include "printf.h"
#include <stdio.h>
#include <stdint.h>
#include <kos.h>
#include "main.h"
//KOS_INIT_FLAGS(INIT_IRQ | INIT_CDROM | INIT_THD_PREEMPT | INIT_CDROM | INIT_CONTROLLER | INIT_VMU | INIT_PURUPURU);
/* Set to 0 to disable audio */
int gDcEnableAudio = 1;

int fog_dirty = 0;
float gl_fog_start;
float gl_fog_end;
float max_z = 0.0f;
void Main(void* arg);
void __osTimerServicesInit(void);
static int sDmgFlash = 0;
extern uintptr_t gSegments[16];
void flush_triangles(void);
void* pc_resolve_addr(u32 addr);

/* The shared decorative flame-mask texture (gameplay_keep). It is the ONLY tile-0
   that should drive the multi-tex stencil path; everything else uses simple lerp. */
extern u64 gDecorativeFlameMaskTex[];

/* Morpha's tentacle/water caustic texture (object_mo). Used as a tight tile-0
   discriminator for the tentacle recolor so it can't collide with other
   sDoAdd==2 blue-env surfaces (e.g. Link's spin-charge effect). */
extern u64 gMorphaWaterTex[];

/* Freezard / ice-chest steam puff (object_fz, single I4 texture). Tile-0
   discriminator to force ADDITIVE blend for the steam — its transparent texels
   are black (intensity 0), so normal alpha-blend + bilinear bleeds a dark fringe. */
extern u64 gFreezardSteamTex[];

//#define RENDER_240P

/* Render resolution (compile-time). Default: 640x480, with N64 320x240 coords
   scaled x2. Build with -DRENDER_240P for native 320x240 (x1 scale, no upscale,
   ~quarter the fill). SCREEN_SCALE is the float coord multiplier; SCREEN_SCALE_I
   the integer (scissor) one; DC_FB_W/H the framebuffer dimensions. */
#ifdef RENDER_240P
#define SCREEN_SCALE   1.0f
#define SCREEN_SCALE_I 1
#define DC_FB_W 320
#define DC_FB_H 240
#else
#define SCREEN_SCALE   2.0f
#define SCREEN_SCALE_I 2
#define DC_FB_W 640
#define DC_FB_H 480
#endif

static u8 pvrOOM = 0;
static u32 sOomLogged; /* PVROOM: diagnostics cap */

static int sFrameCount = 0;
static u32 sRdpHalf1 = 0;
static int sMatrixDirty = 1;
static u64 sCombineMode = 0;
static kthread_t* sMainThread;
/* static */ volatile int sVblCounter;
static volatile int sLastVbl;
static uint32 sFpsTimer = 0;
static uint32 sFpsCounter = 0;
static float sFps = 0.0f;
static int sAlphaTestEnabled = 0;
static u32 sTlutChecksum = 0;
static int sTlutDirty = 1;
static int sDepthTestEnabled = 1;
static int sDepthWriteEnabled = 1;
static float sFillColor[4] = { 0, 0, 0, 1 };
static float sFogColor[4] = { 0, 0, 0, 1 };
static float sFogColor2[4] = { 0, 0, 0, 1 };
static float sBlendColor[4] = { 0, 0, 0, 1 };
static int16 sFogMul;
static int16 sFogOfs;
static u8 sFogChanged;
static u8 sFogColChanged;
/* DC fog-color-redirect (custom DL op "BLND"/"DFLS"): the PVR fog-color register
   is global / once-per-frame, so an object that wants its OWN fog color (e.g.
   Link's i-frame damage flash, Gfx_SetFog2 red) loses to the scene's. While the
   flag is set, the per-vertex fog factor is baked into the vertex color toward
   the captured color instead of routed through the global register. */
static u8 sDamageFlash = 0;
static u8 sDamageCapture = 0;
static u8 sDamageR = 255, sDamageG = 0, sDamageB = 0;
/* DIAG: per-frame min/max fog factor across lit vertices (near vs far) */
static int sFogDbgMin = 999, sFogDbgMax = -1;

static u8 sDrawingSkybox = 0;

#define NEAR_PLANE 0.01f

static float sViewportScaleX = 160.0f;
static float sViewportScaleY = 120.0f;
static float sViewportTransX = 160.0f;
static float sViewportTransY = 120.0f;

static float sPolygonOffsetBias = 0.0f;

/* ============ PROFILING ============ */
/* Per-section CPU timing. File-scope start vars so BEGIN/END need not share a
   scope; safe as long as a section isn't re-entered recursively (none are).
   Note: sPF_Tex includes sPF_TexUpload, and sPF_Combiner overlaps sPF_Tri. */
static uint64 sPF_Tex = 0, sPF_TexUpload = 0, sPF_Vtx = 0, sPF_Tri = 0;
static uint64 sPF_Mtx = 0, sPF_Combiner = 0, sPF_Draw2D = 0;
static uint64 _pb_sPF_Tex, _pb_sPF_TexUpload, _pb_sPF_Vtx, _pb_sPF_Tri;
static uint64 _pb_sPF_Mtx, _pb_sPF_Combiner, _pb_sPF_Draw2D;
/* PROF_ENABLE 0 = no per-section timing (no timer_ns_gettime64 calls) → shows the
   TRUE frame cost a ship build pays. Set to 1 to get the cpu ms/frame breakdown. */
#define PROF_ENABLE 0
#if PROF_ENABLE
#define PROF_BEGIN(acc) (_pb_##acc = timer_ns_gettime64())
#define PROF_END(acc)   (acc += timer_ns_gettime64() - _pb_##acc)
#else
#define PROF_BEGIN(acc) ((void)0)
#define PROF_END(acc)   ((void)0)
#endif
#define NS_TO_MS(ns) ((ns) / 1000000.0)
#define PROF_REPORT_INTERVAL_MS 10000

static uint64 sPA_Total = 0;
/* Coarse per-frame split (once-per-frame timers, negligible overhead, on even
   when PROF_ENABLE=0): CPU display-list walk vs CPU flush vs GPU wait. */
static uint64 sFrameWalkNs = 0, sFrameFlushNs = 0, sFrameFinishNs = 0;
/* Localizing the un-instrumented walk_dl time: per-command state-handler buckets
   (cheap — these fire per state change, not per triangle). */
static uint64 sWalkCombineNs = 0, sWalkOmodeNs = 0, sWalkTexNs = 0;
/* G_LOADTLUT redundancy: how often the palette CONTENT is unchanged (hash-based,
   safe against in-place palette modification — pointer dedup is NOT safe in OoT). */
static u32 sTlutCalls = 0, sTlutRedundant = 0, sLastTlutHash = 0xFFFFFFFFu;
static int sLastTlutOffset = -1, sLastTlutCount = -1, sLastTlutType = -1;
/* WALK_STATS 0 = scaffolding dormant: TIMED runs the call WITHOUT the 2 timer
   reads, and the detailed per-frame perf report is skipped (the FPS line stays).
   Set to 1 to re-enable the full walk-internals/loadtlut/emit/cpu breakdown. */
#define WALK_STATS 0

/* One-shot diagnostic: dump shade/combiner/alpha for the Morpha-class deep-blue
   multitex draws (env ~= 0,100,255). Throttled. Set to 0 to silence. */
#define MORPHA_PROBE 1

/* DIAGNOSTIC: amplify the Morpha tentacle's env-map texgen UV variation to tighten
   its smeared caustic highlights. 1.0 = current (smeared). Tune up = tighter. */
#define MORPHA_TEXGEN_MUL 2.0f

/* DIAGNOSTIC: deepen the Morpha tentacle body via TRANSPARENCY only — lowering the
   surface alpha lets the dark room show through and darken the blue mass, without
   touching the offset (which also feeds the highlights and yellows them if scaled).
   1.0 = opaque/light, lower = more see-through/deeper. */
#define MORPHA_BODY_ALPHA  0.70f

/* Morpha tentacle recolor (the "fake it" path). PVR computes per pixel:
       rgb = TEXEL0(caustic) * packed_rgb + oargb
   Setting oargb = the body color D and packed_rgb = (white - D) makes
   caustic=0 -> D (dark-blue body) and caustic=1 -> white (bright highlight).
   D is the dark translucent body color; tune freely. */
#define MORPHA_BODY_R  0
#define MORPHA_BODY_G  0
#define MORPHA_BODY_B  (32/2)

#if WALK_STATS
#define TIMED(acc, call) do { uint64 _tt = timer_ns_gettime64(); call; acc += timer_ns_gettime64() - _tt; } while (0)
#else
#define TIMED(acc, call) do { (void)(acc); call; } while (0)
#endif
static uint64 sPA_FrameMin = UINT64_MAX;
static uint64 sPA_FrameMax = 0;
static uint32 sPC_Frames = 0;
static uint64 sProf_ReportTimer = 0;

static u32 sLastTexAddr = 0;
static u32 sLastTexFmt = 0;
static u32 sLastTexSiz = 0;
static u32 sLastTexWidth = 0;
static u32 sLastTexHeight = 0;
static u32 sLastTexLine = 0;
static u32 sLastTexPalette = 0;
static TexHandle sLastTexResult = 0;

static void OnVBlank(uint32_t code, void* data) {
    (void)code; (void)data;
    ++sVblCounter;
    genwait_wake_one((void*)&sVblCounter);
}

static void WaitForVBlank(void) {
    /* +3 = 20 FPS (matching N64 original), +2 = 30, +1 = 60, +0 = uncapped */
    while (sVblCounter < sLastVbl + 3)
#if KOS_VERSION_BELOW(2, 2, 3)
        genwait_wait((void*)&sVblCounter, NULL, 10, NULL);
#else
        genwait_wait((void*)&sVblCounter, NULL, 10);
#endif

    sLastVbl = sVblCounter;
}


#define MAX_TEX_SIZE 512
/* Staging capacity in texels. In-game textures are TMEM-bounded (tiny once
   padded) and the largest real user is the prerender-BG path: 320x240 padded
   to 512x256. The old 512x512 sizing wasted 256KB of main RAM we need back
   (KOS fs_vmu mallocs ~2x the save file transiently; we were right at the
   edge, 2026-09-04). Guards below fail loud if anything ever exceeds this. */
#define TEXBUF_TEXELS (512 * 256)
//512
u16 __attribute__((aligned(32))) sTexDataBuf[TEXBUF_TEXELS];
//u16 __attribute__((aligned(32))) sTexDataBuf16[MAX_TEX_SIZE * MAX_TEX_SIZE];

static u8 __attribute__((aligned(32))) sSwapBuf[256 * 256 * 2];
static u16 sTlutSwapBuf[256]; // 256 entries * 2 bytes max

#define PACK_ARGB8888(r, g, b, a) \
    ((uint32_t)((uint8_t)(a) << 24) | ((uint8_t)(r) << 16) | ((uint8_t)(g) << 8) | (uint8_t)(b))

typedef struct __attribute__((packed, aligned(4))) vec3f_gl {
    float x, y, z;
} vec3f_gl;

typedef struct __attribute__((packed, aligned(4))) uv_float {
    float u, v;
} uv_float;

typedef struct __attribute__((packed)) color_uc_struct {
    unsigned char r, g, b, a;
} color_uc_struct; // RGBA order for PVR ARGB packed

typedef union color_uc {
    color_uc_struct array;
    unsigned int packed;
} color_uc;

typedef struct __attribute__((packed, aligned(4))) dc_fast_t {
    uint32_t flags;
    vec3f_gl vert;
    uv_float texture;
    color_uc color;
    unsigned int oargb;
} dc_fast_t;

// Geometry mode flags
static u32 sGeometryMode = 0;
static u8 sGeometryDirty = 0;
#define G_LIGHTING 0x00020000
#define G_TEXTURE_GEN 0x00040000
#define G_TEXTURE_GEN_LINEAR 0x00080000
#define G_FOG 0x00010000
// Lighting
#define MAX_LIGHTS 8
typedef struct {
    float r, g, b;
    float dir[3]; // normalized direction (for directional lights)
} PCLight;        // Renamed to avoid conflict with N64's Light type

static PCLight sLights[MAX_LIGHTS];
static PCLight sAmbient = { 0.2f, 0.2f, 0.2f, { 0, 0, 0 } };
static int sNumLights = 0;

/* SF64-style matrix lighting. Built per light/matrix change; consumed via XMTRX
   FTRV in handle_vtx (3 passes: position MP, intensity COEFF, color COLOR).
   load_3x3/load_4x4 load C-rows as XMTRX columns, so transform(v)_i =
   dot(C-matrix column_i, v).
   - sCoeffMtx[component][light]: column i = obj-space dir of light i * recip127,
     so transform_vec3(RAW s8 normal)_i = cos_i (no per-vertex normalize).
   - sColorMtx[input][channel]: rows 0..2 = light0/1/2 RGB, row 3 = ambient RGB;
     transform_vec4((i0,i1,i2,1)) = (R,G,B,_).
   - sEnvMtx[component][lookat]: cols = obj-space lookat X/Y * recip127 (texgen). */
static float __attribute__((aligned(32))) sCoeffMtx[3][3];
static float __attribute__((aligned(32))) sColorMtx[4][4];
static float __attribute__((aligned(32))) sEnvMtx[3][3];
/* Second batch (only built/used when sNumLights > 3): up to 4 MORE directionals,
   no ambient. COEFF2 is 4x4 so one FTRV(normal, w=0) yields 4 cosines (.x..w);
   COLOR2 rows 0..3 = those 4 light colors. Lets the renderer reach the N64 max
   of 7 directional + 1 ambient (3+amb in batch 1, +4 in batch 2). */
static float __attribute__((aligned(32))) sCoeffMtx2[4][4];
static float __attribute__((aligned(32))) sColorMtx2[4][4];
static int sLightMtxDirty = 1;

// Lookat vectors for G_TEXTURE_GEN (stored from gSPLookAt)
static float sLookAtX[3] = { 1.0f, 0.0f, 0.0f };
static float sLookAtY[3] = { 0.0f, 1.0f, 0.0f };
static int sLookAtValid = 0;

#define G_MV_VIEWPORT 8
#define G_MV_LIGHT    10

static struct {
    float __attribute__((aligned(32))) modelview_stack[18][4][4];
    int modelview_stack_size;
    float __attribute__((aligned(32))) P_matrix[4][4];
    float __attribute__((aligned(32))) MP_matrix[4][4];
    float texture_scale_s; // from gSPTexture
    float texture_scale_t;
}  __attribute__((aligned(32))) rsp;

typedef struct {
    uint8_t r, g, b, a;
} RGBA;

// Exactly 32 bytes - one SH4 cache line
// Pre-transformed at VTX time: screen coords + inv_w ready for PVR
typedef struct __attribute__((aligned(32))) {
    float screen_x, screen_y; // 8 bytes: PVR-ready (viewport + ×2)
    float inv_w;              // 4 bytes: 1/w for PVR Z
    float u, v;               // 8 bytes: texture coords
    RGBA color;               // 4 bytes: vertex color/lighting
    u8 behind;                // 1 byte: w < NEAR_PLANE flag
    u8 fog;                   // 1 byte: "rsp fog" coefficient
    u8 _pad[6];               // 6 bytes: padding to 32 (one SH4 cache line per vertex)
} LoadedVertex;

#define UCODE_MAX_VERTS 32
static LoadedVertex __attribute__((aligned(32))) sVertexBuffer[UCODE_MAX_VERTS];
/* Per-vertex light intensities (i0,i1,i2,1) between the COEFF and COLOR passes. */
static shz_vec4_t __attribute__((aligned(32))) sLightInten[UCODE_MAX_VERTS];
/* Batch-2 scratch (only used when sNumLights > 3): intensities (i3,i4,i5,i6) and
   the unclamped batch-1 RGB accumulator we add batch-2 onto before clamping once. */
static shz_vec4_t __attribute__((aligned(32))) sLightInten2[UCODE_MAX_VERTS];
static float __attribute__((aligned(32))) sLightAccum[UCODE_MAX_VERTS][3];
static uint8_t __attribute__((aligned(32))) sVertexClipCodes[UCODE_MAX_VERTS];
/* near-clip scratch — file-level statics (push_triangle is not reentrant) */
static LoadedVertex sClipSpare[2];
static LoadedVertex* sClipOutv[4];
// Clip-space coords — only needed for near-plane clipping (rare)
static float __attribute__((aligned(32))) sClipSpace[UCODE_MAX_VERTS][4];

#if 0
typedef struct {
    float u_scale; // original_width / padded_width
    float v_scale; // original_height / padded_height
    int padded_w;
    int padded_h;
} TexPow2Info;
#endif

float global_dummy[4] = {1.0f, 1.0f, 1.0f, 1.0f};
float global_combined[4];

#define MAX_TEXTURES TEX_HASH_SIZE
//static TextureCacheEntry __attribute__((aligned(32))) sTextureCache[MAX_TEXTURES];
//static int sTextureCacheSize = 0;

static struct __attribute__((aligned(64))) {
    TextureCacheEntry __attribute__((aligned(64))) pool[MAX_TEXTURES];
    TextureCacheEntry* hashmap[MAX_TEXTURES];
    uint32_t pool_pos;
} sTextureCache;

//static bool sTextureHidden[MAX_TEXTURES];

static u32 sCurTexAddr = 0;
static u32 sCurTexWidth = 0;
static u32 sCurTexFmt = 0;
static u32 sCurTexSiz = 0;

static u8 sDoAdd = 0;

float sPrimColor[4] = { 1, 1, 1, 1 };
static float sEnvColor[4] = { 1, 1, 1, 1 };
static float sPrimLodFrac = 0.0f;  // from gDPSetPrimColor lodfrac parameter

static struct {
    int a0, b0, c0, d0;
    int a1, b1, c1, d1;
    int aa0, ab0, ac0, ad0;  /* alpha mux values (separate from color) */
    int aa1, ab1, ac1, ad1;
    int uses_shade;
    int uses_prim;
    int uses_env;
    int uses_texture;
    int alpha_uses_texture;
} sCombinerState;

TileDescriptor sTiles[8];
static TextureCacheEntry* sCurrentboundTexture = NULL;
static int sCurrentTexWidth = 1;
static int sCurrentTexHeight = 1;
static float sCurrentUls = 0;
static float sCurrentUlt = 0;
static int sEmitCacheDirty = 1;   /* emit-invariant cache (see emit_cache_rebuild) */

static u32 sOtherModeL = 0;
static u32 sOtherModeH = 0;

// Map N64 texture filter (otherModeH bits 13:12) to PVR filter
static inline int pvr_tex_filter(void) {
    int filt = (sOtherModeH >> 12) & 0x3;
    return (filt == 0) ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR;
}
static int sBlendingEnabled = 1;
static int sTlutType = 0;
static u16 __attribute__((aligned(32))) sTLUT[256];
static int sTlutFromNativeMemory = 0;
static float sCurrentTexUScale = 1.0f;
static float sCurrentTexVScale = 1.0f;

static inline u32 tex_hash(u32 addr, u32 width, u32 height) {
    u32 v = addr;
    v ^= v >> 16;
    v *= 0x45d9f3b;
    v ^= v >> 16;
    v ^= width | (height << 8);
    return v & (TEX_HASH_SIZE - 1);
}

static int find_texture_cache_index(TexHandle texID);

static TextureCacheEntry* get_texture(u32 addr, u32 fmt, u32 siz, u32 width, u32 height, u32 line, u32 cms, u32 cmt, u32 palette,
                          u32 src_stride);
static void get_texture_uv_scale(TextureCacheEntry* texID, float* u_scale, float* v_scale);

/* Mark any cached texture at this RAM address as dirty.
   Called from game code (e.g. Font_LoadChar) when data at an address
   is overwritten.  Next get_texture for this address will re-decode. */
void gfx_texture_cache_invalidate(void* addr) {
    u32 phys = (u32)(uintptr_t)addr;

    /* Bust the fast path so get_texture doesn't short-circuit */
    if (sLastTexAddr == phys)
        sLastTexAddr = 0;

    for (int i = 0; i < sTextureCache.pool_pos; i++) {
        if (sTextureCache.pool[i].addr == phys) {
            sTextureCache.pool[i].dirty = 1;
            break;
        }
    }

    /* Mark any tile referencing this address as needing re-resolve */
    for (int i = 0; i < 8; i++) {
        if (sTiles[i].texAddr == phys)
            sTiles[i].texDirty = 1;
    }
}

static float s2DDepth = 200.0f;//0.000001f;
#define OVERLAY_Z_START 200.0f
//0.000001f
#define OVERLAY_Z_STEP  0.01f
//0.0000005f

/* Cached PVR polygon headers - rebuilt only when state changes */
static pvr_poly_hdr_t __attribute__((aligned(32))) sPvrHdrCol;
static pvr_poly_hdr_t __attribute__((aligned(32))) sPvrHdrTex;
static pvr_poly_hdr_t __attribute__((aligned(32))) sPvrHdrNoDepthCol;  /* 2D untextured (fillrect) */
static TexHandle sPvrHdrTexId = 0;
static int sPvrColDirty = 1;
static int sPvrTexDirty = 1;
pvr_dr_state_t sDrState;
int sPvrCurrentList = PVR_LIST_OP_POLY;
u32 sPvrFrameBytes = 0;
/* EXPERIMENT: start of the POLY_XLU display-list buffer, set by graph.c each
   frame. walk_dl forces the OP->TR switch when a branch targets it, so the
   opaque/translucent split is driven by the game's own DL buffers instead of
   per-render-mode blend detection. */
Gfx* gXluDLStart = NULL;
static u32 sDbgFastEmit = 0, sDbgClipEmit = 0;
static u32 sDbgTriIn = 0;   /* triangles entering push_triangle (pre-cull) */
static u32 sDbgPolyOP = 0, sDbgPolyTR = 0, sDbgPolyPT = 0;
static int sPvrOnTrList = 0;   /* 1 = already switched to TR list this frame */
static int sIsOrtho = 0;      /* 1 = projection is orthographic (overlay/2D) */
static int sPvrOnPtList = 0;   /* 1 = already switched to PT list this frame */
static int sPvrNeedHeader = 1; /* 1 = must submit header before next vertices */
static int sPvrScisX1 = 0, sPvrScisY1 = 0, sPvrScisX2 = DC_FB_W, sPvrScisY2 = DC_FB_H;

/* ---- Multi-texture pass 2 buffering ----
   When the combiner references TEXEL1 (e.g. ground detail blend), we render
   pass 1 normally with tile 0, then buffer pass 2 vertices (tile 1 UVs,
   env_alpha blend) and flush them onto the TR list later. */
typedef struct {
    float x, y, z;
    float u, v;                     /* tile 1 UVs (fire/detail) */
    float u0, v0;                   /* tile 0 UVs (mask/base) */
    u32 argb;
    u32 oargb;
} MtxVert;

typedef struct {
    int start, count;
    TextureCacheEntry* texID;       /* tile 1 (fire/detail) */
    int padW, padH;
    u32 cms, cmt;
    TextureCacheEntry* tex0;        /* tile 0 (mask/base) */
    int pad0W, pad0H;
    u32 cms0, cmt0;
    int needs_stencil;              /* 1 = alpha depends on texture (fire mask) */
} MtxBatch;

#define MAX_MTX_VERTS   4096
#define MAX_MTX_BATCHES 160
static MtxVert  sMultiTexVerts[MAX_MTX_VERTS];
static MtxBatch sMultiTexBatches[MAX_MTX_BATCHES];
static int sMultiTexVertCount  = 0;
static int sMultiTexBatchCount = 0;
static int sMultiTexEnabled    = 0;  /* set by parse_combiner */

/* Texture substitution values for combiner evaluation.
   Default 1.0 = "texture present, PVR handles sampling via MODULATEALPHA".
   Set to 0.0 to evaluate what the combiner produces WITHOUT that texture.
   Used by multi-tex texrect path to decompose 2-cycle combiners into
   per-texture contributions that can be rendered in separate passes. */
static float sTexSubst0 = 1.0f;
static float sTexSubst1 = 1.0f;

/* ---- Punch-through (PT) deferred buffer ----
   Alpha-test-only geometry is buffered here during the frame and flushed
   onto PVR_LIST_PT_POLY after TR is finished, respecting PVR list order. */
typedef struct {
    int start, count;
    pvr_poly_hdr_t hdr;  /* compiled header for this batch */
} PtBatch;

#define MAX_PT_VERTS   6144
#define MAX_PT_BATCHES 256
static MtxVert  sPtVerts[MAX_PT_VERTS];
static PtBatch  sPtBatches[MAX_PT_BATCHES];
static int sPtVertCount   = 0;
static int sPtBatchCount  = 0;
static int sPtBuffering   = 0;  /* 1 = currently buffering for PT instead of submitting */

/* ---- Translucent (TR) deferred buffer ----
   Blended geometry is buffered here during the frame and flushed onto
   PVR_LIST_TR_POLY at end of frame, so OP is NEVER finished mid-frame and
   opaque geometry that follows a blend in DL order still lands on OP. Replaces
   the old one-way pvr_ensure_tr_list() switch (kept only as overflow fallback).
   ~221KB at 6144 MtxVert; covers ~2048 deferred TR triangles/frame. */
typedef struct {
    int start, count;
    pvr_poly_hdr_t hdr;  /* compiled header for this batch */
    int is_mod;          /* lens bracket: 1 = textured modifier batch, 2 = untextured */
} TrBatch;

#define MAX_TR_VERTS   6144
#define MAX_TR_BATCHES 256
static MtxVert  sTrVerts[MAX_TR_VERTS];
static TrBatch  sTrBatches[MAX_TR_BATCHES];
static int sTrVertCount   = 0;
static int sTrBatchCount  = 0;
static int sTrBuffering    = 0;  /* 1 = buffering blended geometry for deferred TR */

/* ---- Lens of Truth state (see the lens block before draw_texrect) ----
   The mask texrect arms the circle; 'BLND'/'LNS1'..'LNS0' DL sentinels
   (emitted by Actor_DrawLensActors) bracket the lens-actor draws. Bracketed
   geometry is forced onto the deferred TR buffer with MODIFIER-affected
   headers (param set 2 = hidden); a screen-space circle volume on the TR_MOD
   list then masks ONLY that geometry, per pixel -- world geometry, the PT
   cutouts and the multitex passes are untouched (the earlier depth-prime
   approach poisoned the shared depth buffer for the whole PT/TR phase). */
static pvr_ptr_t sLensVisTexPvr = NULL;   /* baked visible window (shroud/glasses) */
static u32 sLensVisSrc = 0;               /* texAddr the bake came from */
/* bake staging borrows sTexDataBuf (idle at texrect time) -- RAM is too tight
   for a dedicated 32KB buffer (newlib Balloc abort at the title screen, 2026-09-04) */
static u32 sLensLog = 0;                  /* capped diagnostics */
static u32 sPrimDepthZ = 0xFFFF;          /* last G_SETPRIMDEPTH z (0 = nearest) */
static float sLensRadTex = 60.0f;         /* circle radius in window texels (measured at bake) */
static int sLensBracket = 0;              /* inside the LNS1..LNS0 actor bracket */
static int sLensMaskSeen = 0;             /* a lens mask texrect armed this frame */
static int sLensShow = 0;                 /* 1 = SHOW_ACTORS polarity (visible INSIDE circle) */
static int sLensVolNeeded = 0;            /* emit the TR_MOD circle volume this frame */
static float sLensCx, sLensCy, sLensRx, sLensRy;  /* circle in screen coords */

static void pvr_rebuild_col_header(void);
static void pvr_rebuild_tex_header(TextureCacheEntry* entry);
static void pvr_submit_quad_col(float x0, float y0, float x1, float y1, float z, u32 argb);
static void pvr_submit_quad_tex(float x0, float y0, float x1, float y1, float z,
                                float u0, float v0, float u1, float v1, u32 argb, int flip);

/* set PVR vertex fog color register */
static void pvr_vertfog_color(float a, float r, float g, float b) {
    PVR_SET(0x0B4, PVR_PACK_COLOR(a, r, g, b));
}

/* PVR user tile clip  */
static void pvr_submit_user_clip(int x1, int y1, int x2, int y2) {
    uint32_t __attribute__((aligned(32))) clip[8] = {0};
    clip[0] = 0x20000000; /* User tile clip command (param type 1) */
    clip[4] = x1 / 32;
    clip[5] = y1 / 32;
    clip[6] = (x2 > 0 ? x2 - 1 : 0) / 32;
    clip[7] = (y2 > 0 ? y2 - 1 : 0) / 32;
    SHZ_PREFETCH(clip);
    shz_sq_memcpy32_1(pvr_dr_target(sDrState), clip);
    sPvrFrameBytes += 32;
}

void flush_texture_cache_external(void) {
    printf("\n\n!!! TEXTURE CACHE WIPED !!!\n\n");

    for (int i = 0; i < sTextureCache.pool_pos; i++) {
        if (sTextureCache.pool[i].id) {
            pvr_mem_free(sTextureCache.pool[i].id);
        }
    }

    memset(&sTextureCache, 0, sizeof(sTextureCache));

    sLastTexAddr = 0;
    sLastTexResult = 0;

    // Clear stale texture IDs and mark all tiles dirty for lazy re-resolve.
    for (int i = 0; i < 8; i++) {
        sTiles[i].texture = NULL;
        if (sTiles[i].texAddr != 0 && sTiles[i].width > 0 && sTiles[i].height > 0)
            sTiles[i].texDirty = 1;
    }
    sCurrentboundTexture = NULL;
    sPvrColDirty = 1;
    sPvrTexDirty = 1;
    sPvrHdrTexId = 0;
}

static void flush_texture_cache(void) {
    // Drain any buffered triangles BEFORE deleting textures —
    // they reference sCurrentboundTexture which is about to be freed.
    flush_triangles();

    for (int i = 0; i < sTextureCache.pool_pos; i++) {
        if (sTextureCache.pool[i].id) {
            pvr_mem_free(sTextureCache.pool[i].id);
        }
    }

    memset(&sTextureCache, 0, sizeof(sTextureCache));

    sLastTexAddr = 0;
    sLastTexResult = 0;

    // Clear stale texture IDs and mark all tiles dirty for lazy re-resolve.
    for (int i = 0; i < 8; i++) {
        sTiles[i].texture = NULL;
        if (sTiles[i].texAddr != 0 && sTiles[i].width > 0 && sTiles[i].height > 0)
            sTiles[i].texDirty = 1;
    }
    sCurrentboundTexture = NULL;
    sPvrColDirty = 1;
    sPvrTexDirty = 1;
    sPvrHdrTexId = 0;
}

static void resolve_tile_texture(int tile) {
    if (!sTiles[tile].texDirty) return;
    sTiles[tile].texDirty = 0;
    u32 texAddr = sTiles[tile].texAddr ? sTiles[tile].texAddr : sCurTexAddr;
    TextureCacheEntry* entry = get_texture(
        texAddr, sTiles[tile].fmt, sTiles[tile].siz,
        sTiles[tile].width, sTiles[tile].height,
        sTiles[tile].line, sTiles[tile].texCms, sTiles[tile].texCmt,
        sTiles[tile].palette, sTiles[tile].src_stride);
    sTiles[tile].texture = entry;
}

static u32 compute_tlut_checksum(void) {
    u32 sum = 0;
    for (int i = 0; i < 256; i++)
        sum = sum /* * 31 */ + sTLUT[i];
    return sum;
}

static u16 ia16_to_rgba16(u16 p) {
    u8 i = ((p >> 8) & 0xFF) >> 4;
    i &= 0xf;
    u8 a = (p & 0xFF) >> 4;
    a &= 0xf;
    return (a << 12) | (i << 8) | (i << 4) | i;
}

static void apply_depth_settings(void) {
    int z_cmp = (sOtherModeL & Z_CMP) != 0;
    int z_upd = (sOtherModeL & Z_UPD) != 0;

    if (z_cmp != sDepthTestEnabled) {
        flush_triangles();
        sDepthTestEnabled = z_cmp;
        sPvrColDirty = 1;
        sPvrTexDirty = 1;
    }

    if (z_upd != sDepthWriteEnabled) {
        flush_triangles();
        sDepthWriteEnabled = z_upd;
        sPvrColDirty = 1;
        sPvrTexDirty = 1;
    }
}

/* Textures with multi-bit alpha (IA, I, CI) need ARGB4444.
   RGBA textures only have 1-bit alpha → use ARGB1555 for full RGB precision. */
static inline int tex_needs_multi_alpha(u32 fmt) {
    return (fmt == G_IM_FMT_IA || fmt == G_IM_FMT_I || (fmt == G_IM_FMT_CI && sTlutType == 3));
}

static inline u32 pvr_tex_fmt_for(u32 fmt, u32 siz) {
    if (siz == G_IM_SIZ_32b) return PVR_TXRFMT_ARGB4444;
    return tex_needs_multi_alpha(fmt) ? PVR_TXRFMT_ARGB4444 : PVR_TXRFMT_ARGB1555;
}

/* ==== VisMono (flashback desaturation), source-side ==========================
   The N64 effect reads the finished framebuffer as a texture, converts each
   pixel to intensity and lerps env(black)->prim by it, blended by prim alpha.
   PVR can't read the cfb usefully, so the same transform is applied to every
   COLOR SOURCE instead (user's design, 2026-09-04): all texels at conversion
   time, all final vertex/texrect/fill colors at emit time, and the fog color.
   Any blend of grays is gray, so the frame is monochrome by construction; only
   relative brightness can differ slightly (luma of a product vs product of
   lumas). On toggle/change the texture cache (and the BG cache) is wiped so
   everything re-bakes -- naive per-frame rebake if the alpha ramps (sepia
   cutscene cmd); optimize later if it's too slow. */
static u32 sVisMonoWant = 0;   /* 0xAARRGGBB prim (A = blend alpha); 0 = off */
static u32 sVisMonoCur = 0;    /* currently baked into textures/colors */

void dc_vismono_set(u8 r, u8 g, u8 b, u8 a) {
    sVisMonoWant = a ? (((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | b) : 0;
}

static inline u32 vismono_argb(u32 argb) {
    if (!sVisMonoCur)
        return argb;
    u32 a = sVisMonoCur >> 24;
    u32 pr = (sVisMonoCur >> 16) & 0xFF, pg = (sVisMonoCur >> 8) & 0xFF, pb = sVisMonoCur & 0xFF;
    u32 r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    u32 I = (2 * r + 4 * g + b + 3) / 7;
    u32 tr = (pr * I + 127) / 255, tg = (pg * I + 127) / 255, tb = (pb * I + 127) / 255;
    r = (tr * a + r * (255 - a) + 127) / 255;
    g = (tg * a + g * (255 - a) + 127) / 255;
    b = (tb * a + b * (255 - a) + 127) / 255;
    return (argb & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

/* In-place transform of converted texels (only two formats exist in this
   pipeline; BG uses ARGB1555 too). Alpha bits untouched. */
static void vismono_bake_texels(u16* p, u32 n, u32 pvrfmt) {
    if (!sVisMonoCur)
        return;
    if (pvrfmt == PVR_TXRFMT_ARGB4444) {
        for (u32 i = 0; i < n; i++) {
            u16 v = p[i];
            u32 r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
            u32 c = vismono_argb(0xFF000000u | (r * 17 << 16) | (g * 17 << 8) | (b * 17));
            p[i] = (v & 0xF000) | ((((c >> 16) & 0xFF) >> 4) << 8) |
                   ((((c >> 8) & 0xFF) >> 4) << 4) | ((c & 0xFF) >> 4);
        }
    } else { /* ARGB1555 */
        for (u32 i = 0; i < n; i++) {
            u16 v = p[i];
            u32 r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
            u32 c = vismono_argb(0xFF000000u | (((r << 3) | (r >> 2)) << 16) |
                                 (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2)));
            p[i] = (v & 0x8000) | ((((c >> 16) & 0xFF) >> 3) << 10) |
                   ((((c >> 8) & 0xFF) >> 3) << 5) | ((c & 0xFF) >> 3);
        }
    }
}

static void swap_bytes_64(u8* data, size_t size) {
    for (size_t i = 0; i + 7 < size; i += 8) {
        u8 tmp;
        tmp = data[i + 0];
        data[i + 0] = data[i + 7];
        data[i + 7] = tmp;
        tmp = data[i + 1];
        data[i + 1] = data[i + 6];
        data[i + 6] = tmp;
        tmp = data[i + 2];
        data[i + 2] = data[i + 5];
        data[i + 5] = tmp;
        tmp = data[i + 3];
        data[i + 3] = data[i + 4];
        data[i + 4] = tmp;
    }
}

void* pc_resolve_addr(u32 addr) {
    if (addr == 0)
        return NULL;
    u32 seg = (addr >> 24) & 0xFF;
    if (seg <= 0x0F) {
        if (gSegments[seg] == 0)
            return NULL;
        return (void*)(gSegments[seg] + (addr & 0x00FFFFFF));
    }
    return (void*)(uintptr_t)addr;
}

// Get the UV scale for a texture
static void get_texture_uv_scale(TextureCacheEntry *entry, float* u_scale, float* v_scale) {
    if (entry) {
        *u_scale = entry->pow2Info.u_scale;
        *v_scale = entry->pow2Info.v_scale;
    } else {
        *u_scale = 1.0f;
        *v_scale = 1.0f;
    }
}

static void pad_texture_to_pow2(u16* data, int srcW, int srcH, int dstW, int dstH) {
    // Process rows from bottom to top for safe in-place expansion
    for (int y = srcH - 1; y >= 0; y--) {
        u16* srcRow = &data[y * srcW];
        u16* dstRow = &data[y * dstW];

        // Move the row data (memmove handles overlap)
        if (srcRow != dstRow) {
            memmove(dstRow, srcRow, srcW * sizeof(u16));
        }

        // Replicate rightmost pixel into padding (instead of black)
        if (dstW > srcW) {
            u16 edgePixel = dstRow[srcW - 1];
            for (int x = srcW; x < dstW; x++) {
                dstRow[x] = edgePixel;
            }
        }
    }

    // Replicate bottom row into padding rows (instead of black)
    if (dstH > srcH) {
        u16* lastValidRow = &data[(srcH - 1) * dstW];
        for (int y = srcH; y < dstH; y++) {
            memcpy(&data[y * dstW], lastValidRow, dstW * sizeof(u16));
        }
    }
}

static void convert_rgba32(u8 *swapped, u16* tex_data, u32 count) {
    u32 *swapped32 = (u32*)swapped;
    for (u32 i=0;i<count;i++) {
        u32 p = swapped32[i];
        uint8_t a = (p >> 28) & 0xf;
        uint8_t b = (p >> 20) & 0xf;
        uint8_t g = (p >> 12) & 0xf;
        uint8_t r = (p >> 4) & 0xf;
        tex_data[i] = (a << 12) | (r << 8) | (g << 4) | (b);
    }
}

static void convert_rgba16(u8* swapped, u16* tex_data, u32 count) {
    u16* src16 = (u16*)swapped;
    for (u32 i = 0; i < count; i++) {
        u16 val = __builtin_bswap16(src16[i]);
        tex_data[i] = (val << 15) | (val >> 1);
    }
}

static void convert_ia16(u8* swapped, u16* tex_data, u32 count) {
    u16* src16 = (u16*)swapped;
    for (u32 i = 0; i < count; i++) {
        u16 val = src16[i];
        uint8_t al = (val >> 12) & 0xF;
        uint8_t in = (val >> 4) & 0xF;
        tex_data[i] = (al << 12) | (in << 8) | (in << 4) | in;
    }
}

static void convert_ia8(u8* swapped, u16* tex_data, u32 count) {
    for (u32 i = 0; i < count; i++) {
        uint8_t val = swapped[i];
        uint8_t in = ((val >> 4) & 0xf);
        uint8_t al = (val & 0xf);
        tex_data[i] = (al << 12) | (in << 8) | (in << 4) | in;
    }
}

static void convert_ia4(u8* swapped, u16* tex_data, u32 count) {
    for (u32 i = 0; i < count; i++) {
        u8 byte = swapped[i / 2];
        u8 nibble = (i % 2) ? (byte & 0xF) : (byte >> 4);
        /* IA4 = 3-bit intensity + 1-bit alpha. Expand the 3-bit intensity to the
           full 4-bit ARGB4444 range by bit-replication (7 -> 15), otherwise a
           white texel lands at 7/15 (~47%) and renders dark gray. */
        u8 i3 = (nibble >> 1) & 0x7;
        u8 intensity = (i3 << 1) | (i3 >> 2);
        u8 alpha = nibble & 1;
        alpha = alpha ? 15 : 0;
        tex_data[i] = (alpha << 12) | (intensity << 8) | (intensity << 4) | intensity;
    }
}

static void convert_i8(u8* swapped, u16* tex_data, u32 count) {
    for (u32 i = 0; i < count; i++) {
        u8 val = (swapped[i] >> 4) & 0xF;
        tex_data[i] = (val << 12) | (val << 8) | (val << 4) | val;
    }
}

static void convert_i4(u8* swapped, u16* tex_data, u32 count) {
    SHZ_PREFETCH(swapped);
    for (u32 i = 0; i < count; i++) {
        u8 byte = swapped[i / 2];
        u8 val = (i % 2) ? (byte & 0xF) : ((byte >> 4) & 0xF);
        tex_data[i] = (val << 12) | (val << 8) | (val << 4) | val;
    }
}

static void convert_ci8_nostride(u8* swapped, u16* tex_data, u32 count) {
    SHZ_PREFETCH(swapped);
    for (u32 i = 0; i < count; i++) {
        tex_data[i] = sTLUT[swapped[i]];
    }
}

static void convert_ci8_strided(u8* src_data, u16* tex_data, int src_stride, int width, int height) {
    u8* base = (u8*)src_data;
    uintptr_t misalign = ((uintptr_t)base) & 7;
    u8* aligned = base - misalign;
    SHZ_PREFETCH(src_data);

    for (u32 t = 0; t < height; t++) {
        for (u32 s = 0; s < width; s++) {
            u32 pos = misalign + t * src_stride + s;
            u32 swapped_pos = (pos & ~7) | (7 - (pos & 7));
            u8 idx2 = aligned[swapped_pos];
            u16 tlut_val = sTLUT[idx2];
            tex_data[t * width + s] = tlut_val;
        }
    }
}

static void convert_ci4(u8* swapped, u16* tex_data, int palette, u32 count) {
    for (u32 i = 0; i < count; i++) {
        u8 byte = swapped[i / 2];
        u8 nibble = (i % 2) ? (byte & 0xF) : (byte >> 4);
        tex_data[i] = sTLUT[(palette << 4) | nibble];
    }
}

static TextureCacheEntry* get_texture(u32 addr, u32 fmt, u32 siz, u32 width, u32 height, u32 line, u32 cms, u32 cmt, u32 palette,
                          u32 src_stride) {
    PROF_BEGIN(sPF_Tex);
    (void)cms; (void)cmt; /* wrap modes read from sTiles[] in header builder */

    void* src_data = pc_resolve_addr(addr);
    if (!src_data || width == 0 || height == 0) {
        PROF_END(sPF_Tex);
        return (TextureCacheEntry*)NULL;
    }

    u32 phys_addr = (u32)(uintptr_t)src_data;

    // Compute TLUT checksum once if needed
    if (fmt == G_IM_FMT_CI && sTlutDirty) {
        sTlutChecksum = compute_tlut_checksum();
        sTlutDirty = 0;
    }

    u32 hash = tex_hash(phys_addr, width, height);
    /* Single-compare match key: address (32-byte granularity) + palette. Address
       uniquely identifies fmt/siz/width/height/line, so they're not in the key. */
    u32 newkey = ((phys_addr >> 5) << 4) | (palette & 0xF);
    TextureCacheEntry** node = &sTextureCache.hashmap[hash];
    TextureCacheEntry* dirtyNode = NULL;

    uintptr_t last_node = &sTextureCache.pool[sTextureCache.pool_pos];
    while ((*node) != NULL && ((uintptr_t)(*node) < last_node)) {
        __builtin_prefetch((*node)->next);
        TextureCacheEntry* e = (*node);
        if (e->key == newkey) {
            /* Check for a dirty entry at this key — may later reuse its slot + PVR VRAM */
            if (e->dirty) {
                dirtyNode = e;
                break;
            } else {
                /* tlutType is subsumed by tlutVersion: the checksum is computed over
                   the already-type-converted sTLUT[], so type is baked into it. */
                if ((fmt != G_IM_FMT_CI) || (e->tlutVersion == sTlutChecksum)) {
                    sLastTexAddr = phys_addr;
                    sLastTexFmt = fmt;
                    sLastTexSiz = siz;
                    sLastTexWidth = width;
                    sLastTexHeight = height;
                    sLastTexLine = line;
                    sLastTexPalette = palette;
                    sLastTexResult = e->id;
                    return e;
                }
            }
        }

        node = &(*node)->next;
    }

/*    if (!dirtyNode && sTextureCache.pool_pos >= TEX_HASH_SIZE) {
        printf("emergency, the pool is full!\n");
        flush_texture_cache();
        return NULL;
    }*/

    if (dirtyNode) {
        *node = dirtyNode;
    } else {
        *node = &sTextureCache.pool[sTextureCache.pool_pos++];
        (*node)->next = NULL;
    }

    /* True cache miss — must decode + upload */
    PROF_BEGIN(sPF_TexUpload);

    u32 padded_w = next_pot(width);
    u32 padded_h = next_pot(height);

    if (padded_w < 8)
        padded_w = 8;
    if (padded_h < 8)
        padded_h = 8;
    if (padded_w > 256)
        padded_w = 256;
    if (padded_h > 256)
        padded_h = 256;

    float u_scale = (float)width / (float)padded_w;
    float v_scale = (float)height / (float)padded_h;

    size_t tex_bytes;
    if (siz == G_IM_SIZ_4b)
        tex_bytes = (width * height + 1) / 2;
    else if (siz == G_IM_SIZ_8b)
        tex_bytes = width * height;
    else if (siz == G_IM_SIZ_16b)
        tex_bytes = width * height * 2;
    else
        tex_bytes = width * height * 4;

    if (padded_w * padded_h > TEXBUF_TEXELS) {
        PROF_END(sPF_TexUpload);
        PROF_END(sPF_Tex);
        if (!dirtyNode) {
            sTextureCache.pool_pos--;
        }
        *node = NULL;
        return (TextureCacheEntry*)NULL;
    }

    u16* tex_data = sTexDataBuf;
    u8* swapped = sSwapBuf;
    // dont do this memcpy shit
    // only problem is... you'd have to modify whatever creates the assets
    // and DONT OUTPUT AS U64
    memcpy(swapped, src_data, tex_bytes);
    swap_bytes_64(swapped, tex_bytes);

    u32 count = width * height;

    switch (fmt) {
        case G_IM_FMT_RGBA: {
            switch (siz) {
                case G_IM_SIZ_16b:
                    convert_rgba16(swapped, tex_data, count);
                    break;
                case G_IM_SIZ_32b:
                    convert_rgba32(swapped, tex_data, count);
                    break;
                default:
                    break;
            }
        }
        break;

        case G_IM_FMT_IA: {
            switch (siz) {
                case G_IM_SIZ_16b:
                    convert_ia16(swapped, tex_data, count);
                    break;
                case G_IM_SIZ_8b:
                    convert_ia8(swapped, tex_data, count);
                    break;
                case G_IM_SIZ_4b:
                    convert_ia4(swapped, tex_data, count);
                    break;
                default:
                    break;
            
            }
        }
        break;

        case G_IM_FMT_I: {
            switch (siz) {
                case G_IM_SIZ_8b:
                    convert_i8(swapped, tex_data, count);
                    break;
                case G_IM_SIZ_4b:
                    convert_i4(swapped, tex_data, count);
                    break;
                default:
                    break;
            }
        }
        break;

        case G_IM_FMT_CI: {
            switch (siz) {
                case G_IM_SIZ_8b:
                    if (src_stride == 0 || src_stride == width)
                        convert_ci8_nostride(swapped, tex_data, count);
                    else
                        convert_ci8_strided(src_data, tex_data, src_stride, width, height);
                    break;
                case G_IM_SIZ_4b:
                    convert_ci4(swapped, tex_data, palette, count);
                    break;
                default:
                    break;
            }
        }
        break;
    }

    vismono_bake_texels(tex_data, (u32)(width * height), pvr_tex_fmt_for(fmt, siz));

    if (width != padded_w || height != padded_h)
        pad_texture_to_pow2(tex_data, width, height, padded_w, padded_h);

    TexHandle id;
    int cache_idx;

    if (dirtyNode) {
        /* Reuse the dirty entry's PVR VRAM — same size guaranteed since
           font glyphs at this address always have the same dimensions. */
        id = dirtyNode->id;
        pvr_txr_load_ex(tex_data, (pvr_ptr_t)id, padded_w, padded_h, PVR_TXRLOAD_16BPP);// * padded_h * 2);
    } else {
        pvr_ptr_t pvr_tex = pvr_mem_malloc(padded_w * padded_h * 2);
        if (!pvr_tex) {
            pvrOOM = 1;
            if (sOomLogged < 64) { sOomLogged++; printf("PVROOM: tex upload alloc %u B failed\n", (unsigned)(padded_w * padded_h * 2)); }
            PROF_END(sPF_TexUpload);
            PROF_END(sPF_Tex);
            if (!dirtyNode) {
                sTextureCache.pool_pos--;
            }
            *node = NULL;
            return NULL;
        }
        pvr_txr_load_ex(tex_data, pvr_tex, padded_w, padded_h, PVR_TXRLOAD_16BPP);// * padded_h * 2);
        id = pvr_tex;
    }

    (*node)->key = newkey;
    (*node)->addr = phys_addr;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    (*node)->width = width;
    (*node)->height = height;
    (*node)->line = line;
    (*node)->palette = palette;
    (*node)->tlutType = sTlutType;
    (*node)->tlutVersion = sTlutChecksum;
    (*node)->id = id;
    (*node)->dirty = 0;

    (*node)->pow2Info.u_scale = u_scale;
    (*node)->pow2Info.v_scale = v_scale;
    (*node)->pow2Info.padded_w = padded_w;
    (*node)->pow2Info.padded_h = padded_h;

    sLastTexAddr = phys_addr;
    sLastTexFmt = fmt;
    sLastTexSiz = siz;
    sLastTexWidth = width;
    sLastTexHeight = height;
    sLastTexLine = line;
    sLastTexPalette = palette;
    sLastTexResult = id;

    PROF_END(sPF_TexUpload);
    PROF_END(sPF_Tex);
    return (*node);
}

static void mtx_identity(float m[4][4]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m[i][j] = (i == j) ? 1.0f : 0.0f;
}

static void mtx_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    // Row-major A*B via SH4 XMTRX: load B^T, apply A^T => B^T * A^T = (AB)^T => row-major AB
    shz_xmtrx_load_apply_store_4x4((float*)res, (const float*)b, (const float*)a);
}

static void rsp_reset(void) {
#if 0
    {   /* DIAG: report previous frame's fog factor range (near vs far) on change */
        static int lastMin = -2, lastMax = -2;
        if (sFogDbgMax >= 0 && (sFogDbgMin != lastMin || sFogDbgMax != lastMax)) {
            lastMin = sFogDbgMin; lastMax = sFogDbgMax;
            printf("FOGRANGE near=%d far=%d\n", sFogDbgMin, sFogDbgMax);
        }
        sFogDbgMin = 999; sFogDbgMax = -1;
    }
#endif
    rsp.modelview_stack_size = 1;
    mtx_identity(rsp.modelview_stack[0]);
    mtx_identity(rsp.P_matrix);
    mtx_identity(rsp.MP_matrix);
    sGeometryMode = 0;
    sGeometryDirty = 0;
    sNumLights = 0;
    sAmbient.r = 0.2f;
    sAmbient.g = 0.2f;
    sAmbient.b = 0.2f;
    memset(sLights, 0, sizeof(sLights));
    sLookAtX[0] = 1.0f; sLookAtX[1] = 0.0f; sLookAtX[2] = 0.0f;
    sLookAtY[0] = 0.0f; sLookAtY[1] = 1.0f; sLookAtY[2] = 0.0f;
    sLookAtValid = 0;
    sViewportScaleX = 160.0f;
    sViewportScaleY = 120.0f;
    sViewportTransX = 160.0f;
    sViewportTransY = 120.0f;

    // Texture scale defaults to 1.0
    rsp.texture_scale_s = 1.0f;
    rsp.texture_scale_t = 1.0f;

    // Initialize combiner state to pass through shade color
    memset(&sCombinerState, 0, sizeof(sCombinerState));
    sCombinerState.d0 = 4;
    sCombinerState.d1 = 0;
    // Reset depth state
    sDepthTestEnabled = 1;
    sDepthWriteEnabled = 1;

    // Reset blend state
    sBlendingEnabled = 0;

    // Reset alpha test state
    sAlphaTestEnabled = 0;
    sFogChanged = 0;
    sFogColChanged = 0;
    /* Safety: clear the damage-flash redirect each frame so a dropped end-bracket
       can't leak the tint into the next frame's geometry. */
    sDamageFlash = 0;
    sDamageCapture = 0;

    sDrawingSkybox = 0;

    sPvrColDirty = 1;
    sPvrTexDirty = 1;
    sPvrHdrTexId = 0;

    sDoAdd = 0;

    // Reset other mode registers
    sOtherModeL = Z_CMP | Z_UPD;
    sOtherModeH = 0;
}

static void recalc_MP(void) {
    mtx_mul(rsp.MP_matrix, rsp.modelview_stack[rsp.modelview_stack_size - 1], rsp.P_matrix);
    sMatrixDirty = 1;
    sLightMtxDirty = 1;   /* light dirs are transformed by the modelview top */
}
#define recip127 0.00787402f
// ============================================================================
// LIGHTING
// ============================================================================
/* Build sCoeffMtx: each column i = (M^T * worldDir_i) * recip127, where M is the
   modelview-top upper-3x3. Dotting a RAW s8 normal against column i then equals
   normalize(M*n) . worldDir_i (the current per-vertex result) without any
   per-vertex normalize -- the basis of the SF64-style fast path. */
/* coeffs = normalize(M^T * worldDir) * recip127. Matches reference
   CalculateNormalDir (M = modelview top); per-vector normalize divides out the
   matrix scale; recip127 folded in so a dot with the RAW s8 normal gives cos. */
static inline void obj_dir(const float* mv, const float* d, float* ox, float* oy, float* oz) {
    float x = mv[0]*d[0] + mv[4]*d[1] + mv[8]*d[2];
    float y = mv[1]*d[0] + mv[5]*d[1] + mv[9]*d[2];
    float z = mv[2]*d[0] + mv[6]*d[1] + mv[10]*d[2];
    float len2 = x*x + y*y + z*z;
    float t = (len2 > 1e-12f) ? (recip127 / sqrtf(len2)) : 0.0f;
    *ox = x*t; *oy = y*t; *oz = z*t;
}
static void build_light_matrices(void) {
    const float* mv = (const float*)rsp.modelview_stack[rsp.modelview_stack_size - 1];
    int n = sNumLights < 3 ? sNumLights : 3;

    /* COEFF: column i = obj-space dir of light i (FTRV(normal)_i = cos_i). */
    for (int i = 0; i < 3; i++) {
        if (i < n)
            obj_dir(mv, sLights[i].dir, &sCoeffMtx[0][i], &sCoeffMtx[1][i], &sCoeffMtx[2][i]);
        else
            sCoeffMtx[0][i] = sCoeffMtx[1][i] = sCoeffMtx[2][i] = 0.0f;
    }

    /* COLOR: rows 0..2 = light0/1/2 RGB, row 3 = ambient RGB; col 3 unused.
       FTRV((i0,i1,i2,1)) = (R,G,B,_). */
    for (int i = 0; i < 3; i++) {
        sColorMtx[i][0] = (i < n) ? sLights[i].r : 0.0f;
        sColorMtx[i][1] = (i < n) ? sLights[i].g : 0.0f;
        sColorMtx[i][2] = (i < n) ? sLights[i].b : 0.0f;
        sColorMtx[i][3] = 0.0f;
    }
    sColorMtx[3][0] = sAmbient.r; sColorMtx[3][1] = sAmbient.g;
    sColorMtx[3][2] = sAmbient.b; sColorMtx[3][3] = 0.0f;

    /* ENV (texgen): col 0 = obj lookat Y, col 1 = obj lookat X. */
    obj_dir(mv, sLookAtY, &sEnvMtx[0][0], &sEnvMtx[1][0], &sEnvMtx[2][0]);
    obj_dir(mv, sLookAtX, &sEnvMtx[0][1], &sEnvMtx[1][1], &sEnvMtx[2][1]);
    sEnvMtx[0][2] = sEnvMtx[1][2] = sEnvMtx[2][2] = 0.0f;

    /* BATCH 2 (only when >3 directionals): lights 3..6, no ambient.
       COEFF2 col j = obj-space dir of light 3+j (FTRV(normal,w=0) lane j = cos);
       row 3 = 0 so the normal's w lane never contributes.
       COLOR2 row j = light 3+j RGB; FTRV((i3,i4,i5,i6)) = (R,G,B,_). */
    if (sNumLights > 3) {
        int n2 = sNumLights - 3;
        if (n2 > 4) n2 = 4;
        for (int j = 0; j < 4; j++) {
            if (j < n2)
                obj_dir(mv, sLights[3 + j].dir, &sCoeffMtx2[0][j], &sCoeffMtx2[1][j], &sCoeffMtx2[2][j]);
            else
                sCoeffMtx2[0][j] = sCoeffMtx2[1][j] = sCoeffMtx2[2][j] = 0.0f;
            sCoeffMtx2[3][j] = 0.0f;

            sColorMtx2[j][0] = (j < n2) ? sLights[3 + j].r : 0.0f;
            sColorMtx2[j][1] = (j < n2) ? sLights[3 + j].g : 0.0f;
            sColorMtx2[j][2] = (j < n2) ? sLights[3 + j].b : 0.0f;
            sColorMtx2[j][3] = 0.0f;
        }
    }

    sLightMtxDirty = 0;
}
// Extended lighting: also outputs transformed normal for texture gen
// ============================================================================
// COLOR COMBINER
// ============================================================================
static void parse_combiner(void) {
    PROF_BEGIN(sPF_Combiner);
    u32 w0 = (u32)(sCombineMode >> 32);
    u32 w1 = (u32)(sCombineMode & 0xFFFFFFFF);

    sCombinerState.a0 = (w0 >> 20) & 0xF;
    sCombinerState.b0 = (w1 >> 28) & 0xF;
    sCombinerState.c0 = (w0 >> 15) & 0x1F;
    sCombinerState.d0 = (w1 >> 15) & 0x7;

    sCombinerState.a1 = (w0 >> 5) & 0xF;
    sCombinerState.b1 = (w1 >> 24) & 0xF;
    sCombinerState.c1 = (w0 >> 0) & 0x1F;
    sCombinerState.d1 = (w1 >> 6) & 0x7;

    sCombinerState.uses_texture =
        (sCombinerState.a0 == 1 || sCombinerState.a0 == 2 || sCombinerState.b0 == 1 || sCombinerState.b0 == 2 ||
         sCombinerState.c0 == 1 || sCombinerState.c0 == 2 || sCombinerState.c0 == 8 || sCombinerState.c0 == 9 ||
         sCombinerState.d0 == 1 || sCombinerState.d0 == 2 || sCombinerState.a1 == 1 || sCombinerState.a1 == 2 ||
         sCombinerState.b1 == 1 || sCombinerState.b1 == 2 || sCombinerState.c1 == 1 || sCombinerState.c1 == 2 ||
         sCombinerState.c1 == 8 || sCombinerState.c1 == 9 || sCombinerState.d1 == 1 || sCombinerState.d1 == 2);

    /* Shade = MUX 4 for 4-bit (a,b,d), MUX 4 or 11 (shade_alpha) for 5-bit (c) */
    sCombinerState.uses_shade =
        (sCombinerState.a0 == 4 || sCombinerState.b0 == 4 || sCombinerState.d0 == 4 ||
         sCombinerState.c0 == 4 || sCombinerState.c0 == 11 ||
         sCombinerState.a1 == 4 || sCombinerState.b1 == 4 || sCombinerState.d1 == 4 ||
         sCombinerState.c1 == 4 || sCombinerState.c1 == 11);

    /* Alpha MUX fields — separate from color mux.
       N64 combiner alpha inputs: 0=COMBINED,1=TEXEL0,2=TEXEL1,3=PRIM,4=SHADE,5=ENV,6=1,7=0
       Bit positions from GBI macros:
       w0: Aa0=[14:12] Ac0=[11:9]
       w1: Ab0=[14:12] Ad0=[11:9] Aa1=[23:21] Ac1=[20:18] Ab1=[5:3] Ad1=[2:0] */
    sCombinerState.aa0 = (w0 >> 12) & 0x7;
    sCombinerState.ac0 = (w0 >> 9) & 0x7;
    sCombinerState.ab0 = (w1 >> 12) & 0x7;
    sCombinerState.ad0 = (w1 >> 9) & 0x7;
    sCombinerState.aa1 = (w1 >> 21) & 0x7;
    sCombinerState.ac1 = (w1 >> 18) & 0x7;
    sCombinerState.ab1 = (w1 >> 3) & 0x7;
    sCombinerState.ad1 = w1 & 0x7;
    sCombinerState.alpha_uses_texture =
        (sCombinerState.aa0 == 1 || sCombinerState.aa0 == 2 ||
         sCombinerState.ab0 == 1 || sCombinerState.ab0 == 2 ||
         sCombinerState.ac0 == 1 || sCombinerState.ac0 == 2 ||
         sCombinerState.ad0 == 1 || sCombinerState.ad0 == 2 ||
         sCombinerState.aa1 == 1 || sCombinerState.aa1 == 2 ||
         sCombinerState.ab1 == 1 || sCombinerState.ab1 == 2 ||
         sCombinerState.ac1 == 1 || sCombinerState.ac1 == 2 ||
         sCombinerState.ad1 == 1 || sCombinerState.ad1 == 2);

    sCombinerState.uses_shade = sCombinerState.uses_shade ||
        (sCombinerState.aa0 == 4 || sCombinerState.ab0 == 4 ||
         sCombinerState.ac0 == 4 || sCombinerState.ad0 == 4 ||
         sCombinerState.aa1 == 4 || sCombinerState.ab1 == 4 ||
         sCombinerState.ac1 == 4 || sCombinerState.ad1 == 4);

    /* Multi-texture: combiner references TEXEL1 (mux value 2) or
       TEXEL1_ALPHA (5-bit mux value 9) */
    sMultiTexEnabled =
        (sCombinerState.a0 == 2 || sCombinerState.b0 == 2 || sCombinerState.d0 == 2 ||
         sCombinerState.c0 == 2 || sCombinerState.c0 == 9 ||
         sCombinerState.a1 == 2 || sCombinerState.b1 == 2 || sCombinerState.d1 == 2 ||
         sCombinerState.c1 == 2 || sCombinerState.c1 == 9);

    PROF_END(sPF_Combiner);
}

// Lookup table for 4-bit inputs (a, b, d)
static inline void get_cc_input_4bit(int mux, float* shade, float* combined, float* out) {
    switch (mux) {
        case 0:
            out[0] = combined[0];
            out[1] = combined[1];
            out[2] = combined[2];
            out[3] = combined[3];
            break;
        case 1: // TEXEL0
            out[0] = out[1] = out[2] = out[3] = sTexSubst0;
            break;
        case 2: // TEXEL1
            out[0] = out[1] = out[2] = out[3] = sTexSubst1;
            break;
        case 3:
            out[0] = sPrimColor[0];
            out[1] = sPrimColor[1];
            out[2] = sPrimColor[2];
            out[3] = sPrimColor[3];
            break;
        case 4:
            out[0] = shade[0];
            out[1] = shade[1];
            out[2] = shade[2];
            out[3] = shade[3];
            break;
        case 5:
            out[0] = sEnvColor[0];
            out[1] = sEnvColor[1];
            out[2] = sEnvColor[2];
            out[3] = sEnvColor[3];
            break;
        case 6:
            out[0] = out[1] = out[2] = out[3] = 1.0f;
            break;
        default:
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            break;
    }
}

// Lookup table for 5-bit inputs (c only - has more options)
static inline void get_cc_input_5bit(int mux, float* shade, float* combined, float* out) {
    switch (mux) {
        case 0:
            out[0] = combined[0];
            out[1] = combined[1];
            out[2] = combined[2];
            out[3] = combined[3];
            break;
        case 1: // TEXEL0
            out[0] = out[1] = out[2] = out[3] = sTexSubst0;
            break;
        case 2: // TEXEL1
            out[0] = out[1] = out[2] = out[3] = sTexSubst1;
        case 3:
            out[0] = sPrimColor[0];
            out[1] = sPrimColor[1];
            out[2] = sPrimColor[2];
            out[3] = sPrimColor[3];
            break;
        case 4:
            out[0] = shade[0];
            out[1] = shade[1];
            out[2] = shade[2];
            out[3] = shade[3];
            break;
        case 5:
            out[0] = sEnvColor[0];
            out[1] = sEnvColor[1];
            out[2] = sEnvColor[2];
            out[3] = sEnvColor[3];
            break;
        case 6:
            out[0] = out[1] = out[2] = out[3] = 1.0f;
            break;
        case 8: // TEXEL0_ALPHA
            out[0] = out[1] = out[2] = out[3] = sTexSubst0;
            break;
        case 9: // TEXEL1_ALPHA
            out[0] = out[1] = out[2] = out[3] = sTexSubst1;
            break;
        case 10:
            out[0] = out[1] = out[2] = out[3] = sPrimColor[3];
            break;
        case 11:
            out[0] = out[1] = out[2] = out[3] = shade[3];
            break;
        case 12:
            out[0] = out[1] = out[2] = out[3] = sEnvColor[3];
            break;
        case 13: // LOD_FRACTION (treat as prim_lod_frac for now)
        case 14: // PRIM_LOD_FRAC — from gDPSetPrimColor lodfrac
            out[0] = out[1] = out[2] = out[3] = sPrimLodFrac;
            break;
        default:
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            break;
    }
}

/* Alpha mux lookup (3-bit): 0=COMBINED,1=TEX0,2=TEX1,3=PRIM,4=SHADE,5=ENV,6=1,7=0 */
static inline float get_alpha_abd(int mux, float shade_a, float combined_a) {
    switch (mux) {
        case 0: return combined_a;
        case 1: return sTexSubst0;  /* TEXEL0 */
        case 2: return sTexSubst1;  /* TEXEL1 */
        case 3: return sPrimColor[3];
        case 4: return shade_a;
        case 5: return sEnvColor[3];
        case 6: return 1.0f;
        default: return 0.0f;
    }
}
/* Alpha C (multiplier): 0=LOD_FRAC,1=TEX0,2=TEX1,3=PRIM,4=SHADE,5=ENV,6=PRIM_LOD_FRAC,7=0 */
static inline float get_alpha_c(int mux, float shade_a, float combined_a) {
    switch (mux) {
        case 0: return sPrimLodFrac;   /* LOD_FRACTION */
        case 1: return sTexSubst0;  /* TEXEL0 */
        case 2: return sTexSubst1;  /* TEXEL1 */
        case 3: return sPrimColor[3];
        case 4: return shade_a;
        case 5: return sEnvColor[3];
        case 6: return sPrimLodFrac;   /* PRIM_LOD_FRAC */
        default: return 0.0f;
    }
}

static u8 not_jank(u8 index) {
    return index != 6 && index != 7 && index != 15 && index != 31;
}


static inline void get_combiner_color(float shade[4], float out[4]) {
    float colA[4], colB[4], colC[4], colD[4];
    float cycle0[4];
    float dummy[4] = { 0, 0, 0, 0 };
//if (shade == global_dummy)
   // printf("a0 %d b0 %d c0 %d d0 %d\n",sCombinerState.a0, sCombinerState.b0,sCombinerState.c0,sCombinerState.d0);

    sDrawingSkybox = (sCombinerState.c0 == G_CCMUX_PRIMITIVE_ALPHA);

/*     if ((sCombinerState.a0 == G_CCMUX_K5) && (G_CCMUX_K5) && (sCombinerState.c0 == G_CCMUX_0) &&
    (sCombinerState.a1 == G_CCMUX_K5) && (sCombinerState.b1 == G_CCMUX_K5) && (sCombinerState.c1 == G_CCMUX_0) &&
    (sCombinerState.d0 == G_CCMUX_PRIMITIVE) && (sCombinerState.d1 == G_CCMUX_PRIMITIVE)) {

            out[0] = 0;
            out[1] = 0;
            out[2] = 0;
            out[3] = sPrimColor[3];
        if (!sDoAdd) {
            sPvrColDirty = 1;
            sPvrTexDirty = 1;
            sPvrNeedHeader = 1;
        }
//        printf("doing lens flare maybe\n");
                    sDoAdd = 3;
        } else */ {
    /* Cycle 0 — color (RGB) */
    get_cc_input_4bit(sCombinerState.a0, shade, dummy, colA);
    get_cc_input_4bit(sCombinerState.b0, shade, dummy, colB);
    get_cc_input_5bit(sCombinerState.c0, shade, dummy, colC);
    get_cc_input_4bit(sCombinerState.d0, shade, dummy, colD);

#if 1
    if (sCombinerState.d1 == G_CCMUX_ENVIRONMENT) {
//        sDrawingSkybox = 0;
        if (!sDoAdd) {
            sPvrColDirty = 1;
            sPvrTexDirty = 1;
            sPvrNeedHeader = 1;
        }
        sDoAdd = 2;
    } else if (sCombinerState.d0 == G_CCMUX_ENVIRONMENT) {
  //      sDrawingSkybox = 0;
        if (!sDoAdd) {
            sPvrColDirty = 1;
            sPvrTexDirty = 1;
            sPvrNeedHeader = 1;
        }
        sDoAdd = 1;
    } /* else if (not_jank(sCombinerState.a1) && not_jank(sCombinerState.b1) && not_jank(sCombinerState.c1) && sCombinerState.d1 == G_CCMUX_PRIMITIVE) {
        sDrawingSkybox = 0;
        if (!sDoAdd) {
            sPvrColDirty = 1;
            sPvrTexDirty = 1;
            sPvrNeedHeader = 1;
        }
        sDoAdd = 6;
    } else if (not_jank(sCombinerState.a0) && not_jank(sCombinerState.b0) && not_jank(sCombinerState.c0) && sCombinerState.d0 == G_CCMUX_PRIMITIVE) {
        sDrawingSkybox = 0;
        if (!sDoAdd) {
            sPvrColDirty = 1;
            sPvrTexDirty = 1;
            sPvrNeedHeader = 1;
        }
        sDoAdd = 3;
    } */ else {
        if (sDoAdd) {
            sPvrColDirty = 1;
            sPvrTexDirty = 1;
            sPvrNeedHeader = 1;
        }
        sDoAdd = 0;
    }
#endif
    for (int i = 0; i < 3; i++) {
        cycle0[i] = (colA[i] - colB[i]) * colC[i];
        if (sDoAdd != 1 && sDoAdd != 3) {
            cycle0[i] += colD[i];
        } 
        if (cycle0[i] < 0.0f) cycle0[i] = 0.0f;
        if (cycle0[i] > 1.0f) cycle0[i] = 1.0f;
    }
    /* Cycle 0 — alpha (separate mux) */
    {
        float aA = get_alpha_abd(sCombinerState.aa0, shade[3], 0.0f);
        float aB = get_alpha_abd(sCombinerState.ab0, shade[3], 0.0f);
        float aC = get_alpha_c(sCombinerState.ac0, shade[3], 0.0f);
        float aD = get_alpha_abd(sCombinerState.ad0, shade[3], 0.0f);
        cycle0[3] = (aA - aB) * aC + aD;
        if (cycle0[3] < 0.0f) cycle0[3] = 0.0f;
        if (cycle0[3] > 1.0f) cycle0[3] = 1.0f;
    }

    /* Cycle 1 — color (RGB) */
    get_cc_input_4bit(sCombinerState.a1, shade, cycle0, colA);
    get_cc_input_4bit(sCombinerState.b1, shade, cycle0, colB);
    get_cc_input_5bit(sCombinerState.c1, shade, cycle0, colC);
    get_cc_input_4bit(sCombinerState.d1, shade, cycle0, colD);

    for (int i = 0; i < 3; i++) {
        out[i] = (colA[i] - colB[i]) * colC[i];
        if (sDoAdd != 2 && sDoAdd != 6) 
            out[i] += colD[i];
        if (out[i] < 0.0f) out[i] = 0.0f;
        if (out[i] > 1.0f) out[i] = 1.0f;
    }
    /* Cycle 1 — alpha (separate mux) */
    {
        float aA = get_alpha_abd(sCombinerState.aa1, shade[3], cycle0[3]);
        float aB = get_alpha_abd(sCombinerState.ab1, shade[3], cycle0[3]);
        float aC = get_alpha_c(sCombinerState.ac1, shade[3], cycle0[3]);
        float aD = get_alpha_abd(sCombinerState.ad1, shade[3], cycle0[3]);
        out[3] = (aA - aB) * aC + aD;
        if (out[3] < 0.0f) out[3] = 0.0f;
        if (out[3] > 1.0f) out[3] = 1.0f;
    }
}
}
static int combiner_uses_texture(void) {
    return sCombinerState.uses_texture;
}

/* Map N64 geometry-mode cull flags to PVR hardware culling.
   Fast-path screen-space cross: positive = CCW (front), negative = CW (back).
   G_CULL_BACK rejects CW  → PVR_CULLING_CW
   G_CULL_FRONT rejects CCW → PVR_CULLING_CCW */
static inline int pvr_cull_mode(void) {
    int cb = (sGeometryMode & G_CULL_BACK)  != 0;
    int cf = (sGeometryMode & G_CULL_FRONT) != 0;
    if (cb && cf) return PVR_CULLING_SMALL;   
    if (cb)       return PVR_CULLING_CW;
    if (cf)       return PVR_CULLING_CCW;
    return PVR_CULLING_NONE;
}

static int sFogMode = 0;

static void pvr_rebuild_col_header(void) {
    sPvrHdrCol.m0.list_type = sPvrCurrentList;

    sPvrHdrCol.m1.culling = pvr_cull_mode();
    sPvrHdrCol.m1.depth_cmp = (sDepthTestEnabled ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS);
    sPvrHdrCol.m1.depth_write_dis = (!sDepthWriteEnabled);

    if (sFogColChanged != 2) {
    sPvrHdrCol.m2.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd && !sDamageFlash) ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
    } else if (sFogColChanged == 2) {
    sPvrHdrCol.m2.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd) ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
    }
    sPvrHdrCol.m2.alpha = sPvrCurrentList > PVR_LIST_OP_MOD;

    if (sAlphaTestEnabled || sBlendingEnabled) {
        sPvrHdrCol.m2.blend_src = PVR_BLEND_SRCALPHA;
        sPvrHdrCol.m2.blend_dst = PVR_BLEND_INVSRCALPHA;
    } else {
        sPvrHdrCol.m2.blend_src = PVR_BLEND_ONE;
        sPvrHdrCol.m2.blend_dst = PVR_BLEND_ZERO;
    }
}

static void pvr_rebuild_tex_header(TextureCacheEntry* entry) {
    __builtin_prefetch(&sPvrHdrTex);

    int tw = entry->pow2Info.padded_w;
    int th = entry->pow2Info.padded_h;
    u32 pfmt = pvr_tex_fmt_for(entry->fmt, entry->siz);
    /* Texture wrapping: translate N64 cms/cmt to PVR uv_clamp/uv_flip */
    u32 cms = sTiles[0].cms;
    u32 cmt = sTiles[0].cmt;

    sPvrHdrTex.m0.list_type = sPvrCurrentList;
    /* The offset color (oargb) is shared by the sDoAdd offset color and vertex
       fog; enable it only when one of those is actually in use, else the PVR
       adds a stale oargb and washes the surface. (Raw builder otherwise inherits
       the template's always-on specular bit.) */
    sPvrHdrTex.m0.oargb_en = (sDoAdd || (sGeometryMode & G_FOG)) ? 1 : 0;

    u32 mode1 = sPvrHdrTex.mode1 & (~(PVR_TA_PM1_CULLING_MASK | PVR_TA_PM1_DEPTHCMP_MASK | PVR_TA_PM1_DEPTHWRITE_MASK));
    mode1 |= (pvr_cull_mode() << PVR_TA_PM1_CULLING_SHIFT);
    mode1 |= ((sDepthTestEnabled ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS) << PVR_TA_PM1_DEPTHCMP_SHIFT);
    mode1 |= ((!sDepthWriteEnabled )? (PVR_DEPTHWRITE_DISABLE << PVR_TA_PM1_DEPTHWRITE_SHIFT) : 0);
    sPvrHdrTex.mode1 = mode1;

    u32 mode2 = sPvrHdrTex.mode2 & ( 
        ~(PVR_TA_PM2_TXRALPHA_MASK | PVR_TA_PM2_USIZE_MASK | 
            PVR_TA_PM2_VSIZE_MASK | PVR_TA_PM2_UVCLAMP_MASK | 
            PVR_TA_PM2_UVFLIP_MASK | PVR_TA_PM2_FOG_MASK |
            PVR_TA_PM2_FILTER_MASK | PVR_TA_PM2_ALPHA_MASK | 
        PVR_TA_PM2_SRCBLEND_MASK | PVR_TA_PM2_DSTBLEND_MASK
    ));

    mode2 |= ((__builtin_ctz(tw) - 3) << PVR_TA_PM2_USIZE_SHIFT);
    mode2 |= ((__builtin_ctz(th) - 3) << PVR_TA_PM2_VSIZE_SHIFT);
    mode2 |= ((((!!(cms & 2)) << 1) | (!!(cmt & 2))) << PVR_TA_PM2_UVCLAMP_SHIFT);
    mode2 |= ((((!!(cms & 1)) << 1) | (!!(cmt & 1))) << PVR_TA_PM2_UVFLIP_SHIFT);
    mode2 |= ((pvr_tex_filter()) << PVR_TA_PM2_FILTER_SHIFT);
//    mode2 |= ((((sGeometryMode & G_FOG) && !sDoAdd && !sDamageFlash) ? PVR_FOG_VERTEX : PVR_FOG_DISABLE) << PVR_TA_PM2_FOG_SHIFT);
    if (sFogColChanged != 2) {
mode2 |= ((((sGeometryMode & G_FOG) && !sDoAdd && !sDamageFlash) ? PVR_FOG_VERTEX : PVR_FOG_DISABLE) << PVR_TA_PM2_FOG_SHIFT);    
} else if (sFogColChanged == 2) {
    printf("some table fog snuck in this frame\n");
mode2 |= ((((sGeometryMode & G_FOG) && !sDoAdd) ? PVR_FOG_TABLE : PVR_FOG_DISABLE) << PVR_TA_PM2_FOG_SHIFT);
    }

mode2 |= ((sPvrCurrentList > PVR_LIST_OP_MOD) << PVR_TA_PM2_ALPHA_SHIFT);
    if (sAlphaTestEnabled || sBlendingEnabled) {
        mode2 |= (PVR_BLEND_SRCALPHA << PVR_TA_PM2_SRCBLEND_SHIFT);
        /* Steam puff: additive (SRCALPHA/ONE) instead of SRCALPHA/INVSRCALPHA.
           The single I4 texture has black RGB in its transparent texels, so the
           normal over-blend + bilinear filtering bleeds a dark gray fringe (worst
           along the bottom edge). Additive makes dark/transparent texels add ~zero
           → no fringe, and reads brighter. SRCALPHA (not ONE) keeps the per-effect
           alpha fade working. */
        u32 dstBlend = PVR_BLEND_INVSRCALPHA;
        if (pc_resolve_addr(sTiles[0].texAddr) == pc_resolve_addr(gFreezardSteamTex))
            dstBlend = PVR_BLEND_ONE;
        mode2 |= (dstBlend << PVR_TA_PM2_DSTBLEND_SHIFT);
    } else {
        mode2 |= (PVR_BLEND_ONE << PVR_TA_PM2_SRCBLEND_SHIFT);
        mode2 |= (PVR_BLEND_ZERO << PVR_TA_PM2_DSTBLEND_SHIFT);
        mode2 |= (PVR_TXRALPHA_DISABLE << PVR_TA_PM2_TXRALPHA_SHIFT);
    }
    sPvrHdrTex.mode2 = mode2;

    sPvrHdrTex.m3.pixel_mode = ((pfmt == PVR_TXRFMT_ARGB1555) ? PVR_PIXEL_MODE_ARGB1555 : PVR_PIXEL_MODE_ARGB4444);
    sPvrHdrTex.m3.txr_base = to_pvr_txr_ptr(entry->id);

    sPvrHdrTexId = entry->id;
}

/* ---- Multi-texture flush: 3-pass dest-alpha stencil ----
   Pass A (alpha clear): DESTCOLOR/ZERO with white+alpha=0 — clears FB alpha
           in the triangle region while preserving existing color.
   Pass B (mask):        Tile 0 with SRCALPHA/INVSRCALPHA — renders mask shape,
           establishes FB alpha from tile 0's alpha.
   Pass C (fire):        Tile 1 with DESTALPHA/ONE — fire added only where
           FB alpha is non-zero (inside the mask shape). */
static void flush_multitex_pass2(void) {
    if (sMultiTexVertCount == 0) return;

    /* Finalize last batch count */
    if (sMultiTexBatchCount > 0) {
        MtxBatch* last = &sMultiTexBatches[sMultiTexBatchCount - 1];
        last->count = sMultiTexVertCount - last->start;
    }

    for (int b = 0; b < sMultiTexBatchCount; b++) {
        MtxBatch* batch = &sMultiTexBatches[b];
        if (batch->count < 3) continue;

        /* Non-stencil path: simple lerp (ground detail, skybox, water) */
        if (!batch->needs_stencil) {
            u32 bfmt = pvr_tex_fmt_for(batch->texID->fmt, batch->texID->siz);
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                              bfmt,// | PVR_TXRFMT_NONTWIDDLED,
                              batch->padW, batch->padH,
                              batch->texID->id, pvr_tex_filter());
            cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
            { int uvc = 0, uvf = 0;
              if (batch->cms & 2) uvc |= PVR_UVCLAMP_U;
              if (batch->cmt & 2) uvc |= PVR_UVCLAMP_V;
              if (batch->cms & 1) uvf |= PVR_UVFLIP_U;
              if (batch->cmt & 1) uvf |= PVR_UVFLIP_V;
              cxt.txr.uv_clamp = uvc; cxt.txr.uv_flip = uvf; }
            cxt.gen.culling = PVR_CULLING_NONE;
            cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
            cxt.gen.specular = 1;
            cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
            cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
            cxt.blend.src = PVR_BLEND_SRCALPHA;
            cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
            cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
            pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
            pvr_poly_compile(&hdr, &cxt);
            SHZ_PREFETCH(&hdr);
            shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
            for (int i = batch->start; i < batch->start + batch->count; i += 3) {
                for (int j = 0; j < 3 && (i + j) < batch->start + batch->count; j++) {
                    MtxVert* mv = &sMultiTexVerts[i + j];
                    pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
                    vert->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                    vert->x = mv->x; vert->y = mv->y; vert->z = mv->z;
                    if (mv->z > max_z) max_z = mv->z;
                    vert->u = mv->u; vert->v = mv->v;
                    vert->argb = mv->argb;
                    vert->oargb = mv->oargb;
                    pvr_dr_commit(vert);
                }
            }
            continue;
        }

        /* --- Stencil path for masked effects (fire/torch) --- */

        /* --- Pass A: Alpha clear — DESTCOLOR/ZERO with white+alpha=0 --- */
        {
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
            cxt.gen.culling = PVR_CULLING_NONE;
            cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
            cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
            cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
            cxt.blend.src = PVR_BLEND_DESTCOLOR;
            cxt.blend.dst = PVR_BLEND_ZERO;
            pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
            pvr_poly_compile(&hdr, &cxt);
            SHZ_PREFETCH(&hdr);
            shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
            for (int i = batch->start; i < batch->start + batch->count; i += 3) {
                for (int j = 0; j < 3 && (i + j) < batch->start + batch->count; j++) {
                    MtxVert* mv = &sMultiTexVerts[i + j];
                    pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
                    vert->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                    vert->x = mv->x; vert->y = mv->y; vert->z = mv->z;
                    vert->u = 0; vert->v = 0;
                    vert->argb = 0x00FFFFFF; /* white color, zero alpha */
                    vert->oargb = 0;
                    pvr_dr_commit(vert);
                }
            }
        }

        /* --- Pass B: Mask (tile 0) — SRCALPHA/INVSRCALPHA --- */
        if (batch->tex0) {
            u32 fmt0 = pvr_tex_fmt_for(batch->tex0->fmt, batch->tex0->siz);
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                              fmt0,// | PVR_TXRFMT_NONTWIDDLED,
                              batch->pad0W, batch->pad0H,
                              batch->tex0->id, pvr_tex_filter());
            cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
            { int uvc = 0, uvf = 0;
              if (batch->cms0 & 2) uvc |= PVR_UVCLAMP_U;
              if (batch->cmt0 & 2) uvc |= PVR_UVCLAMP_V;
              if (batch->cms0 & 1) uvf |= PVR_UVFLIP_U;
              if (batch->cmt0 & 1) uvf |= PVR_UVFLIP_V;
              cxt.txr.uv_clamp = uvc; cxt.txr.uv_flip = uvf; }
            cxt.gen.culling = PVR_CULLING_NONE;
            cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
            cxt.gen.specular = 1;
            cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
            cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
            cxt.blend.src = PVR_BLEND_SRCALPHA;
            cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
            cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
            pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
            pvr_poly_compile(&hdr, &cxt);
            SHZ_PREFETCH(&hdr);
            shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
            for (int i = batch->start; i < batch->start + batch->count; i += 3) {
                for (int j = 0; j < 3 && (i + j) < batch->start + batch->count; j++) {
                    MtxVert* mv = &sMultiTexVerts[i + j];
                    pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
                    vert->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                    vert->x = mv->x; vert->y = mv->y; vert->z = mv->z;
                    vert->u = mv->u0; vert->v = mv->v0;
                    vert->argb = mv->argb;
                    vert->oargb = mv->oargb;
                    pvr_dr_commit(vert);
                }
            }
        }

        /* --- Pass C: Fire (tile 1) — DESTALPHA/ONE (masked by FB alpha) --- */
        {
            u32 bfmt = pvr_tex_fmt_for(batch->texID->fmt, batch->texID->siz);
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                              bfmt,// | PVR_TXRFMT_NONTWIDDLED,
                              batch->padW, batch->padH,
                              batch->texID->id, pvr_tex_filter());
            cxt.txr.env = PVR_TXRENV_MODULATE;//ALPHA;
            { int uvc = 0, uvf = 0;
              if (batch->cms & 2) uvc |= PVR_UVCLAMP_U;
              if (batch->cmt & 2) uvc |= PVR_UVCLAMP_V;
              if (batch->cms & 1) uvf |= PVR_UVFLIP_U;
              if (batch->cmt & 1) uvf |= PVR_UVFLIP_V;
              cxt.txr.uv_clamp = uvc; cxt.txr.uv_flip = uvf; }
            cxt.gen.culling = PVR_CULLING_NONE;
            cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
            cxt.gen.specular = 1;
            cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
            cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
            cxt.blend.src = PVR_BLEND_DESTALPHA;
            cxt.blend.dst = PVR_BLEND_ONE;
            cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
            pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
            pvr_poly_compile(&hdr, &cxt);
            SHZ_PREFETCH(&hdr);
            shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
            for (int i = batch->start; i < batch->start + batch->count; i += 3) {
                for (int j = 0; j < 3 && (i + j) < batch->start + batch->count; j++) {
                    MtxVert* mv = &sMultiTexVerts[i + j];
                    pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
                    vert->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                    vert->x = mv->x; vert->y = mv->y; vert->z = mv->z;
                    if (mv->z > max_z) max_z = mv->z;
                    vert->u = mv->u; vert->v = mv->v;
                    vert->argb = mv->argb;
                    vert->oargb = mv->oargb;
                    pvr_dr_commit(vert);
                }
            }
        }
    }

    sMultiTexVertCount  = 0;
    sMultiTexBatchCount = 0;
}

#if 0
void pvr_ensure_mtx_hdr_nostencil(MtxBatch* batch, pvr_poly_hdr_t* hdr) {
    u32 bfmt = pvr_tex_fmt_for(batch->texID->fmt, batch->texID->siz);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                      bfmt | PVR_TXRFMT_NONTWIDDLED,
                      batch->padW, batch->padH,
                      batch->texID->id, pvr_tex_filter());
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    {
        int uvc = 0, uvf = 0;
        if (batch->cms & 2) uvc |= PVR_UVCLAMP_U;
        if (batch->cmt & 2) uvc |= PVR_UVCLAMP_V;
        if (batch->cms & 1) uvf |= PVR_UVFLIP_U;
        if (batch->cmt & 1) uvf |= PVR_UVFLIP_V;
        cxt.txr.uv_clamp = uvc; cxt.txr.uv_flip = uvf; 
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    cxt.gen.specular = 1;
    // TODO fog
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    pvr_poly_compile(&hdr, &cxt);
    SHZ_PREFETCH(&hdr);
}

void pvr_ensure_mtx_hdr_stencil_pass_a(MtxBatch* batch, pvr_poly_hdr_t* hdr) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_DESTCOLOR;
    cxt.blend.dst = PVR_BLEND_ZERO;
    pvr_poly_compile(&hdr, &cxt);
    SHZ_PREFETCH(&hdr);
}

void pvr_ensure_mtx_hdr_stencil_pass_b(MtxBatch* batch, pvr_poly_hdr_t* hdr) {
    u32 fmt0 = pvr_tex_fmt_for(batch->tex0->fmt, batch->tex0->siz);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, fmt0 | PVR_TXRFMT_NONTWIDDLED, batch->pad0W, batch->pad0H, batch->tex0->id,
                     pvr_tex_filter());
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    {
        int uvc = 0, uvf = 0;
        if (batch->cms0 & 2)
            uvc |= PVR_UVCLAMP_U;
        if (batch->cmt0 & 2)
            uvc |= PVR_UVCLAMP_V;
        if (batch->cms0 & 1)
            uvf |= PVR_UVFLIP_U;
        if (batch->cmt0 & 1)
            uvf |= PVR_UVFLIP_V;
        cxt.txr.uv_clamp = uvc;
        cxt.txr.uv_flip = uvf;
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    cxt.gen.specular = 1;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    pvr_poly_compile(&hdr, &cxt);
    SHZ_PREFETCH(&hdr);
}

void pvr_ensure_mtx_hdr_stencil_pass_c(MtxBatch* batch, pvr_poly_hdr_t* hdr) {
    u32 bfmt = pvr_tex_fmt_for(batch->texID->fmt, batch->texID->siz);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, bfmt | PVR_TXRFMT_NONTWIDDLED, batch->padW, batch->padH, batch->texID->id,
                     pvr_tex_filter());
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    {
        int uvc = 0, uvf = 0;
        if (batch->cms & 2)
            uvc |= PVR_UVCLAMP_U;
        if (batch->cmt & 2)
            uvc |= PVR_UVCLAMP_V;
        if (batch->cms & 1)
            uvf |= PVR_UVFLIP_U;
        if (batch->cmt & 1)
            uvf |= PVR_UVFLIP_V;
        cxt.txr.uv_clamp = uvc;
        cxt.txr.uv_flip = uvf;
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    cxt.gen.specular = 1;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_DESTALPHA;
    cxt.blend.dst = PVR_BLEND_ONE;
    cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    pvr_poly_compile(&hdr, &cxt);
    SHZ_PREFETCH(&hdr);
}

void mtx_vertex_loop_step_a(MtxBatch* batch) {
    for (int i = batch->start; i < batch->start + batch->count; i += 3) {
        for (int j = 0; j < 3 && (i + j) < batch->start + batch->count; j++) {
            MtxVert* mv = &sMultiTexVerts[i + j];
            pvr_vertex_t* vert = (pvr_vertex_t*)pvr_dr_target(sDrState);
            vert->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            vert->x = mv->x;
            vert->y = mv->y;
            vert->z = mv->z;
            vert->u = 0;
            vert->v = 0;
            vert->argb = 0x00FFFFFF; /* white color, zero alpha */
            vert->oargb = 0;
            pvr_dr_commit(vert);
        }
    }
}

void mtx_vertex_loop_vtxcoloruv(MtxBatch* batch, int tile) {
    for (int i = batch->start; i < batch->start + batch->count; i += 3) {
        for (int j = 0; j < 3 && (i + j) < batch->start + batch->count; j++) {
            MtxVert* mv = &sMultiTexVerts[i + j];
            pvr_vertex_t* vert = (pvr_vertex_t*)pvr_dr_target(sDrState);
            vert->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            vert->x = mv->x;
            vert->y = mv->y;
            vert->z = mv->z;
            if (tile == 1) {
            vert->u = mv->u;
            vert->v = mv->v;
            } else {
            vert->u = mv->u0;
            vert->v = mv->v0;
            }
            vert->argb = mv->argb;
            vert->oargb = mv->oargb;
            pvr_dr_commit(vert);
        }
    }
}

/* ---- Multi-texture flush: 3-pass dest-alpha stencil ----
   Pass A (alpha clear): DESTCOLOR/ZERO with white+alpha=0 — clears FB alpha
           in the triangle region while preserving existing color.
   Pass B (mask):        Tile 0 with SRCALPHA/INVSRCALPHA — renders mask shape,
           establishes FB alpha from tile 0's alpha.
   Pass C (fire):        Tile 1 with DESTALPHA/ONE — fire added only where
           FB alpha is non-zero (inside the mask shape). */
static void flush_multitex_pass2(void) {
    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;

{
    sMultiTexVertCount  = 0;
    sMultiTexBatchCount = 0;
    return;
}
    if (sMultiTexVertCount == 0) return;

    /* Finalize last batch count */
    if (sMultiTexBatchCount > 0) {
        MtxBatch* last = &sMultiTexBatches[sMultiTexBatchCount - 1];
        last->count = sMultiTexVertCount - last->start;
    }

    for (int b = 0; b < sMultiTexBatchCount; b++) {
        MtxBatch* batch = &sMultiTexBatches[b];
        if (batch->count < 3) continue;

        /* Non-stencil path: simple lerp (ground detail, skybox, water) */
        if (!batch->needs_stencil) {
            pvr_ensure_mtx_hdr_nostencil(batch, &hdr);
            shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
            mtx_vertex_loop_vtxcoloruv(batch, 1);
            continue;
        }

        /* --- Stencil path for masked effects (fire/torch) --- */

        /* --- Pass A: Alpha clear — DESTCOLOR/ZERO with white+alpha=0 --- */
        {
            pvr_ensure_mtx_hdr_stencil_pass_a(batch, &hdr);
            shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
            mtx_vertex_loop_step_a(batch);
        }

        /* --- Pass B: Mask (tile 0) — SRCALPHA/INVSRCALPHA --- */
        if (batch->tex0) {
            pvr_ensure_mtx_hdr_stencil_pass_b(batch, &hdr);
            shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
            mtx_vertex_loop_vtxcoloruv(batch, 0);
        }

        /* --- Pass C: Fire (tile 1) — DESTALPHA/ONE (masked by FB alpha) --- */
        {
            pvr_ensure_mtx_hdr_stencil_pass_c(batch, &hdr);
            shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
            mtx_vertex_loop_vtxcoloruv(batch, 1);
        }
    }

    sMultiTexVertCount  = 0;
    sMultiTexBatchCount = 0;
}
#endif
#if 0
/* ---- Multi-texture pass 2: flush buffered triangles onto TR list ---- */
static void flush_multitex_pass2(void) {
    if (sMultiTexVertCount == 0) return;

    /* Finalize last batch count */
    if (sMultiTexBatchCount > 0) {
        MtxBatch* last = &sMultiTexBatches[sMultiTexBatchCount - 1];
        last->count = sMultiTexVertCount - last->start;
    }

    for (int b = 0; b < sMultiTexBatchCount; b++) {
        MtxBatch* batch = &sMultiTexBatches[b];
        if (batch->count < 3) continue;

        u32 bfmt = pvr_tex_fmt_for(batch->texID->fmt, batch->texID->siz);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                          bfmt | PVR_TXRFMT_NONTWIDDLED,
                          batch->padW, batch->padH,
                          batch->texID->id, pvr_tex_filter());
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;

        int uv_clamp = 0, uv_flip = 0;
        if (batch->cms & 2) uv_clamp |= PVR_UVCLAMP_U;
        if (batch->cmt & 2) uv_clamp |= PVR_UVCLAMP_V;
        if (batch->cms & 1) uv_flip  |= PVR_UVFLIP_U;
        if (batch->cmt & 1) uv_flip  |= PVR_UVFLIP_V;
        cxt.txr.uv_clamp = uv_clamp;
        cxt.txr.uv_flip  = uv_flip;

        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;

        cxt.gen.specular = 1;
        if (sFogChanged != 2) {
        cxt.gen.fog_type = (!sDoAdd & (sGeometryMode & G_FOG)) ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
        } else {
            cxt.gen.fog_type = (!sDoAdd & (sGeometryMode & G_FOG)) ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
        }
        pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
        pvr_poly_compile(&hdr, &cxt);
        SHZ_PREFETCH(&hdr);
        shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);

        for (int i = batch->start; i < batch->start + batch->count; i += 3) {
            for (int j = 0; j < 3 && (i + j) < batch->start + batch->count; j++) {
                 MtxVert* mv = &sMultiTexVerts[i + j];
                pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
                vert->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                vert->x = mv->x;
                vert->y = mv->y;
                vert->z = mv->z;
                if (mv->z > max_z) max_z = mv->z;
                vert->u = mv->u;
                vert->v = mv->v;
                vert->argb = mv->argb;
                vert->oargb = mv->oargb;
                pvr_dr_commit(vert);
            }
        }
    }

    sMultiTexVertCount  = 0;
    sMultiTexBatchCount = 0;
}
#endif

/* Start or continue a multi-tex batch for the given tile 1 texture */
static void mtx_ensure_batch(void) {
    TextureCacheEntry* entry = sTiles[1].texture;
    u32 cms = sTiles[1].cms;
    u32 cmt = sTiles[1].cmt;

    if (!entry) {
        return;
    }

    if (sMultiTexBatchCount > 0) {
        MtxBatch* cur = &sMultiTexBatches[sMultiTexBatchCount - 1];
        if (cur->texID->id == entry->id && cur->cms == cms && cur->cmt == cmt)
            return;  /* current batch still matches */
        cur->count = sMultiTexVertCount - cur->start;
    }
    if (sMultiTexBatchCount >= MAX_MTX_BATCHES) return;

    MtxBatch* b = &sMultiTexBatches[sMultiTexBatchCount++];
    b->start = sMultiTexVertCount;
    b->count = 0;
    b->texID = entry;
    b->padW  = entry->pow2Info.padded_w;
    b->padH  = entry->pow2Info.padded_h;
    b->cms   = cms;
    b->cmt   = cmt;
    /* Capture tile 0 (mask/base) for stencil passes */
    b->tex0  = sTiles[0].texture;
    b->pad0W = b->tex0 ? b->tex0->pow2Info.padded_w : 8;
    b->pad0H = b->tex0 ? b->tex0->pow2Info.padded_h : 8;
    b->cms0  = sTiles[0].cms;
    b->cmt0  = sTiles[0].cmt;
    /* Only use stencil masking for the decorative flame-mask effect (Poe souls,
       Freezard steam, etc.), keyed on the specific tile-0 texture. Everything else
       — Morpha's water, ground detail, skybox — must use the simple lerp, or the
       additive Pass C blows them out. */
    b->needs_stencil =  (sCombinerState.alpha_uses_texture && sBlendingEnabled) ||
                        (b->tex0 &&
                        pc_resolve_addr(b->tex0->addr) == pc_resolve_addr(gDecorativeFlameMaskTex));
}

/* One-way switch from OP to TR list mid-scene (for blended/alpha-tested geometry) */
static void __attribute__((unused)) pvr_ensure_tr_list(void) {
    if (sPvrOnTrList) return;
    flush_triangles();
    pvr_list_finish();                       /* finish OP */
    pvr_list_begin(PVR_LIST_TR_POLY);
    flush_multitex_pass2();                  /* pass 2 goes first on TR */
    pvr_dr_init(&sDrState);
    sPvrCurrentList = PVR_LIST_TR_POLY;
    sPvrOnTrList = 1;
    sPvrColDirty = 1;
    sPvrTexDirty = 1;
    sPvrNeedHeader = 1;
    pvr_submit_user_clip(sPvrScisX1, sPvrScisY1, sPvrScisX2, sPvrScisY2);
}

#if 0
/* One-way switch to PT list mid-scene (for alpha-tested geometry) */
static void pvr_ensure_pt_list(void) {
    if (sPvrOnPtList) return;
    flush_triangles();
    pvr_list_finish();                       /* finish current (OP or TR) */
    if (!sPvrOnTrList) {
        pvr_list_begin(PVR_LIST_TR_POLY);
        pvr_list_finish();                   /* empty TR */
        sPvrOnTrList = 1;
    }
    pvr_list_begin(PVR_LIST_PT_POLY);
    pvr_dr_init(&sDrState);
    sPvrCurrentList = PVR_LIST_PT_POLY;
    sPvrOnPtList = 1;
    sPvrColDirty = 1;
    sPvrTexDirty = 1;
    sPvrNeedHeader = 1;
    pvr_submit_user_clip(sPvrScisX1, sPvrScisY1, sPvrScisX2, sPvrScisY2);
}
#endif

/* Start a new PT batch with the current render state compiled for PT list.
   Called when sPtBuffering is active and header state changes. */
static void pt_start_batch(void) {
    /* Finalize previous batch */
    if (sPtBatchCount > 0) {
        PtBatch* prev = &sPtBatches[sPtBatchCount - 1];
        if (prev->count == 0)
            prev->count = sPtVertCount - prev->start;
    }
    if (sPtBatchCount >= MAX_PT_BATCHES) return;

    PtBatch* b = &sPtBatches[sPtBatchCount++];
    b->start = sPtVertCount;
    b->count = 0;

    /* Compile header for PT list using current texture/color state */
    if ((sCurrentboundTexture != NULL) && (sCurrentboundTexture->id != 0)) {
        TexHandle texID = sCurrentboundTexture->id;
        int tw = sCurrentboundTexture->pow2Info.padded_w;
        int th = sCurrentboundTexture->pow2Info.padded_h;
        u32 pfmt = pvr_tex_fmt_for(sCurrentboundTexture->fmt, sCurrentboundTexture->siz);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY,
                          pfmt,// | PVR_TXRFMT_NONTWIDDLED,
                          tw, th, texID, pvr_tex_filter());
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        u32 cms = sTiles[0].cms, cmt = sTiles[0].cmt;
        int uv_clamp = 0, uv_flip = 0;
        if (cms & 2) uv_clamp |= PVR_UVCLAMP_U;
        if (cmt & 2) uv_clamp |= PVR_UVCLAMP_V;
        if (cms & 1) uv_flip  |= PVR_UVFLIP_U;
        if (cmt & 1) uv_flip  |= PVR_UVFLIP_V;
        cxt.txr.uv_clamp = uv_clamp;
        cxt.txr.uv_flip  = uv_flip;
        cxt.gen.specular = 1;
//        if(sFogChanged) {
            cxt.gen.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd && !sDamageFlash) ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
//        }

        cxt.gen.culling = pvr_cull_mode();
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = sDepthTestEnabled ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = sDepthWriteEnabled ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
        pvr_poly_compile(&b->hdr, &cxt);
    } else {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_PT_POLY);
        cxt.gen.specular = 1;
//if(sFogChanged != 2) {
        cxt.gen.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd && !sDamageFlash) ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
//} else {
//        cxt.gen.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd) ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
//}
        cxt.gen.culling = pvr_cull_mode();
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = sDepthTestEnabled ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = sDepthWriteEnabled ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&b->hdr, &cxt);
    }
}

/* Flush buffered PT geometry onto PVR_LIST_PT_POLY.
   Called at end of frame after TR list is finished. */
static void flush_pt_buffer(void) {
    if (sPtVertCount == 0) return;

    /* Finalize last batch count */
    if (sPtBatchCount > 0) {
        PtBatch* last = &sPtBatches[sPtBatchCount - 1];
        if (last->count == 0)
            last->count = sPtVertCount - last->start;
    }

    for (int b = 0; b < sPtBatchCount; b++) {
        PtBatch* batch = &sPtBatches[b];
        if (batch->count < 3) continue;

        /* Submit the pre-compiled header */
        SHZ_PREFETCH(&batch->hdr);
        shz_sq_memcpy32_1(pvr_dr_target(sDrState), &batch->hdr);

        for (int i = batch->start; i < batch->start + batch->count; i += 3) {
            for (int j = 0; j < 3 && (i + j) < batch->start + batch->count; j++) {
                MtxVert* mv = &sPtVerts[i + j];
                pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
                vert->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                vert->x = mv->x;
                vert->y = mv->y;
                vert->z = mv->z;
                if (mv->z > max_z) max_z = mv->z;
                vert->u = mv->u;
                vert->v = mv->v;
                vert->argb = mv->argb;
                vert->oargb = mv->oargb;
                pvr_dr_commit(vert);
            }
        }
    }

    sPtVertCount  = 0;
    sPtBatchCount = 0;
}

/* Start a new TR batch with the current render state compiled for TR list.
   Called when sTrBuffering is active and header state changes. Clone of
   pt_start_batch; PVR_LIST_TR_POLY defaults blend to SRCALPHA/INVSRCALPHA. */
static void tr_start_batch(void) {
    if (sTrBatchCount > 0) {
        TrBatch* prev = &sTrBatches[sTrBatchCount - 1];
        if (prev->count == 0)
            prev->count = sTrVertCount - prev->start;
    }
    if (sTrBatchCount >= MAX_TR_BATCHES) return;

    TrBatch* b = &sTrBatches[sTrBatchCount++];
    b->start = sTrVertCount;
    b->count = 0;
    b->is_mod = 0;

    if (sLensBracket) {
        /* Lens bracket: compile a modifier-affected header; param set 2
           (inside the TR_MOD circle volume) is written at flush time. */
        pvr_poly_cxt_t cxt;
        if ((sCurrentboundTexture != NULL) && (sCurrentboundTexture->id != 0)) {
            TexHandle texID = sCurrentboundTexture->id;
            int tw = sCurrentboundTexture->pow2Info.padded_w;
            int th = sCurrentboundTexture->pow2Info.padded_h;
            u32 pfmt = pvr_tex_fmt_for(sCurrentboundTexture->fmt, sCurrentboundTexture->siz);
            pvr_poly_cxt_txr_mod(&cxt, PVR_LIST_TR_POLY,
                                 pfmt, tw, th, texID, pvr_tex_filter(),
                                 pfmt, tw, th, texID, pvr_tex_filter());
            cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
            cxt.txr2.env = PVR_TXRENV_MODULATEALPHA;
            {
                u32 cms = sTiles[0].cms, cmt = sTiles[0].cmt;
                int uvc = 0, uvf = 0;
                if (cms & 2) uvc |= PVR_UVCLAMP_U;
                if (cmt & 2) uvc |= PVR_UVCLAMP_V;
                if (cms & 1) uvf |= PVR_UVFLIP_U;
                if (cmt & 1) uvf |= PVR_UVFLIP_V;
                cxt.txr.uv_clamp = uvc;  cxt.txr.uv_flip = uvf;
                cxt.txr2.uv_clamp = uvc; cxt.txr2.uv_flip = uvf;
            }
            cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
            cxt.txr2.alpha = PVR_TXRALPHA_ENABLE;
            b->is_mod = 1;
        } else {
            pvr_poly_cxt_col_mod(&cxt, PVR_LIST_TR_POLY);
            b->is_mod = 2;
        }
        cxt.gen.specular = 1;
        cxt.gen.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd && !sDamageFlash) ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
        cxt.gen.culling = pvr_cull_mode();
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = sDepthTestEnabled ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = sDepthWriteEnabled ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
        cxt.blend.src = PVR_BLEND_SRCALPHA;   cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.blend.src2 = PVR_BLEND_SRCALPHA;  cxt.blend.dst2 = PVR_BLEND_INVSRCALPHA;
        pvr_poly_mod_compile(&b->hdr, &cxt);
        sLensVolNeeded = 1;
        return;
    }

    if ((sCurrentboundTexture != NULL) && (sCurrentboundTexture->id != 0)) {
        TexHandle texID = sCurrentboundTexture->id;
        int tw = sCurrentboundTexture->pow2Info.padded_w;
        int th = sCurrentboundTexture->pow2Info.padded_h;
        u32 pfmt = pvr_tex_fmt_for(sCurrentboundTexture->fmt, sCurrentboundTexture->siz);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                          pfmt,// | PVR_TXRFMT_NONTWIDDLED,
                          tw, th, texID, pvr_tex_filter());
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        u32 cms = sTiles[0].cms, cmt = sTiles[0].cmt;
        int uv_clamp = 0, uv_flip = 0;
        if (cms & 2) uv_clamp |= PVR_UVCLAMP_U;
        if (cmt & 2) uv_clamp |= PVR_UVCLAMP_V;
        if (cms & 1) uv_flip  |= PVR_UVFLIP_U;
        if (cmt & 1) uv_flip  |= PVR_UVFLIP_V;
        cxt.txr.uv_clamp = uv_clamp;
        cxt.txr.uv_flip  = uv_flip;
        cxt.gen.specular = 1;
//        if (sFogChanged != 2) {
            cxt.gen.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd && !sDamageFlash) ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
//        } else {
//            cxt.gen.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd) ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
//        }
        cxt.gen.culling = pvr_cull_mode();
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = sDepthTestEnabled ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = sDepthWriteEnabled ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
        pvr_poly_compile(&b->hdr, &cxt);
    } else {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        cxt.gen.specular = 1;
//        if (sFogChanged != 2) {
            cxt.gen.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd && !sDamageFlash) ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
//        } else {
//            cxt.gen.fog_type = ((sGeometryMode & G_FOG) && !sDoAdd) ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
//        }
        cxt.gen.culling = pvr_cull_mode();
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = sDepthTestEnabled ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = sDepthWriteEnabled ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&b->hdr, &cxt);
    }
}

/* Flush buffered TR geometry onto PVR_LIST_TR_POLY. Called at end of frame
   after OP is finished and the TR list is open. */
static void flush_deferred_tr(void) {
    if (sTrVertCount == 0) return;

    if (sTrBatchCount > 0) {
        TrBatch* last = &sTrBatches[sTrBatchCount - 1];
        if (last->count == 0)
            last->count = sTrVertCount - last->start;
    }

    for (int b = 0; b < sTrBatchCount; b++) {
        TrBatch* batch = &sTrBatches[b];
        if (batch->count < 3) continue;

        SHZ_PREFETCH(&batch->hdr);
        shz_sq_memcpy32_1(pvr_dr_target(sDrState), &batch->hdr);

        for (int i = batch->start; i < batch->start + batch->count; i += 3) {
            for (int j = 0; j < 3 && (i + j) < batch->start + batch->count; j++) {
                MtxVert* mv = &sTrVerts[i + j];
                if (batch->is_mod) {
                    /* Two-parameter (modifier-affected) vertex. Param 1 = outside
                       the circle volume, param 2 = inside. SHOW rooms are visible
                       inside; HIDE rooms are visible outside. */
                    u32 a_vis = mv->argb;
                    u32 a_hid = mv->argb & 0x00FFFFFFu;
                    u32 argb_out = sLensShow ? a_hid : a_vis;
                    u32 argb_in  = sLensShow ? a_vis : a_hid;
                    if (batch->is_mod == 1) {
                        pvr_vertex_tpcm_t *vt = (pvr_vertex_tpcm_t *)pvr_dr_target(sDrState);
                        vt->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                        vt->x = mv->x; vt->y = mv->y; vt->z = mv->z;
                        if (mv->z > max_z) max_z = mv->z;
                        vt->u0 = mv->u; vt->v0 = mv->v;
                        vt->argb0 = argb_out; vt->oargb0 = mv->oargb;
                        pvr_dr_commit(vt);
                        uint32_t *w = (uint32_t *)pvr_dr_target(sDrState);
                        ((float *)w)[0] = mv->u;   /* u1 */
                        ((float *)w)[1] = mv->v;   /* v1 */
                        w[2] = argb_in;            /* argb1 */
                        w[3] = mv->oargb;          /* oargb1 */
                        w[4] = w[5] = w[6] = w[7] = 0;
                        pvr_dr_commit(w);
                    } else {
                        pvr_vertex_pcm_t *vc = (pvr_vertex_pcm_t *)pvr_dr_target(sDrState);
                        vc->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                        vc->x = mv->x; vc->y = mv->y; vc->z = mv->z;
                        if (mv->z > max_z) max_z = mv->z;
                        vc->argb0 = argb_out;
                        vc->argb1 = argb_in;
                        vc->d1 = vc->d2 = 0;
                        pvr_dr_commit(vc);
                    }
                    continue;
                }
                pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
                vert->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                vert->x = mv->x;
                vert->y = mv->y;
                vert->z = mv->z;
                if (mv->z > max_z) max_z = mv->z;
                vert->u = mv->u;
                vert->v = mv->v;
                vert->argb = mv->argb;
                vert->oargb = mv->oargb;
                pvr_dr_commit(vert);
            }
        }
    }

    sTrVertCount  = 0;
    sTrBatchCount = 0;
}

/* Submit an untextured quad (2 tris) via DR - used for fillrect.  */
static int sPvrNoDepthColList = -1;
static void pvr_submit_quad_col(float x0, float y0, float x1, float y1, float z, u32 argb) {
    SHZ_PREFETCH(&sPvrHdrNoDepthCol);
    /* Rebuild header if list type changed */
//    if (sPvrNoDepthColList != sPvrCurrentList) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, sPvrCurrentList);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.gen.specular = 0;
        cxt.depth.comparison = /* sDepthTestEnabled ? PVR_DEPTHCMP_GEQUAL : */ PVR_DEPTHCMP_ALWAYS ;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;//ENABLE;
        pvr_poly_compile(&sPvrHdrNoDepthCol, &cxt);
        sPvrNoDepthColList = sPvrCurrentList;
//    }
    shz_sq_memcpy32_1(pvr_dr_target(sDrState), &sPvrHdrNoDepthCol);
    sPvrFrameBytes += 32;

    float vx[4], vy[4];

    vx[0] = x0; vy[0] = y0;
    vx[1] = x1; vy[1] = y0;
    vx[2] = x0; vy[2] = y1;
    vx[3] = x1; vy[3] = y1;

//    float zp = ((argb & 0x00ffffff) && (sPvrCurrentList == PVR_LIST_TR_POLY)) ? z * 128.0f : z;

    for (int i = 0; i < 4; i++) {
        pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
        vert->flags = ((i == 3)) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        vert->x = vx[i]; vert->y = vy[i]; vert->z = z; //zp;
        vert->argb = argb; vert->oargb = 0;
        pvr_dr_commit(vert);
    }
    sPvrFrameBytes += 4 * 32;
    sPvrNeedHeader = 1;
    sPvrColDirty = 1;
    sPvrTexDirty = 1;
}

/* Submit a textured quad (2 tris) via DR - used for texrect/BG */
static void pvr_submit_quad_tex(float x0, float y0, float x1, float y1, float z,
                                float u0, float v0, float u1, float v1, u32 argb, int flip) {
    float vx[4], vy[4], vu[4], vv[4];

    vx[0] = x0; vy[0] = y0; vu[0] = u0; vv[0] = v0;
    vx[1] = x1; vy[1] = y0; vu[1] = u1; vv[1] = v0;
    vx[2] = x0; vy[2] = y1; vu[2] = u0; vv[2] = v1;
    vx[3] = x1; vy[3] = y1; vu[3] = u1; vv[3] = v1;

    for (int i = 0; i < 4; i++) {
        pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
        vert->flags = ((i == 3)) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        vert->x = vx[i]; vert->y = vy[i]; vert->z = z;
        vert->u = vu[i]; vert->v = vv[i];
        vert->argb = argb; vert->oargb = 0;
        pvr_dr_commit(vert);
    }
    sPvrFrameBytes += 4 * 32;
    sPvrNeedHeader = 1;
}

/* Submit PVR polygon header lazily — called before first vertex of each batch */
static void pvr_ensure_header(void) {
    if (!sPvrNeedHeader) return;
    /* Lazily resolve rendertile texture if dirty */
    if (sTiles[G_TX_RENDERTILE].texDirty) {
        resolve_tile_texture(G_TX_RENDERTILE);
//        if ((sCurrentboundTexture != NULL) && (sCurrentboundTexture->id != sTiles[G_TX_RENDERTILE].texture->id)) {
        if (sCurrentboundTexture != sTiles[G_TX_RENDERTILE].texture) {
            sCurrentboundTexture = sTiles[G_TX_RENDERTILE].texture;
            sEmitCacheDirty = 1;   /* texture scale/dims changed */
            sCurrentTexWidth = sTiles[G_TX_RENDERTILE].width;
            sCurrentTexHeight = sTiles[G_TX_RENDERTILE].height;
            get_texture_uv_scale(sCurrentboundTexture, &sCurrentTexUScale, &sCurrentTexVScale);
            sPvrTexDirty = 1;
        }
    }

    if (sTrBuffering) {
        /* Start a new TR batch with current state compiled for TR list */
        tr_start_batch();
        sPvrNeedHeader = 0;
        return;
    }

    if (sPtBuffering) {
        /* Start a new PT batch with current state compiled for PT list */
        pt_start_batch();
        sPvrNeedHeader = 0;
        return;
    }

    if ((sCurrentboundTexture != NULL) && (sCurrentboundTexture->id != 0)) {
        SHZ_PREFETCH(&sPvrHdrTex);
        if (sPvrTexDirty || (sPvrHdrTexId != sCurrentboundTexture->id)) {
            pvr_rebuild_tex_header(sCurrentboundTexture);
            sPvrTexDirty = 0;
        }
        shz_sq_memcpy32_1(pvr_dr_target(sDrState), &sPvrHdrTex);
        sPvrFrameBytes += 32;
    } else {
        SHZ_PREFETCH(&sPvrHdrCol);
        if (sPvrColDirty) {
            pvr_rebuild_col_header();
            sPvrColDirty = 0;
        }
        shz_sq_memcpy32_1(pvr_dr_target(sDrState), &sPvrHdrCol);
        sPvrFrameBytes += 32;
    }
    sPvrNeedHeader = 0;
}

void flush_triangles(void) {
    /* Vertices already submitted directly to PVR store queue.
       Just mark that the next batch needs a fresh header. */
    sPvrNeedHeader = 1;
}
// ============================================================================
#if 0
static void draw_texrect(u32 w0, u32 w1, u32 w2, u32 w3, int flip) {
    PROF_BEGIN(sPF_Draw2D);
    flush_triangles();

    u32 xh = (w0 >> 12) & 0xFFF;
    u32 yh = (w0) & 0xFFF;
    int tile = (w1 >> 24) & 0x7;
    u32 xl = (w1 >> 12) & 0xFFF;
    u32 yl = (w1) & 0xFFF;
    s16 s = (s16)(w2 >> 16);
    s16 t = (s16)(w2 & 0xFFFF);
    s16 dsdx = (s16)(w3 >> 16);
    s16 dtdy = (s16)(w3 & 0xFFFF);
    u8 copy = 0;
//    uint32_t saved_combine_mode = sComb
    if ((sOtherModeH & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        copy = 1;
        // Color combiner is turned off in copy mode
//        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0));

        // Per documentation one extra pixel is added in this modes to each edge
    }

    float width_orig = (xh - xl) / 4.0f;
    float height_orig = (yh - yl) / 4.0f;
    float x0 = xl / 4.0f * SCREEN_SCALE;
    float y0 = yl / 4.0f * SCREEN_SCALE;
    float x1 = xh / 4.0f * SCREEN_SCALE;
    float y1 = yh / 4.0f * SCREEN_SCALE;
    float s0 = s / 32.0f;
    float t0 = t / 32.0f;
    float dsdx_f = dsdx / 1024.0f;
    float dtdy_f = dtdy / 1024.0f;
    float s1, t1;

    if (flip) {
        s1 = s0 + (height_orig * dsdx_f);
        t1 = t0 + (width_orig * dtdy_f);
    } else {
        s1 = s0 + (width_orig * dsdx_f);
        t1 = t0 + (height_orig * dtdy_f);
    }

    /* Resolve texture for this tile (lazy or CI re-resolve) */
    if (sTiles[tile].texDirty)
        resolve_tile_texture(tile);
    else if (sTiles[tile].fmt == G_IM_FMT_CI && sTiles[tile].texAddr != 0) {
        TextureCacheEntry* entry = get_texture(sTiles[tile].texAddr, sTiles[tile].fmt, sTiles[tile].siz,
                                      sTiles[tile].width, sTiles[tile].height, sTiles[tile].line,
                                      sTiles[tile].texCms, sTiles[tile].texCmt, sTiles[tile].palette, 0);
        if (entry != NULL)
            sTiles[tile].texture = entry;
    }

    TextureCacheEntry* texID = sTiles[tile].texture;
    if (!texID) texID = sCurrentboundTexture;
    if (!texID) { PROF_END(sPF_Draw2D); return; }

    /* Get UV scale for POW2 texture */
    float tex_u_scale, tex_v_scale;
    get_texture_uv_scale(texID, &tex_u_scale, &tex_v_scale);

    float tw = (sTiles[tile].width > 0) ? (float)sTiles[tile].width : (float)sCurrentTexWidth;
    float th = (sTiles[tile].height > 0) ? (float)sTiles[tile].height : (float)sCurrentTexHeight;
    if (tw < 1.0f) tw = 1.0f;
    if (th < 1.0f) th = 1.0f;

    /* Subtract tile offset and normalize UVs */
    float uls_pix = (float)sTiles[tile].uls / 4.0f;
    float ult_pix = (float)sTiles[tile].ult / 4.0f;
    s0 -= uls_pix; s1 -= uls_pix;
    t0 -= ult_pix; t1 -= ult_pix;
    if (copy) {
        s1 += 1;
        t1 += 1;
    }

    float u0 = (s0 / tw) * tex_u_scale;
    float u1 = (s1 / tw) * tex_u_scale;
    float v0_uv = (t0 / th) * tex_v_scale;
    float v1_uv = (t1 / th) * tex_v_scale;

    /* Evaluate combiner cycle 0 to determine vertex modulation color.
       Texrects have no vertex shading so shade = white.
       TEXEL0/TEXEL1 evaluate to 1.0, so the result is the color the
       texture should be multiplied by (e.g. primColor for MODULATEIA_PRIM,
       white for DECALRGBA). */
    float shade_w[4] = { 1, 1, 1, 1 };
    float cc_zero[4] = { 0, 0, 0, 0 };
    float ccA[4], ccB[4], ccC[4], ccD[4], cc_out[4];
    get_cc_input_4bit(sCombinerState.a0, shade_w, cc_zero, ccA);
    get_cc_input_4bit(sCombinerState.b0, shade_w, cc_zero, ccB);
    get_cc_input_5bit(sCombinerState.c0, shade_w, cc_zero, ccC);
    get_cc_input_4bit(sCombinerState.d0, shade_w, cc_zero, ccD);
    for (int i = 0; i < 4; i++) {
        cc_out[i] = (ccA[i] - ccB[i]) * ccC[i] + ccD[i];
        if (cc_out[i] < 0.0f) cc_out[i] = 0.0f;
        if (cc_out[i] > 1.0f) cc_out[i] = 1.0f;
    }
    u8 cr = (u8)(cc_out[0] * 255.0f);
    u8 cg = (u8)(cc_out[1] * 255.0f);
    u8 cb = (u8)(cc_out[2] * 255.0f);
    u8 ca = (u8)(cc_out[3] * 255.0f);
    u32 argb = (ca << 24) | (cr << 16) | (cg << 8) | cb;
//    if (copy)
  //      argb = 0xffffffff;

    /* Build one-off textured header for 2D overlay.
       Each texrect gets an incrementing Z (s2DDepth) so PVR's
       tile-based renderer resolves overlapping 2D draws in the
       correct back-to-front order. */
    int ptw = texID->pow2Info.padded_w;
    int pth = texID->pow2Info.padded_h;
    u32 pfmt = pvr_tex_fmt_for(texID->fmt, texID->siz);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, sPvrCurrentList,
                      pfmt | PVR_TXRFMT_NONTWIDDLED,
                      ptw, pth,
                      texID->id,
                      pvr_tex_filter());
    /* Use REPLACE when combiner says texture is unmodulated (white vertex),
       MODULATEALPHA otherwise to multiply texture by the combiner color. */
    cxt.txr.env = (argb == 0xFFFFFFFF) ? PVR_TXRENV_REPLACE : PVR_TXRENV_MODULATEALPHA;
    /* Set UV clamp/flip from the tile's wrap modes (health/magic bars need this) */
    {
        u32 cms = sTiles[tile].cms;
        u32 cmt = sTiles[tile].cmt;
        int uv_clamp = 0;
        int uv_flip = 0;
        if (cms & 2) uv_clamp |= PVR_UVCLAMP_U;
        if (cmt & 2) uv_clamp |= PVR_UVCLAMP_V;
        if (cms & 1) uv_flip  |= PVR_UVFLIP_U;
        if (cmt & 1) uv_flip  |= PVR_UVFLIP_V;
        cxt.txr.uv_clamp = uv_clamp;
        cxt.txr.uv_flip = uv_flip;
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    if (sAlphaTestEnabled || sBlendingEnabled) {
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    }
    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
    pvr_poly_compile(&hdr, &cxt);
    SHZ_PREFETCH(&hdr);
    shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
    sPvrFrameBytes += 32;
    s2DDepth += OVERLAY_Z_STEP;

    pvr_submit_quad_tex(x0, y0, x1, y1, s2DDepth * 1e6f, u0, v0_uv, u1, v1_uv, argb, flip);
    PROF_END(sPF_Draw2D);
}
#endif

/* ==== Lens of Truth (modifier-volume masking) ================================
   N64 mechanism (z_actor.c Actor_DrawLensActors): a FULL-SCREEN texrect with
   G_ZS_PRIM + SetPrimDepth(0,0). Texture: the I8 64x64 circle quadrant,
   MIRRORed into a 128x128 window at the tile's uls/ult; beyond the window the
   N64 tile CLAMP holds the opaque outer texel (74). Where the combined alpha
   passes, the rect writes NEAREST depth (Z_UPD), z-rejecting later z-tested
   draws. SHOW_ACTORS rooms: alpha = TEXEL0 (mask OUTSIDE the circle) + the
   same rect draws the red shroud; HIDE rooms: depth-only, inverted alpha
   (74 - TEXEL0, mask INSIDE), and a later Z-less texrect draws the tint.
   DC: a depth-write mask cannot work on PVR -- the hardware depth-tests the
   ENTIRE PT+TR phases against it (world cutouts, blended world and the
   multitex ground pass all vanished in the masked region on HW). Instead the
   mask texrect only ARMS the circle (position/radius/polarity); the
   'LNS1'..'LNS0' sentinels around the lens-actor draws route that geometry --
   and only it -- through modifier-affected TR batches, and a circle modifier
   volume on TR_MOD flips them to their hidden parameter set per pixel. The
   visible shroud/glasses pass is drawn here from a baked pre-mirrored 128x128
   window texture with true texel-space UVs (PVR clamp = N64 tile clamp),
   because the generic 2D path cannot express mirror+clamp.
   The asset is compiled as u64 words -> on little-endian SH4 each 8-byte
   group is byte-reversed; the bake reads offset^7 (swap_bytes_64 equivalent). */

static pvr_ptr_t lens_visible_tex(u32 texAddr) {
    if (sLensVisTexPvr != NULL && sLensVisSrc == texAddr)
        return sLensVisTexPvr;
    const u8* src = (const u8*)pc_resolve_addr(texAddr);
    if (src == NULL)
        return NULL;
    for (int y = 0; y < 128; y++) {
        int sy = (y < 64) ? y : 127 - y;
        u16* drow = &sTexDataBuf[y * 128];
        for (int x = 0; x < 128; x++) {
            int sx = (x < 64) ? x : 127 - x;
            u8 I = src[(sy * 64 + sx) ^ 7];
            u16 n = I >> 4;                    /* intensity+alpha, modulated by prim */
            drow[x] = (n << 12) | (n << 8) | (n << 4) | n;
        }
    }
    /* Measure the circle radius for the modifier volume: on the window's
       center row (source y=63, the row nearest the mirror seam), walk from the
       seam (x=63) outward and count how far the inside (I < 74) extends. */
    {
        int r = 0;
        for (int x = 63; x >= 0; x--) {
            u8 I = src[(63 * 64 + x) ^ 7];
            if (I < 74) r = 64 - x;
        }
        if (r > 2) sLensRadTex = (float)r;
    }
    if (sLensVisTexPvr == NULL)
        sLensVisTexPvr = pvr_mem_malloc(128 * 128 * 2);
    if (sLensVisTexPvr == NULL) {
        if (sLensLog < 8) { sLensLog++; printf("LENS: visible tex VRAM OOM\n"); }
        return NULL;
    }
    pvr_txr_load_ex(sTexDataBuf, sLensVisTexPvr, 128, 128, PVR_TXRLOAD_16BPP);
    sLensVisSrc = texAddr;
    return sLensVisTexPvr;
}

/* The visible half of the lens overlay (red shroud in SHOW rooms, the tint in
   HIDE rooms): the baked window texture modulated by the combiner colour,
   drawn live like a normal 2D texrect. */
static void lens_draw_visible(float x0, float y0, float x1, float y1,
                              float lu0, float lv0, float lu1, float lv1,
                              u32 argb, u32 texAddr) {
    pvr_ptr_t tex = lens_visible_tex(texAddr);
    if (tex == NULL)
        return;
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, sPvrCurrentList, PVR_TXRFMT_ARGB4444, 128, 128, tex, pvr_tex_filter());
    cxt.gen.culling      = PVR_CULLING_NONE;
    cxt.gen.clip_mode    = PVR_USERCLIP_INSIDE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write      = PVR_DEPTHWRITE_DISABLE;   /* CLD_SURF never writes Z */
    cxt.blend.src        = PVR_BLEND_SRCALPHA;
    cxt.blend.dst        = PVR_BLEND_INVSRCALPHA;
    cxt.txr.env          = PVR_TXRENV_MODULATEALPHA;
    cxt.txr.alpha        = PVR_TXRALPHA_ENABLE;
    cxt.txr.uv_clamp     = PVR_UVCLAMP_U | PVR_UVCLAMP_V;
    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
    pvr_poly_compile(&hdr, &cxt);
    SHZ_PREFETCH(&hdr);
    shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
    sPvrFrameBytes += 32;
    s2DDepth += OVERLAY_Z_STEP;
    pvr_submit_quad_tex(x0, y0, x1, y1, s2DDepth * 1e6f, lu0, lv0, lu1, lv1, argb, 0);
}

static void draw_texrect(u32 w0, u32 w1, u32 w2, u32 w3, int flip) {
    PROF_BEGIN(sPF_Draw2D);
    flush_triangles();

    u32 xh = (w0 >> 12) & 0xFFF;
    u32 yh = (w0) & 0xFFF;
    int tile = (w1 >> 24) & 0x7;
    u32 xl = (w1 >> 12) & 0xFFF;
    u32 yl = (w1) & 0xFFF;
    s16 s = (s16)(w2 >> 16);
    s16 t = (s16)(w2 & 0xFFFF);
    s16 dsdx = (s16)(w3 >> 16);
    s16 dtdy = (s16)(w3 & 0xFFFF);

    float width_orig = (xh - xl) / 4.0f;
    float height_orig = (yh - yl) / 4.0f;
    float x0 = xl / 4.0f * SCREEN_SCALE;
    float y0 = yl / 4.0f * SCREEN_SCALE;
    float x1 = xh / 4.0f * SCREEN_SCALE;
    float y1 = yh / 4.0f * SCREEN_SCALE;
    float s0 = s / 32.0f;
    float t0 = t / 32.0f;
    float dsdx_f = dsdx / 1024.0f;
    float dtdy_f = dtdy / 1024.0f;
    float s1, t1;

    if (flip) {
        s1 = s0 + (height_orig * dsdx_f);
        t1 = t0 + (width_orig * dtdy_f);
    } else {
        s1 = s0 + (width_orig * dsdx_f);
        t1 = t0 + (height_orig * dtdy_f);
    }

    /* Resolve texture for this tile (lazy or CI re-resolve) */
    if (sTiles[tile].texDirty)
        resolve_tile_texture(tile);
    if (sTiles[tile].fmt == G_IM_FMT_CI && sTiles[tile].texAddr != 0) {
        TextureCacheEntry* entry = get_texture(sTiles[tile].texAddr, sTiles[tile].fmt, sTiles[tile].siz,
                                      sTiles[tile].width, sTiles[tile].height, sTiles[tile].line,
                                      sTiles[tile].texCms, sTiles[tile].texCmt, sTiles[tile].palette, 0);
        if (entry != NULL)
            sTiles[tile].texture = entry;
    }

    TextureCacheEntry* texID = sTiles[tile].texture;
    if (!texID) texID = sCurrentboundTexture;
    if (!texID) { PROF_END(sPF_Draw2D); return; }

    /* Get UV scale for POW2 texture */
    float tex_u_scale, tex_v_scale;
    get_texture_uv_scale(texID, &tex_u_scale, &tex_v_scale);

    float tw = (sTiles[tile].width > 0) ? (float)sTiles[tile].width : (float)sCurrentTexWidth;
    float th = (sTiles[tile].height > 0) ? (float)sTiles[tile].height : (float)sCurrentTexHeight;
    if (tw < 1.0f) tw = 1.0f;
    if (th < 1.0f) th = 1.0f;

    /* Save raw s/t before tile offset subtraction (needed for tile 1 UVs) */
    float s0_raw = s0, t0_raw = t0, s1_raw = s1, t1_raw = t1;

    /* Subtract tile offset and normalize UVs */
    float uls_pix = (float)sTiles[tile].uls / 4.0f;
    float ult_pix = (float)sTiles[tile].ult / 4.0f;
    s0 -= uls_pix; s1 -= uls_pix;
    t0 -= ult_pix; t1 -= ult_pix;
    float u0 = (s0 / tw) * tex_u_scale;
    float u1 = (s1 / tw) * tex_u_scale;
    float v0_uv = (t0 / th) * tex_v_scale;
    float v1_uv = (t1 / th) * tex_v_scale;

    /* ---- Multi-texture 2-cycle path ----
       When the combiner references TEXEL1, we decompose the 2-cycle combiner
       into 3 additive passes using linear decomposition:
         Pass 1: TEX0 × combiner(TEX0=1, TEX1=0) — TEX0's contribution
         Pass 2: TEX1 × combiner(TEX0=0, TEX1=1) — TEX1's contribution
         Pass 3: flat   combiner(TEX0=0, TEX1=0) — constant offset (e.g. ENV)
       Pass 1 writes alpha (for masking); passes 2-3 use DESTALPHA to confine
       the effect to the mask/silhouette region established by TEX0's alpha. */
    if (sMultiTexEnabled) {
        if (sTiles[1].texDirty) resolve_tile_texture(1);
        TextureCacheEntry* tex1 = sTiles[1].texture;
        if (tex1) {
            /* Compute multi-pass vertex colors using direct lerp approach.
               Instead of decomposing through the combiner (which crushes TEX1's
               contribution through the cycle 1 transform), we compute:
                 result = scale × lerp(TEX0, TEX1, f) + offset
               where scale = cycle1.a - cycle1.b, offset = cycle1.d, and
               f = cycle0's blend factor (ENV_ALPHA, PRIM_LOD_FRAC, etc).
               This gives visible fire/effect modulation between the textures. */

            /* Extract cycle 0 blend factor from c0 mux */
            float blend_f = 0.5f;
            switch (sCombinerState.c0) {
                case 10: blend_f = sPrimColor[3]; break;   /* PRIM_ALPHA */
                case 12: blend_f = sEnvColor[3]; break;    /* ENV_ALPHA */
                case 13: case 14: blend_f = sPrimLodFrac; break; /* LOD_FRAC / PRIM_LOD_FRAC */
                default: break;
            }

            /* Extract cycle 1 scale and offset.
               PASS2: a1=b1=c1=d1=0 → result = COMBINED (scale=1, offset=0)
               Lerp:  c1=0(COMBINED) → result = (a1-b1)*COMBINED + d1 */
            int c1_pass2 = (sCombinerState.a1 == 0 && sCombinerState.b1 == 0 &&
                            sCombinerState.c1 == 0 && sCombinerState.d1 == 0);
            float scale_rgb[3], offset_rgb[3];
            if (c1_pass2) {
                scale_rgb[0] = scale_rgb[1] = scale_rgb[2] = 1.0f;
                offset_rgb[0] = offset_rgb[1] = offset_rgb[2] = 0.0f;
            } else {
                float shade_w[4] = {1,1,1,1};
                float dummy[4] = {0,0,0,0};
                float a1v[4], b1v[4], d1v[4];
                get_cc_input_4bit(sCombinerState.a1, shade_w, dummy, a1v);
                get_cc_input_4bit(sCombinerState.b1, shade_w, dummy, b1v);
                get_cc_input_4bit(sCombinerState.d1, shade_w, dummy, d1v);
                for (int i = 0; i < 3; i++) {
                    scale_rgb[i] = a1v[i] - b1v[i];
                    if (scale_rgb[i] < 0.0f) scale_rgb[i] = 0.0f;
                    offset_rgb[i] = d1v[i];
                }
            }

            /* Get b0 value — the constant subtracted from TEX1 in cycle 0.
               Cycle 0: (TEX1 - b0) × f + TEX0 = TEX0 + TEX1×f - b0×f
               Since we can't subtract b0×f in the multi-pass, fold it into
               TEX0's weight: TEX0 × (1 - b0×f) instead of TEX0 × (1-f).
               This matches the N64 baseline brightness when TEX1=0. */
            float b0v[4];
            {
                float _sw[4] = {1,1,1,1}, _dm[4] = {0,0,0,0};
                get_cc_input_4bit(sCombinerState.b0, _sw, _dm, b0v);
            }

            /* Compute pass 1 alpha from combiner (TEX0=1 contribution) */
            float alpha_tmp[4];
            {
                float _sw[4] = {1,1,1,1};
                sTexSubst0 = 1.0f; sTexSubst1 = 0.0f;
                get_combiner_color(_sw, alpha_tmp);
                sTexSubst0 = 1.0f; sTexSubst1 = 1.0f;
            }
            float pass1_alpha = alpha_tmp[3];

            /* Pass 1 vtx: scale × (1 - b0×f) per channel — brighter baseline
               Pass 2 vtx: scale × f — fire contribution
               Pass 3 vtx: offset (ENV constant) */
            float p1_rgb[3], p2_rgb[3];
            for (int i = 0; i < 3; i++) {
                float p1f = 1.0f - b0v[i] * blend_f;
                if (p1f < 0.0f) p1f = 0.0f;
                p1_rgb[i] = scale_rgb[i] * p1f;
                p2_rgb[i] = scale_rgb[i] * blend_f;
            }

            u8 p1a = (u8)(pass1_alpha * 255.0f);
            u32 argb_p1 = (p1a << 24) |
                          ((u8)(p1_rgb[0] * 255.0f) << 16) |
                          ((u8)(p1_rgb[1] * 255.0f) << 8) |
                          (u8)(p1_rgb[2] * 255.0f);

            u32 argb_p2 = (0xFF << 24) |
                          ((u8)(p2_rgb[0] * 255.0f) << 16) |
                          ((u8)(p2_rgb[1] * 255.0f) << 8) |
                          (u8)(p2_rgb[2] * 255.0f);

            u8 p3r = (u8)(offset_rgb[0] * 255.0f);
            u8 p3g = (u8)(offset_rgb[1] * 255.0f);
            u8 p3b = (u8)(offset_rgb[2] * 255.0f);
            u32 argb_p3 = (0xFF << 24) | (p3r << 16) | (p3g << 8) | p3b;

            s2DDepth += OVERLAY_Z_STEP;
            float z = s2DDepth * 1e6f;

            /* --- Pass 0: Clear FB alpha in texrect region ---
               Render white quad with alpha=0, blend DESTCOLOR/ZERO.
               result = (1,1,1,0) × dst = (dst.r, dst.g, dst.b, 0)
               This preserves the existing color (3D logo etc) but zeros
               the alpha channel so DESTALPHA masking in passes 2-3 works
               regardless of what prior rendering wrote to FB alpha. */
            {
                pvr_poly_cxt_t cxt;
                pvr_poly_cxt_col(&cxt, sPvrCurrentList);
                cxt.gen.culling = PVR_CULLING_NONE;
                cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
                cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
                cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
                cxt.blend.src = PVR_BLEND_DESTCOLOR;
                cxt.blend.dst = PVR_BLEND_ZERO;
                pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
                pvr_poly_compile(&hdr, &cxt);
                SHZ_PREFETCH(&hdr);
                shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
                sPvrFrameBytes += 32;
                float vx[4] = {x0, x1, x0, x1};
                float vy[4] = {y0, y0, y1, y1};
                for (int i = 0; i < 4; i++) {
                    pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
                    vert->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                    vert->x = vx[i]; vert->y = vy[i]; vert->z = z;
                    vert->u = 0; vert->v = 0;
                    vert->argb = 0x00FFFFFF; /* white color, zero alpha */
                    vert->oargb = 0;
                    pvr_dr_commit(vert);
                }
                sPvrFrameBytes += 4 * 32;
            }

            /* --- Pass 1: TEX0 (alpha-blended, establishes alpha mask) --- */
            {
                int ptw0 = texID->pow2Info.padded_w;
                int pth0 = texID->pow2Info.padded_h;
                u32 pfmt0 = pvr_tex_fmt_for(texID->fmt, texID->siz);
                pvr_poly_cxt_t cxt;
                pvr_poly_cxt_txr(&cxt, sPvrCurrentList,
                                  pfmt0,// | PVR_TXRFMT_NONTWIDDLED,
                                  ptw0, pth0, texID->id, pvr_tex_filter());
                cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
                { u32 cms = sTiles[tile].cms, cmt = sTiles[tile].cmt;
                  int uvc = 0, uvf = 0;
                  if (cms & 2) uvc |= PVR_UVCLAMP_U;
                  if (cmt & 2) uvc |= PVR_UVCLAMP_V;
                  if (cms & 1) uvf |= PVR_UVFLIP_U;
                  if (cmt & 1) uvf |= PVR_UVFLIP_V;
                  cxt.txr.uv_clamp = uvc; cxt.txr.uv_flip = uvf; }
                cxt.gen.culling = PVR_CULLING_NONE;
                cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
                cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
                cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
                cxt.blend.src = PVR_BLEND_SRCALPHA;
                cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
                cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
                pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
                pvr_poly_compile(&hdr, &cxt);
                SHZ_PREFETCH(&hdr);
                shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
                sPvrFrameBytes += 32;
                pvr_submit_quad_tex(x0, y0, x1, y1, z, u0, v0_uv, u1, v1_uv, argb_p1, flip);
            }

            /* --- Pass 2: TEX1 (masked by FB alpha via DESTALPHA) --- */
            {
                /* Tile 1 UV computation */
                float uls1_pix = sTiles[1].uls / 4.0f;
                float ult1_pix = sTiles[1].ult / 4.0f;
                int shifts1 = sTiles[1].shifts;
                int shiftt1 = sTiles[1].shiftt;
                float u_sh1 = 1.0f, v_sh1 = 1.0f;
                if (shifts1 > 0 && shifts1 < 11)
                    u_sh1 = 1.0f / (float)(1 << shifts1);
                else if (shifts1 >= 11)
                    u_sh1 = (float)(1 << (16 - shifts1));
                if (shiftt1 > 0 && shiftt1 < 11)
                    v_sh1 = 1.0f / (float)(1 << shiftt1);
                else if (shiftt1 >= 11)
                    v_sh1 = (float)(1 << (16 - shiftt1));

                float tw1 = sTiles[1].width > 0 ? (float)sTiles[1].width : 32.0f;
                float th1 = sTiles[1].height > 0 ? (float)sTiles[1].height : 32.0f;
                float t1us, t1vs;
                get_texture_uv_scale(tex1, &t1us, &t1vs);

                float st0 = (s0_raw - uls1_pix) * u_sh1;
                float st1 = (s1_raw - uls1_pix) * u_sh1;
                float tt0 = (t0_raw - ult1_pix) * v_sh1;
                float tt1 = (t1_raw - ult1_pix) * v_sh1;
                float ut0 = (st0 / tw1) * t1us;
                float ut1 = (st1 / tw1) * t1us;
                float vt0 = (tt0 / th1) * t1vs;
                float vt1 = (tt1 / th1) * t1vs;

                int ptw1 = tex1->pow2Info.padded_w;
                int pth1 = tex1->pow2Info.padded_h;
                u32 pfmt1 = pvr_tex_fmt_for(tex1->fmt, tex1->siz);
                pvr_poly_cxt_t cxt;
                pvr_poly_cxt_txr(&cxt, sPvrCurrentList,
                                  pfmt1,// | PVR_TXRFMT_NONTWIDDLED,
                                  ptw1, pth1, tex1->id, pvr_tex_filter());
                cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
                { u32 cms = sTiles[1].cms, cmt = sTiles[1].cmt;
                  int uvc = 0, uvf = 0;
                  if (cms & 2) uvc |= PVR_UVCLAMP_U;
                  if (cmt & 2) uvc |= PVR_UVCLAMP_V;
                  if (cms & 1) uvf |= PVR_UVFLIP_U;
                  if (cmt & 1) uvf |= PVR_UVFLIP_V;
                  cxt.txr.uv_clamp = uvc; cxt.txr.uv_flip = uvf; }
                cxt.gen.culling = PVR_CULLING_NONE;
                cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
                cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
                cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
                cxt.blend.src = PVR_BLEND_DESTALPHA;
                cxt.blend.dst = PVR_BLEND_ONE;
                cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
                pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
                pvr_poly_compile(&hdr, &cxt);
                SHZ_PREFETCH(&hdr);
                shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
                sPvrFrameBytes += 32;
                pvr_submit_quad_tex(x0, y0, x1, y1, z, ut0, vt0, ut1, vt1, argb_p2, flip);
            }

            /* --- Pass 3: Constant offset (masked by FB alpha) --- */
            {
                if (argb_p3 & 0x00FFFFFF) {
                    pvr_poly_cxt_t cxt;
                    pvr_poly_cxt_col(&cxt, sPvrCurrentList);
                    cxt.gen.culling = PVR_CULLING_NONE;
                    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
                    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
                    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
                    cxt.blend.src = PVR_BLEND_DESTALPHA;
                    cxt.blend.dst = PVR_BLEND_ONE;
                    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
                    pvr_poly_compile(&hdr, &cxt);
                    SHZ_PREFETCH(&hdr);
                    shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
                    sPvrFrameBytes += 32;
                    float vx[4] = {x0, x1, x0, x1};
                    float vy[4] = {y0, y0, y1, y1};
                    for (int i = 0; i < 4; i++) {
                        pvr_vertex_t *vert = (pvr_vertex_t *)pvr_dr_target(sDrState);
                        vert->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                        vert->x = vx[i]; vert->y = vy[i]; vert->z = z;
                        vert->u = 0; vert->v = 0;
                        vert->argb = argb_p3; vert->oargb = 0;
                        pvr_dr_commit(vert);
                    }
                    sPvrFrameBytes += 4 * 32;
                }
            }

            sPvrNeedHeader = 1;
            PROF_END(sPF_Draw2D);
            return;
        }
        /* tex1 not available — fall through to single-texture path */
    }

    /* Evaluate full 2-cycle combiner to determine vertex modulation color.
       Texrects have no vertex shading so shade = white.
       TEXEL0/TEXEL1 evaluate to sTexSubst (default 1.0), so the result
       is the color the texture should be multiplied by. */
    float shade_w[4] = { 1, 1, 1, 1 };
    float cc_out[4];
    get_combiner_color(shade_w, cc_out);
    u8 cr = (u8)(cc_out[0] * 255.0f);
    u8 cg = (u8)(cc_out[1] * 255.0f);
    u8 cb = (u8)(cc_out[2] * 255.0f);
    u8 ca = (u8)(cc_out[3] * 255.0f);
    u32 argb = vismono_argb((ca << 24) | (cr << 16) | (cg << 8) | cb);

    /* Lens of Truth passes: prim-depth 0 + G_ZS_PRIM on a mirrored I8 texrect.
       Z_UPD variants ARM the circle modifier volume (+ draw the red shroud in
       SHOW rooms); the Z-less variant is the HIDE rooms' tint overlay. Drawn
       here with the baked 128x128 window texture and true texel-space UVs
       (PVR clamp = N64 tile clamp). */
    if ((sOtherModeL & G_ZS_PRIM) && sPrimDepthZ == 0 &&
        sTiles[tile].fmt == G_IM_FMT_I && sTiles[tile].siz == G_IM_SIZ_8b &&
        (sTiles[tile].cms & 1) && sTiles[tile].masks == 6) {
        float lu0 = (s0_raw - uls_pix) * (1.0f / 128.0f);
        float lu1 = (s1_raw - uls_pix) * (1.0f / 128.0f);
        float lv0 = (t0_raw - ult_pix) * (1.0f / 128.0f);
        float lv1 = (t1_raw - ult_pix) * (1.0f / 128.0f);
        if (sOtherModeL & Z_UPD) {
            int preserves_color = (((sOtherModeL >> 26) & 3) == G_BL_0) &&
                                  (((sOtherModeL >> 22) & 3) == G_BL_CLR_MEM);
            /* Arm the circle for the modifier volume: screen position of the
               128-texel window from the UV mapping, radius from the bake. */
            if (lens_visible_tex(sTiles[tile].texAddr) != NULL && lu1 != lu0 && lv1 != lv0) {
                float wx0 = x0 + (0.0f - lu0) * (x1 - x0) / (lu1 - lu0);
                float wx1 = x0 + (1.0f - lu0) * (x1 - x0) / (lu1 - lu0);
                float wy0 = y0 + (0.0f - lv0) * (y1 - y0) / (lv1 - lv0);
                float wy1 = y0 + (1.0f - lv0) * (y1 - y0) / (lv1 - lv0);
                sLensCx = 0.5f * (wx0 + wx1);
                sLensCy = 0.5f * (wy0 + wy1);
                sLensRx = (wx1 - wx0) * (sLensRadTex / 128.0f);
                sLensRy = (wy1 - wy0) * (sLensRadTex / 128.0f);
                sLensShow = !preserves_color;
                sLensMaskSeen = 1;
            }
            if (preserves_color) {   /* depth-only on N64: nothing visible here */
                sPvrNeedHeader = 1;
                PROF_END(sPF_Draw2D);
                return;
            }
        }
        lens_draw_visible(x0, y0, x1, y1, lu0, lv0, lu1, lv1, argb, sTiles[tile].texAddr);
        PROF_END(sPF_Draw2D);
        return;
    }

    /* Build one-off textured header for 2D overlay.
       Each texrect gets an incrementing Z (s2DDepth) so PVR's
       tile-based renderer resolves overlapping 2D draws in the
       correct back-to-front order. */
    int ptw = texID->pow2Info.padded_w;
    int pth = texID->pow2Info.padded_h;
    u32 pfmt = pvr_tex_fmt_for(texID->fmt, texID->siz);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, sPvrCurrentList,
                      pfmt,// | PVR_TXRFMT_NONTWIDDLED,
                      ptw, pth,
                      texID->id,
                      pvr_tex_filter());
    /* Use REPLACE when combiner says texture is unmodulated (white vertex),
       MODULATEALPHA otherwise to multiply texture by the combiner color. */
    cxt.txr.env = (argb == 0xFFFFFFFF) ? PVR_TXRENV_REPLACE : PVR_TXRENV_MODULATEALPHA;
    /* Set UV clamp/flip from the tile's wrap modes (health/magic bars need this) */
    {
        u32 cms = sTiles[tile].cms;
        u32 cmt = sTiles[tile].cmt;
        int uv_clamp = 0;
        int uv_flip = 0;
        if (cms & 2) uv_clamp |= PVR_UVCLAMP_U;
        if (cmt & 2) uv_clamp |= PVR_UVCLAMP_V;
        if (cms & 1) uv_flip  |= PVR_UVFLIP_U;
        if (cmt & 1) uv_flip  |= PVR_UVFLIP_V;
        cxt.txr.uv_clamp = uv_clamp;
        cxt.txr.uv_flip = uv_flip;
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    if (sAlphaTestEnabled || sBlendingEnabled) {
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    }
    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
    pvr_poly_compile(&hdr, &cxt);
    SHZ_PREFETCH(&hdr);
    shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
    sPvrFrameBytes += 32;
    s2DDepth += OVERLAY_Z_STEP;

    pvr_submit_quad_tex(x0, y0, x1, y1, s2DDepth * 1e6f, u0, v0_uv, u1, v1_uv, argb, flip);
    PROF_END(sPF_Draw2D);
}





static void handle_mtx(u32 w0, u32 w1) {
    PROF_BEGIN(sPF_Mtx);
    Mtx* mtx = (Mtx*)pc_resolve_addr(w1);
    u8 params = w0 & 0xFF;
    int push = !(params & G_MTX_PUSH);
    int load = (params & G_MTX_LOAD);
    int proj = (params & G_MTX_PROJECTION);
    if (!mtx) {
        PROF_END(sPF_Mtx);
        return;
    }

    if (proj) {
        if (load)
            memcpy(rsp.P_matrix, mtx->m, sizeof(rsp.P_matrix));
        else
            mtx_mul(rsp.P_matrix, mtx->m, rsp.P_matrix);
        /* Ortho: P[3][3]==1, P[2][3]==0; Perspective: P[3][3]==0, P[2][3]==-1 */
        sIsOrtho = (rsp.P_matrix[2][3] == 0.0f);
    } else {
        if (push && rsp.modelview_stack_size < 18) {
            memcpy(rsp.modelview_stack[rsp.modelview_stack_size], rsp.modelview_stack[rsp.modelview_stack_size - 1],
                   sizeof(rsp.modelview_stack[0]));
            rsp.modelview_stack_size++;
        }
        if (load)
            memcpy(rsp.modelview_stack[rsp.modelview_stack_size - 1], mtx->m, sizeof(rsp.modelview_stack[0]));
        else
            mtx_mul(rsp.modelview_stack[rsp.modelview_stack_size - 1], mtx->m,
                    rsp.modelview_stack[rsp.modelview_stack_size - 1]);
    }
    recalc_MP();
    PROF_END(sPF_Mtx);
}

static void handle_popmtx(u32 w1) {
    PROF_BEGIN(sPF_Mtx);
    int count = w1 >> 6;
    while (count-- > 0 && rsp.modelview_stack_size > 1)
        rsp.modelview_stack_size--;
    recalc_MP();
    PROF_END(sPF_Mtx);
}

#define recip64k 0.00001526f
static void handle_vtx(u32 w0, u32 w1) {
    PROF_BEGIN(sPF_Vtx);
    int n = (w0 >> 12) & 0xFF;
    int v0 = ((w0 >> 1) & 0x7F) - n;
    Vtx* src = (Vtx*)pc_resolve_addr(w1);
    if (!src || v0 < 0 || v0 + n > UCODE_MAX_VERTS) {
        if (src && v0 + n > UCODE_MAX_VERTS) {
            static u32 sVtxOverLogged;
            if (sVtxOverLogged < 16) { sVtxOverLogged++; printf("VTX_OVER: v0=%d n=%d > %d\n", v0, n, UCODE_MAX_VERTS); }
        }
        PROF_END(sPF_Vtx);
        return;
    }

    int lightingEnabled = (sGeometryMode & G_LIGHTING) != 0;
    int texgen = lightingEnabled && (sGeometryMode & G_TEXTURE_GEN);

    if (lightingEnabled) {
        if (sLightMtxDirty) build_light_matrices();
        if (sNumLights > 7) {
            static u32 sWarnMoreLights = 0;
            if ((sWarnMoreLights++ & 0x1FF) == 0)
                printf("matrix-lighting: scene requested %d directionals (>7 unsupported, extras ignored)\n", sNumLights);
        }
    }

    /* Pass 1: position / fog / clip codes / screen / non-lit color / base UV.
       (XMTRX = MP for the perspective transform.) */
    shz_xmtrx_load_4x4((const float*)rsp.MP_matrix);
    for (int i = 0; i < n; i++) {
        Vtx_t* v = &src[i].v;
        LoadedVertex* d = &sVertexBuffer[v0 + i];

        shz_vec3_t pos = shz_vec3_init((float)v->ob[0], (float)v->ob[1], (float)v->ob[2]);
        shz_vec4_t clip = shz_xmtrx_transform_vec4(shz_vec3_vec4(pos, 1.0f));
        if (sGeometryMode & G_FOG) {
            float w = clip.w, z = clip.z, fog;
            if (z < -w) {
                fog = 0.0f;
            } else {
                fog = (((float)sFogMul * (z / w)) + (float)sFogOfs);
                if (fog > 255.0f) fog = 255.0f;
                fog = (fog >= 0.0f) ? fog : 0.0f;
            }
            d->fog = (u8)fog;
            if ((int)fog < sFogDbgMin) sFogDbgMin = (int)fog;   /* DIAG */
            if ((int)fog > sFogDbgMax) sFogDbgMax = (int)fog;   /* DIAG */
        } else {
            d->fog = 0;
        }
        int idx = v0 + i;
        sClipSpace[idx][0] = clip.x;
        sClipSpace[idx][1] = clip.y;
        sClipSpace[idx][2] = clip.z;
        sClipSpace[idx][3] = clip.w;

        uint8_t cr = 0;
        if (clip.w < 0.0f)  cr |= 0x40;
        if (clip.z > clip.w)  cr |= 0x20;
        if (clip.z < -clip.w) cr |= 0x10;
        if (clip.y > clip.w)  cr |= 0x08;
        if (clip.y < -clip.w) cr |= 0x04;
        if (clip.x > clip.w)  cr |= 0x02;
        if (clip.x < -clip.w) cr |= 0x01;
        sVertexClipCodes[idx] = cr;

        if (clip.w >= NEAR_PLANE) {
            float iw = shz_invf_fsrra(clip.w);
            d->screen_x = (clip.x * iw * sViewportScaleX + sViewportTransX) * SCREEN_SCALE;
            d->screen_y = (-clip.y * iw * sViewportScaleY + sViewportTransY) * SCREEN_SCALE;
            d->inv_w = iw;
            d->behind = 0;
        } else {
            d->screen_x = 0; d->screen_y = 0; d->inv_w = 0; d->behind = 1;
        }

        d->color.a = v->cn[3];
        if (!lightingEnabled) {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }
        if (sGeometryMode & G_FOG) d->color.a = 255 - d->fog;

        /* base UV (texgen overwrites below for lit env-mapped surfaces) */
        d->u = v->tc[0] * rsp.texture_scale_s * recip64k;
        d->v = v->tc[1] * rsp.texture_scale_t * recip64k;
    }

    if (lightingEnabled) {
        /* Pass 2: per-light intensities. XMTRX = COEFF; FTRV(raw s8 normal)_i = cos_i. */
        shz_xmtrx_load_3x3((const shz_mat3x3_t*)sCoeffMtx);
        for (int i = 0; i < n; i++) {
            Vtx_t* v = &src[i].v;
            shz_vec3_t it = shz_xmtrx_transform_vec3(
                shz_vec3_init((float)(s8)v->cn[0], (float)(s8)v->cn[1], (float)(s8)v->cn[2]));
            sLightInten[i].x = (it.x > 0.0f) ? it.x : 0.0f;
            sLightInten[i].y = (it.y > 0.0f) ? it.y : 0.0f;
            sLightInten[i].z = (it.z > 0.0f) ? it.z : 0.0f;
            sLightInten[i].w = 1.0f;   /* drives the ambient term in pass 3 */
        }

        if (sNumLights <= 3) {
            /* Fast path: <=3 directionals + ambient in one matrix pair. Clamp+write. */
            /* Pass 3: combine to RGB. XMTRX = COLOR; FTRV((i0,i1,i2,1)) = (R,G,B,_). */
            shz_xmtrx_load_4x4((const float*)sColorMtx);
            for (int i = 0; i < n; i++) {
                LoadedVertex* d = &sVertexBuffer[v0 + i];
                shz_vec4_t rgb = shz_xmtrx_transform_vec4(sLightInten[i]);
                float r = rgb.x > 1.0f ? 1.0f : rgb.x;
                float g = rgb.y > 1.0f ? 1.0f : rgb.y;
                float b = rgb.z > 1.0f ? 1.0f : rgb.z;
                d->color.r = (uint8_t)(r * 255.0f);
                d->color.g = (uint8_t)(g * 255.0f);
                d->color.b = (uint8_t)(b * 255.0f);
            }
        } else {
            /* >3 directionals: add a second batch (lights 3..6, no ambient) and
               clamp only after summing both batches. */
            /* Pass 2b: batch-2 intensities. XMTRX = COEFF2 (4x4); FTRV(normal,w=0)
               lanes x,y,z,w = cos of lights 3,4,5,6. */
            shz_xmtrx_load_4x4((const float*)sCoeffMtx2);
            for (int i = 0; i < n; i++) {
                Vtx_t* v = &src[i].v;
                shz_vec4_t it = shz_xmtrx_transform_vec4(shz_vec3_vec4(
                    shz_vec3_init((float)(s8)v->cn[0], (float)(s8)v->cn[1], (float)(s8)v->cn[2]), 0.0f));
                sLightInten2[i].x = (it.x > 0.0f) ? it.x : 0.0f;
                sLightInten2[i].y = (it.y > 0.0f) ? it.y : 0.0f;
                sLightInten2[i].z = (it.z > 0.0f) ? it.z : 0.0f;
                sLightInten2[i].w = (it.w > 0.0f) ? it.w : 0.0f;
            }

            /* Pass 3a: batch-1 color -> unclamped float accumulator. */
            shz_xmtrx_load_4x4((const float*)sColorMtx);
            for (int i = 0; i < n; i++) {
                shz_vec4_t rgb = shz_xmtrx_transform_vec4(sLightInten[i]);
                sLightAccum[i][0] = rgb.x;
                sLightAccum[i][1] = rgb.y;
                sLightAccum[i][2] = rgb.z;
            }

            /* Pass 3b: batch-2 color, sum onto accumulator, THEN clamp+write. */
            shz_xmtrx_load_4x4((const float*)sColorMtx2);
            for (int i = 0; i < n; i++) {
                LoadedVertex* d = &sVertexBuffer[v0 + i];
                shz_vec4_t rgb = shz_xmtrx_transform_vec4(sLightInten2[i]);
                float r = sLightAccum[i][0] + rgb.x;
                float g = sLightAccum[i][1] + rgb.y;
                float b = sLightAccum[i][2] + rgb.z;
                r = r > 1.0f ? 1.0f : r;
                g = g > 1.0f ? 1.0f : g;
                b = b > 1.0f ? 1.0f : b;
                d->color.r = (uint8_t)(r * 255.0f);
                d->color.g = (uint8_t)(g * 255.0f);
                d->color.b = (uint8_t)(b * 255.0f);
            }
        }

        /* Texgen pass: env-map UVs. XMTRX = ENV; FTRV(raw normal) = (dotX,dotY,_). */
        if (texgen) {
            /* Amplify env-map UV variation for the Morpha tentacle to tighten its
               smeared caustics. Gated on the tentacle's tile-0 texture (same as the
               recolor gate) so it can't catch other env-mapped surfaces like Link's
               spin-charge effect. texAddr is set at texture-load (before this vertex
               command), so it's valid here even though the texture isn't resolved yet;
               pc_resolve_addr on both sides makes the segment/load state irrelevant. */
            float tg_mul = (pc_resolve_addr(sTiles[0].texAddr) == pc_resolve_addr(gMorphaWaterTex))
                               ? MORPHA_TEXGEN_MUL : 1.0f;
            shz_xmtrx_load_3x3((const shz_mat3x3_t*)sEnvMtx);
            for (int i = 0; i < n; i++) {
                Vtx_t* v = &src[i].v;
                LoadedVertex* d = &sVertexBuffer[v0 + i];
                shz_vec3_t dt = shz_xmtrx_transform_vec3(
                    shz_vec3_init((float)(s8)v->cn[0], (float)(s8)v->cn[1], (float)(s8)v->cn[2]));
                float dotX = dt.x, dotY = dt.y;
                if (sGeometryMode & G_TEXTURE_GEN_LINEAR) {
                    float ax = (dotX < 0) ? -dotX : dotX;
                    float ay = (dotY < 0) ? -dotY : dotY;
                    dotX = 0.5f - ax * 0.25f - ax * ax * ax * 0.25f;
                    dotY = 0.5f - ay * 0.25f - ay * ay * ay * 0.25f;
                } else {
                    dotX = dotX * 0.5f + 0.5f;
                    dotY = dotY * 0.5f + 0.5f;
                }
                d->u = dotX * rsp.texture_scale_s * tg_mul;
                d->v = dotY * rsp.texture_scale_t * tg_mul;
            }
        }
    }
    PROF_END(sPF_Vtx);
}
#define recip255 0.00392157f
static void handle_light(u32 w0, u32 w1) {
    // F3DEX2 MOVEMEM encoding: bits[15:8] = ofs/8, bits[7:0] = idx
    // Light offsets: G_MVO_L0=48, G_MVO_L1=72, ... each 24 bytes apart
    // gSPLight(pkt, l, n) uses offset = n*24+24
    // gSPLookAt uses offsets 0 (X) and 24 (Y)
    int ofs = ((w0 >> 8) & 0xFF) * 8;
    int lightIdx = (ofs - 48) / 24;  // 0-based: L0→0, L1→1, ...

    void* data = pc_resolve_addr(w1);
    if (!data)
        return;

    u8* bytes = (u8*)data;

    // LookAt vectors: offset 0 = LookAtX, offset 24 = LookAtY
    if (ofs == 0) {
        // LookAt X — direction at bytes 8,9,10 (same layout as Light_t)
        sLookAtX[0] = (float)(s8)bytes[8];
        sLookAtX[1] = (float)(s8)bytes[9];
        sLookAtX[2] = (float)(s8)bytes[10];
        shz_vec3_t sLookAtXIdxVec = shz_vec3_normalize(shz_vec3_deref(sLookAtX));
        sLookAtX[0] = sLookAtXIdxVec.x;
        sLookAtX[1] = sLookAtXIdxVec.y;
        sLookAtX[2] = sLookAtXIdxVec.z;

        sLookAtValid = 1;
        
        return;
    }
    if (ofs == 24) {
        // LookAt Y
        sLookAtY[0] = (float)(s8)bytes[8];
        sLookAtY[1] = (float)(s8)bytes[9];
        sLookAtY[2] = (float)(s8)bytes[10];
        shz_vec3_t sLookAtYIdxVec = shz_vec3_normalize(shz_vec3_deref(sLookAtY));
        sLookAtY[0] = sLookAtYIdxVec.x;
        sLookAtY[1] = sLookAtYIdxVec.y;
        sLookAtY[2] = sLookAtYIdxVec.z;
        return;
    }

    if (lightIdx >= 0 && lightIdx < sNumLights && lightIdx < MAX_LIGHTS) {
        // Directional light
        sLights[lightIdx].r = bytes[0] * recip255;
        sLights[lightIdx].g = bytes[1] * recip255;
        sLights[lightIdx].b = bytes[2] * recip255;

        // Direction (signed bytes at offset 8, 9, 10 in Light_t)
        sLights[lightIdx].dir[0] = (float)(s8)bytes[8];
        sLights[lightIdx].dir[1] = (float)(s8)bytes[9];
        sLights[lightIdx].dir[2] = (float)(s8)bytes[10];
        shz_vec3_t sLightIdxVec = shz_vec3_normalize(shz_vec3_deref(sLights[lightIdx].dir));
        sLights[lightIdx].dir[0] = sLightIdxVec.x;
        sLights[lightIdx].dir[1] = sLightIdxVec.y;
        sLights[lightIdx].dir[2] = sLightIdxVec.z;
    } else if (lightIdx == sNumLights) {
        // Ambient light (placed right after the last directional light)
        sAmbient.r = bytes[0] * recip255;
        sAmbient.g = bytes[1] * recip255;
        sAmbient.b = bytes[2] * recip255;
    }
    sLightMtxDirty = 1;
}

static void handle_setgeometrymode(u32 w1) {
    sGeometryMode |= w1;
}

static void handle_cleargeometrymode(u32 w1) {
    sGeometryMode &= ~w1;
}

static void handle_setnum_lights(u32 w1) {
    // Number of directional lights
    sNumLights = (w1 / 24);
    if (sNumLights > MAX_LIGHTS)
        sNumLights = MAX_LIGHTS;
}

/* Fast emit for non-clipped triangles — reads pre-transformed data from LoadedVertex.
   No perspective divide, no viewport transform, no ClipVertex construction. */
/* --- Emit-invariant cache --- texture/tile/combiner/prim/env state is constant
   across many triangles, so derive the per-vertex constants ONCE per state change
   instead of per triangle (was: 2 divides + shift calc + a full get_combiner_color
   on EVERY triangle). sEmitCacheDirty is set by every handler that writes a dep. */
static float sEC_invTexW = 1.0f, sEC_invTexH = 1.0f;
static float sEC_uls = 0.0f, sEC_ult = 0.0f;
static float sEC_uShiftMul = 1.0f, sEC_vShiftMul = 1.0f;
static int sEC_shadeConst = 0;
static int sEC_combAffine = 0;         /* combiner is affine in shade: out = A*shade + B */
static float sEC_combA[4] = { 1, 1, 1, 1 };
static float sEC_combB[4] = { 0, 0, 0, 0 };
static u32 sEC_precompArgb = 0;
/* fog_oargb (the dual-purpose offset-color / vertex-fog-coeff field) is a
   per-state CONSTANT whenever sDoAdd != 0 (it packs env/prim color and never
   reads src->fog) — precompute once per state change instead of per vertex. */
static int sEC_fogConst = 0;
static u32 sEC_fogOargbConst = 0;
static void emit_cache_rebuild(void) {
    sEC_invTexW = shz_divf_fsrra(sCurrentTexUScale, (float)(sCurrentTexWidth > 0 ? sCurrentTexWidth : 1));
    sEC_invTexH = shz_divf_fsrra(sCurrentTexVScale, (float)(sCurrentTexHeight > 0 ? sCurrentTexHeight : 1));
    sEC_uls = sCurrentUls;
    sEC_ult = sCurrentUlt;
    int shifts = sTiles[0].shifts, shiftt = sTiles[0].shiftt;
    sEC_uShiftMul = 1.0f; sEC_vShiftMul = 1.0f;
    if (shifts > 0 && shifts < 11) sEC_uShiftMul = shz_invf_fsrra((float)(1 << shifts));
    else if (shifts >= 11) sEC_uShiftMul = (float)(1 << (16 - shifts));
    if (shiftt > 0 && shiftt < 11) sEC_vShiftMul = shz_invf_fsrra((float)(1 << shiftt));
    else if (shiftt >= 11) sEC_vShiftMul = (float)(1 << (16 - shiftt));
    sEC_shadeConst = !sCombinerState.uses_shade;
    if (sEC_shadeConst) {
        float dummy[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float combined[4];
        get_combiner_color(dummy, combined);
        u32 w0_comb = (u32)(sCombineMode >> 32);
        if (((w0_comb >> 0) & 0x1F) == 5) {
            combined[0] *= sEnvColor[0];
            combined[1] *= sEnvColor[1];
            combined[2] *= sEnvColor[2];
        }
        u8 cr = (uint8_t)(combined[0] * 255.0f);
        u8 cg = (uint8_t)(combined[1] * 255.0f);
        u8 cb = (uint8_t)(combined[2] * 255.0f);
        u8 ca = (uint8_t)(combined[3] * 255.0f);
        sEC_precompArgb = (ca << 24) | (cr << 16) | (cg << 8) | cb;
    }

    /* Precompute the combiner as affine in shade: out[c] = A[c]*shade[c] + B[c].
       get_combiner_color is per-channel affine for the vast majority of OoT
       combiners (texel*shade, prim/env lerps, etc.), so derive A,B from shade=0
       and shade=1, then verify at shade=0.5 (catches clamps / non-affine). Per
       vertex this becomes 4 madds instead of the full mux dispatch. */
    sEC_combAffine = 0;
    if (!sEC_shadeConst) {
        float s0[4] = { 0, 0, 0, 0 }, s1[4] = { 1, 1, 1, 1 }, sh[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
        float o0[4], o1[4], oh[4];
        get_combiner_color(s0, o0);
        get_combiner_color(s1, o1);
        get_combiner_color(sh, oh);
        const float E = 0.01f;
        int affine = 1;
        for (int c = 0; c < 4; c++) {
            float A = o1[c] - o0[c], B = o0[c];
            if (fabsf(A * 0.5f + B - oh[c]) > E) { affine = 0; break; }
            sEC_combA[c] = A; sEC_combB[c] = B;
        }
        if (affine) {
            sEC_combAffine = 1;
            /* env-mult post-step (combine mode == 5) scales RGB by env: fold in. */
            u32 w0_comb = (u32)(sCombineMode >> 32);
            if (((w0_comb >> 0) & 0x1F) == 5) {
                for (int c = 0; c < 3; c++) { sEC_combA[c] *= sEnvColor[c]; sEC_combB[c] *= sEnvColor[c]; }
            }
        }
    }
    /* fog_oargb hoist: in the sDoAdd!=0 modes the offset-color field is a pure
       per-state constant (env/prim color packed, no src->fog) — pack it once.
       sDoAdd is current here (get_combiner_color above derives it; its deps —
       combiner/env/prim — all invalidate this cache). sDoAdd==0 stays per-vertex
       (src->fog<<24). 3/6/10 are currently dead paths but kept for parity. */
    if (sDoAdd == 1 || sDoAdd == 2) {
        sEC_fogOargbConst = 0xff000000u | ((u32)(u8)(sEnvColor[0]*255.0f) << 16) |
                            ((u32)(u8)(sEnvColor[1]*255.0f) << 8) | (u32)(u8)(sEnvColor[2]*255.0f);
        sEC_fogConst = 1;
    } else if (sDoAdd == 3 || sDoAdd == 6) {
        sEC_fogOargbConst = 0xff000000u | ((u32)(u8)(sPrimColor[0]*255.0f) << 16) |
                            ((u32)(u8)(sPrimColor[1]*255.0f) << 8) | (u32)(u8)(sPrimColor[2]*255.0f);
        sEC_fogConst = 1;
    } else if (sDoAdd == 10) {
        sEC_fogOargbConst = 0xFFFF0000u;
        sEC_fogConst = 1;
    } else {
        sEC_fogConst = 0;
    }

    sEmitCacheDirty = 0;
}

static void emit_triangle_fast(LoadedVertex* v0, LoadedVertex* v1, LoadedVertex* v2) {
    pvr_ensure_header();
#if 0 /* DBG: dump routing/mode state once per distinct multitexture combine */
    if (sMultiTexEnabled) {
        static u64 sDbgLastMtxCombine = 0;
        if (sCombineMode != sDbgLastMtxCombine) {
            sDbgLastMtxCombine = sCombineMode;
            printf("MTEX combine=%08lx%08lx omodeL=%08lx | blend=%d atest=%d mtex=%d list=%d pt=%d tr=%d depthW=%d alphaTex=%d | prim_a=%.2f env_a=%.2f\n",
                   (unsigned long)(sCombineMode >> 32), (unsigned long)(sCombineMode & 0xFFFFFFFF),
                   (unsigned long)sOtherModeL, sBlendingEnabled, sAlphaTestEnabled, sMultiTexEnabled,
                   sPvrCurrentList, sPtBuffering, sTrBuffering, sDepthWriteEnabled,
                   sCombinerState.alpha_uses_texture, sPrimColor[3], sEnvColor[3]);
        }
    }
#endif
    if (sIsOrtho || (!sDepthTestEnabled && sPvrCurrentList == PVR_LIST_TR_POLY))
        s2DDepth += OVERLAY_Z_STEP;

    /* Per-state-change invariants, not per-triangle (see emit_cache_rebuild). */
    if (sEmitCacheDirty) emit_cache_rebuild();
    float inv_texW = sEC_invTexW;
    float inv_texH = sEC_invTexH;
    float uls = sEC_uls;
    float ult = sEC_ult;
    float u_shift_mul = sEC_uShiftMul;
    float v_shift_mul = sEC_vShiftMul;
    int shade_const = sEC_shadeConst;
    u32 precomputed_argb = sEC_precompArgb;

    LoadedVertex* srcs[3] = { v0, v1, v2 };
    for (int i = 0; i < 3; i++) {
        LoadedVertex* src = srcs[i];
        /* Damage-flash redirect: scale the captured per-object color by this
           vertex's fog coefficient and pack it into the post-texture offset color
           (PVR adds oargb after texturing: tex*shade + oargb). Header fog is forced
           off while the flag is set, so this is the only fog contribution and it's
           a per-object color instead of the global gray register. */
        uint32 fog_oargb;
        if (sDamageFlash) {
            float f = src->fog * (1.0f / 255.0f);
            fog_oargb = 0xff000000u
                      | ((u32)(u8)(sDamageR * f) << 16)
                      | ((u32)(u8)(sDamageG * f) << 8)
                      |  (u32)(u8)(sDamageB * f);
        } else {
            fog_oargb = sEC_fogConst ? sEC_fogOargbConst : (src->fog << 24);
        }

        u32 packed_color;
        if (shade_const) {
            packed_color = precomputed_argb;
        } else if (sEC_combAffine) {
            /* out = A*shade + B (precomputed per state) — 4 madds, not the mux eval. */
            float sr = src->color.r * (1.0f / 255.0f), sg = src->color.g * (1.0f / 255.0f);
            float sb = src->color.b * (1.0f / 255.0f), sa = src->color.a * (1.0f / 255.0f);
            float r = sEC_combA[0] * sr + sEC_combB[0];
            float g = sEC_combA[1] * sg + sEC_combB[1];
            float b = sEC_combA[2] * sb + sEC_combB[2];
            float a = sEC_combA[3] * sa + sEC_combB[3];
            r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
            g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
            b = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
            a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
            packed_color = ((u32)(u8)(a * 255.0f) << 24) | ((u32)(u8)(r * 255.0f) << 16) |
                           ((u32)(u8)(g * 255.0f) << 8) | (u32)(u8)(b * 255.0f);
        } else {
            float shade[4] = {
                src->color.r * (1.0f / 255.0f), src->color.g * (1.0f / 255.0f),
                src->color.b * (1.0f / 255.0f), src->color.a * (1.0f / 255.0f)
            };
            float combined[4];
            get_combiner_color(shade, combined);
//            if ((sPvrCurrentList != PVR_LIST_OP_POLY || sPtBuffering) && combined[3] <= 0.0f)
  //              combined[3] = 1.0f;
            u32 w0_comb = (u32)(sCombineMode >> 32);
            if (((w0_comb >> 0) & 0x1F) == 5) {
                combined[0] *= sEnvColor[0];
                combined[1] *= sEnvColor[1];
                combined[2] *= sEnvColor[2];
            }
            u8 cr = (uint8_t)(combined[0] * 255.0f);
            u8 cg = (uint8_t)(combined[1] * 255.0f);
            u8 cb = (uint8_t)(combined[2] * 255.0f);
            u8 ca = (uint8_t)(combined[3] * 255.0f);
            packed_color = (ca << 24) | (cr << 16) | (cg << 8) | cb;
            //if (sGeometryMode & G_FOG) {
              //  packed_color = 0xFFFF00FF;
            //}
        }
#if 1
        /* Morpha tentacle recolor: dark-blue translucent body + bright white
           highlights. PVR does rgb = TEXEL0(caustic)*packed_rgb + oargb, so set
           oargb = body color D and packed_rgb = (white - D): caustic 0 -> D,
           caustic 1 -> white. Alpha tracks caustic via the I8 texel (MODULATEALPHA),
           so the body is naturally translucent; MORPHA_BODY_ALPHA scales it overall.
           Gated on the tentacle's tile-0 texture (gMorphaWaterTex) so it can't catch
           other sDoAdd==2 blue-env surfaces (e.g. Link's spin-charge effect). The
           water body shares this texture but is sDoAdd==0, so it stays excluded. */
        if (sDoAdd == 2 && sTiles[0].texture &&
            pc_resolve_addr(sTiles[0].texture->addr) == pc_resolve_addr(gMorphaWaterTex)) {
            u8 a = (u8)(((packed_color >> 24) & 0xFF) * MORPHA_BODY_ALPHA);
            packed_color = ((u32)a << 24) | 0xffffff;
//                           ((u32)(255 - MORPHA_BODY_R) << 16) |
  //                         ((u32)(255 - MORPHA_BODY_G) << 8) |
    //                       (u32)(255 - MORPHA_BODY_B);
            fog_oargb = 0xff000000u | ((u32)MORPHA_BODY_R << 16) |
                        ((u32)MORPHA_BODY_G << 8) | (u32)MORPHA_BODY_B;
        }
#endif
        packed_color = vismono_argb(packed_color);
        fog_oargb = vismono_argb(fog_oargb);

        float u = (src->u - uls) * u_shift_mul;
        float v = (src->v - ult) * v_shift_mul;

        float pvr_z;
        if (sIsOrtho) {
            pvr_z = s2DDepth * 1e6f;
        } else if (!sDepthTestEnabled && sPvrCurrentList == PVR_LIST_TR_POLY) {
            /* No Z-test on TR list (e.g. transition overlays) — force to front
               so PVR autosort doesn't bury them behind scene TR geometry */
            pvr_z = s2DDepth * 1e6f;
        } else {
            pvr_z = src->inv_w;
            if (pvr_z < 0.0000001f) pvr_z = 0.0000001f;
        }
        if (sPolygonOffsetBias > 0.0f)
            pvr_z += pvr_z * sPolygonOffsetBias;
        u8 do_mtex = 0;
        u8 dbg_comb_a = (u8)((packed_color >> 24) & 0xFF);   /* true combiner alpha, pre-override */
        (void)dbg_comb_a;
        /* Buffer pass 2 vertex for multi-texture blend (tile 1) */
        if (sMultiTexEnabled && (sDrawingSkybox || sDepthWriteEnabled || (sCombinerState.alpha_uses_texture && sBlendingEnabled)) &&
            (sTiles[1].texDirty ? (resolve_tile_texture(1), 1) : 1) &&
            sTiles[1].texture != NULL && sMultiTexVertCount < MAX_MTX_VERTS) {
            do_mtex = 1;
            u8 mt_blend_a_inv;
            /* Use prim_lod_frac as blend alpha when combiner uses it,
                otherwise fall back to env_alpha */
            float blend_f = (sPrimLodFrac > 0.001f) ? sPrimLodFrac : sEnvColor[3];
            if (sDrawingSkybox) {
                blend_f = sPrimColor[3];
                pvr_z /= 1000.0f;
            }
            mt_blend_a_inv = 255 - (u8)(blend_f * 255.0f);
            packed_color = (packed_color & 0x00ffffff) | (mt_blend_a_inv << 24);
        }
#if 0 
//MORPHA_PROBE
        if (i == 0 && (sMultiTexEnabled || sDoAdd) && sEnvColor[2] > 0.9f && sEnvColor[0] < 0.05f) {
            static u32 sMorphaProbe = 0;
            if ((sMorphaProbe++ & 0x3F) == 0) {
                float blend_f = (sPrimLodFrac > 0.001f) ? sPrimLodFrac : sEnvColor[3];
                printf("MORPHA light=%d shade=(%d,%d,%d,%d) doadd=%d affine=%d "
                       "A=(%.2f,%.2f,%.2f) B=(%.2f,%.2f,%.2f) packed=%08lx "
                       "comb_a=%d do_mtex=%d blend_f=%.2f prim_a=%.2f env=(%.2f,%.2f,%.2f,%.2f)\n",
                       (sGeometryMode & G_LIGHTING) != 0,
                       src->color.r, src->color.g, src->color.b, src->color.a,
                       sDoAdd, sEC_combAffine,
                       sEC_combA[0], sEC_combA[1], sEC_combA[2],
                       sEC_combB[0], sEC_combB[1], sEC_combB[2],
                       (unsigned long)packed_color, dbg_comb_a, do_mtex, blend_f,
                       sPrimColor[3], sEnvColor[0], sEnvColor[1], sEnvColor[2], sEnvColor[3]);
            }
        }
#endif

        if (sPtBuffering && sPtVertCount < MAX_PT_VERTS) {
            MtxVert* mv = &sPtVerts[sPtVertCount++];
            mv->x = src->screen_x;
            mv->y = src->screen_y;
            mv->z = pvr_z;
            mv->u = u * inv_texW;
            mv->v = v * inv_texH;
            mv->argb = packed_color;
            mv->oargb = fog_oargb;
        } else if (sTrBuffering) {
            if (sTrVertCount < MAX_TR_VERTS) {
                MtxVert* mv = &sTrVerts[sTrVertCount++];
                mv->x = src->screen_x;
                mv->y = src->screen_y;
                mv->z = pvr_z;
                mv->u = u * inv_texW;
                mv->v = v * inv_texH;
                mv->argb = packed_color;
                mv->oargb = fog_oargb;
            }
            /* else: TR buffer full -> drop this vert (degraded frame; never OP). */
        } else {
            dc_fast_t* dst = (dc_fast_t *)pvr_dr_target(sDrState);
            dst->vert.x = src->screen_x;  /* already PVR-ready from VTX time */
            dst->vert.y = src->screen_y;
            dst->vert.z = pvr_z;
            if (pvr_z > max_z) max_z = pvr_z;
            dst->texture.u = u * inv_texW;
            dst->texture.v = v * inv_texH;
            dst->color.packed = packed_color;
            dst->oargb = fog_oargb;
            dst->flags = (i == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            pvr_dr_commit(dst);
            sPvrFrameBytes += 32;
        }

        /* Buffer pass 2 vertex for multi-texture blend (tile 1) */
        //if (sMultiTexEnabled && sDepthWriteEnabled &&
          //  (sTiles[1].texDirty ? (resolve_tile_texture(1), 1) : 1) &&
            //sTiles[1].texture != NULL && sMultiTexVertCount < MAX_MTX_VERTS) {
        if (do_mtex) {
            static float mt_inv_t1W, mt_inv_t1H;
            static float mt_uls1, mt_ult1;
            static float mt_u1_shift, mt_v1_shift;
            static u8 mt_blend_a;
            if (i == 0) {
                mtx_ensure_batch();
                int t1_shifts = sTiles[1].shifts;
                int t1_shiftt = sTiles[1].shiftt;
                mt_u1_shift = 1.0f;
                mt_v1_shift = 1.0f;
                if (t1_shifts > 0 && t1_shifts < 11)
                    mt_u1_shift = 1.0f / (float)(1 << t1_shifts);
                else if (t1_shifts >= 11)
                    mt_u1_shift = (float)(1 << (16 - t1_shifts));
                if (t1_shiftt > 0 && t1_shiftt < 11)
                    mt_v1_shift = 1.0f / (float)(1 << t1_shiftt);
                else if (t1_shiftt >= 11)
                    mt_v1_shift = (float)(1 << (16 - t1_shiftt));

                mt_uls1 = sTiles[1].uls / 4.0f - 0.5f;
                mt_ult1 = sTiles[1].ult / 4.0f - 0.5f;

                float t1_us, t1_vs;
                get_texture_uv_scale(sTiles[1].texture, &t1_us, &t1_vs);
                int t1_w = sTiles[1].width > 0 ? sTiles[1].width : 1;
                int t1_h = sTiles[1].height > 0 ? sTiles[1].height : 1;
                mt_inv_t1W = t1_us / (float)t1_w;
                mt_inv_t1H = t1_vs / (float)t1_h;

                /* Use prim_lod_frac as blend alpha when combiner uses it,
                   otherwise fall back to env_alpha */
                float blend_f = (sPrimLodFrac > 0.001f) ? sPrimLodFrac : sEnvColor[3];
                if (sDrawingSkybox)
                    blend_f = sPrimColor[3];
                mt_blend_a = (u8)(blend_f * 255.0f);
            }

            float mt_u = (src->u - mt_uls1) * mt_u1_shift;
            float mt_v = (src->v - mt_ult1) * mt_v1_shift;

            MtxVert* mv = &sMultiTexVerts[sMultiTexVertCount++];
            mv->x = src->screen_x;
            mv->y = src->screen_y;
            mv->z = pvr_z;
            mv->u = mt_u * mt_inv_t1W;
            mv->v = mt_v * mt_inv_t1H;
            mv->u0 = u * inv_texW;   /* tile 0 UVs for mask passes */
            mv->v0 = v * inv_texH;
            mv->argb = (mt_blend_a << 24) | (packed_color & 0x00FFFFFF);
            mv->oargb = fog_oargb;
        }
    }
}

/* Near-plane intersection of edge (front -> behind) into a screen-space spare
   LoadedVertex. Attributes lerp linearly by the clip-space parameter t (which is
   perspective-correct at the intersection point). cw == NEAR_PLANE by construction. */
static void clip_near_isect(int vFront, int vBehind, LoadedVertex* out) {
    const float* P = sClipSpace[vFront];
    const float* Q = sClipSpace[vBehind];
    const LoadedVertex* lp = &sVertexBuffer[vFront];
    const LoadedVertex* lq = &sVertexBuffer[vBehind];
    float t = shz_divf(NEAR_PLANE - P[3], Q[3] - P[3]);
    float cx = P[0] + t * (Q[0] - P[0]);
    float cy = P[1] + t * (Q[1] - P[1]);
    float cw = P[3] + t * (Q[3] - P[3]);
    float iw = shz_invf_fsrra(cw);
    out->screen_x = (cx * iw * sViewportScaleX + sViewportTransX) * SCREEN_SCALE;
    out->screen_y = (-cy * iw * sViewportScaleY + sViewportTransY) * SCREEN_SCALE;
    out->inv_w = iw;
    out->u = lp->u + t * (lq->u - lp->u);
    out->v = lp->v + t * (lq->v - lp->v);
    out->color.r = (u8)(lp->color.r + t * ((int)lq->color.r - (int)lp->color.r));
    out->color.g = (u8)(lp->color.g + t * ((int)lq->color.g - (int)lp->color.g));
    out->color.b = (u8)(lp->color.b + t * ((int)lq->color.b - (int)lp->color.b));
    out->color.a = (u8)(lp->color.a + t * ((int)lq->color.a - (int)lp->color.a));
    out->fog = (u8)(lp->fog + t * ((int)lq->fog - (int)lp->fog));
    out->behind = 0;
}

static void push_triangle(int v0, int v1, int v2) {
    PROF_BEGIN(sPF_Tri);
    sDbgTriIn++;
    if (v0 >= UCODE_MAX_VERTS || v1 >= UCODE_MAX_VERTS || v2 >= UCODE_MAX_VERTS || v0 < 0 || v1 < 0 || v2 < 0) {
        PROF_END(sPF_Tri);
        return;
    }

    LoadedVertex* vert0 = &sVertexBuffer[v0];
    LoadedVertex* vert1 = &sVertexBuffer[v1];
    LoadedVertex* vert2 = &sVertexBuffer[v2];

    uint8_t c0 = sVertexClipCodes[v0];
    uint8_t c1 = sVertexClipCodes[v1];
    uint8_t c2 = sVertexClipCodes[v2];

    // Trivial reject - all vertices outside same frustum plane
    // Only check X/Y planes (0x0F); Z handled by behindCount below
    if ((c0 & c1 & c2) & 0x0F) {
        PROF_END(sPF_Tri);
        return;
    }

    int behindCount = vert0->behind + vert1->behind + vert2->behind;
    // 
    if (behindCount == 3) {
        PROF_END(sPF_Tri);
        return;
    }

    if (behindCount == 0) {
        // --- Fast path: no clipping needed   ---
        // Backface cull using pre-transformed screen coords (no inv_w needed).
        // Screen-space cross has flipped sign vs NDC due to Y negation.
        if (sGeometryMode & (G_CULL_FRONT | G_CULL_BACK)) {
            float dx1 = vert0->screen_x - vert1->screen_x;
            float dy1 = vert0->screen_y - vert1->screen_y;
            float dx2 = vert2->screen_x - vert1->screen_x;
            float dy2 = vert2->screen_y - vert1->screen_y;
            float cross = dx1 * dy2 - dy1 * dx2;

            if ((sGeometryMode & G_CULL_BACK) && cross <= 0.0f) {
                PROF_END(sPF_Tri);
                return;
            }
            if ((sGeometryMode & G_CULL_FRONT) && cross >= 0.0f) {
                PROF_END(sPF_Tri);
                return;
            }
        }

        sDbgFastEmit++;
        if (sPtBuffering) sDbgPolyPT++;
        else if (sTrBuffering) sDbgPolyTR++;
        else if (sPvrCurrentList == PVR_LIST_OP_POLY) sDbgPolyOP++;
        else if (sPvrCurrentList == PVR_LIST_TR_POLY) sDbgPolyTR++;
        else sDbgPolyPT++;
        emit_triangle_fast(vert0, vert1, vert2);
    } else {
        sDbgClipEmit++;
        if (sPtBuffering) sDbgPolyPT++;
        else if (sTrBuffering) sDbgPolyTR++;
        else if (sPvrCurrentList == PVR_LIST_OP_POLY) sDbgPolyOP++;
        else if (sPvrCurrentList == PVR_LIST_TR_POLY) sDbgPolyTR++;
        else sDbgPolyPT++;
        /* Specialized single-triangle near-plane clip (one Sutherland-Hodgman
           pass). A triangle straddling the near plane produces at most 2 new
           intersection verts; the in-front originals pass through by pointer
           (never copied), and everything emits via the fast path. */
        int idx[3] = { v0, v1, v2 };
        int numOut = 0, sp = 0, prev = 2;
        int prevIn = !sVertexBuffer[idx[2]].behind;
        for (int i = 0; i < 3; i++) {
            int curIn = !sVertexBuffer[idx[i]].behind;
            if (prevIn && curIn) {
                sClipOutv[numOut++] = &sVertexBuffer[idx[i]];
            } else if (prevIn && !curIn) {
                clip_near_isect(idx[prev], idx[i], &sClipSpare[sp]);
                sClipOutv[numOut++] = &sClipSpare[sp++];
            } else if (!prevIn && curIn) {
                clip_near_isect(idx[i], idx[prev], &sClipSpare[sp]);
                sClipOutv[numOut++] = &sClipSpare[sp++];
                sClipOutv[numOut++] = &sVertexBuffer[idx[i]];
            }
            prev = i;
            prevIn = curIn;
        }
        if (numOut < 3) {
            PROF_END(sPF_Tri);
            return;
        }

        /* Backface cull in screen space (same test as the fast path; fan tris
           share winding, so cull once on the first). */
        if (sGeometryMode & (G_CULL_FRONT | G_CULL_BACK)) {
            float dx1 = sClipOutv[0]->screen_x - sClipOutv[1]->screen_x;
            float dy1 = sClipOutv[0]->screen_y - sClipOutv[1]->screen_y;
            float dx2 = sClipOutv[2]->screen_x - sClipOutv[1]->screen_x;
            float dy2 = sClipOutv[2]->screen_y - sClipOutv[1]->screen_y;
            float cross = dx1 * dy2 - dy1 * dx2;
            if ((sGeometryMode & G_CULL_BACK) && cross <= 0.0f) { PROF_END(sPF_Tri); return; }
            if ((sGeometryMode & G_CULL_FRONT) && cross >= 0.0f) { PROF_END(sPF_Tri); return; }
        }

        for (int i = 1; i < numOut - 1; i++)
            emit_triangle_fast(sClipOutv[0], sClipOutv[i], sClipOutv[i + 1]);
    }
    PROF_END(sPF_Tri);
}

static void handle_setcombine(u32 w0, u32 w1) {
    sEmitCacheDirty = 1;
    sCombineMode = ((u64)w0 << 32) | w1;
    parse_combiner();
    u32 ac_cc = sOtherModeL & 0x3;
    int cvg_cc = (sOtherModeL >> 12) & 0x1;
    int newAt = (ac_cc == 1) || (cvg_cc && sCombinerState.alpha_uses_texture);
    if (newAt != sAlphaTestEnabled) {
        sPvrTexDirty = 1;
        sPvrColDirty = 1;
        sAlphaTestEnabled = newAt;
    }
    if (sBlendingEnabled) {
        /* Blended geometry inside OPA can't blend on the PVR OP list, so defer it
           to the TR buffer (flushed at the OPA->XLU boundary). Past the boundary
           we're already on TR -> render live. */
        sPtBuffering = 0;
        if (sPvrCurrentList == PVR_LIST_OP_POLY) {
            if (!sTrBuffering) {
                flush_triangles();
                sTrBuffering = 1;
                sPvrNeedHeader = 1;
            }
        } else if (sTrBuffering) {
            sTrBuffering = 0;
            sPvrNeedHeader = 1;
        }
    } else if (sAlphaTestEnabled) {
        /* Alpha-test-only: buffer for PT list instead of switching to TR */
        if (sTrBuffering) { sTrBuffering = 0; sPvrNeedHeader = 1; }
        if (!sPtBuffering) {
            flush_triangles();
            sPtBuffering = 1;
            sPvrNeedHeader = 1;
        }
    } else {
        sPtBuffering = 0;
        if (sTrBuffering) { sTrBuffering = 0; sPvrNeedHeader = 1; }
    }
    if (sLensBracket) {
        /* Lens bracket: everything rides the deferred TR buffer as modifier-affected */
        sPtBuffering = 0;
        if (!sTrBuffering) { flush_triangles(); sTrBuffering = 1; sPvrNeedHeader = 1; }
    }
}

static void handle_othermode_apply_alpha(void) {
    u32 ac = sOtherModeL & 0x3;
    int cvg_x_alpha = (sOtherModeL >> 12) & 0x1;
    int newAlphaTest = (ac == 1) || (cvg_x_alpha && sCombinerState.alpha_uses_texture);
    if (newAlphaTest != sAlphaTestEnabled) {
        sPvrTexDirty = 1;
        sPvrColDirty = 1;
    }
    sAlphaTestEnabled = newAlphaTest;
    if (sBlendingEnabled) {
        /* Blended geometry inside OPA can't blend on the PVR OP list, so defer it
           to the TR buffer (flushed at the OPA->XLU boundary). Past the boundary
           we're already on TR -> render live. */
        sPtBuffering = 0;
        if (sPvrCurrentList == PVR_LIST_OP_POLY) {
            if (!sTrBuffering) {
                flush_triangles();
                sTrBuffering = 1;
                sPvrNeedHeader = 1;
            }
        } else if (sTrBuffering) {
            sTrBuffering = 0;
            sPvrNeedHeader = 1;
        }
    } else if (sAlphaTestEnabled) {
        if (sTrBuffering) { sTrBuffering = 0; sPvrNeedHeader = 1; }
        if (!sPtBuffering) {
            flush_triangles();
            sPtBuffering = 1;
            sPvrNeedHeader = 1;
        }
    } else {
        sPtBuffering = 0;
        if (sTrBuffering) { sTrBuffering = 0; sPvrNeedHeader = 1; }
    }
    if (sLensBracket) {
        /* Lens bracket: everything rides the deferred TR buffer as modifier-affected */
        sPtBuffering = 0;
        if (!sTrBuffering) { flush_triangles(); sTrBuffering = 1; sPvrNeedHeader = 1; }
    }
}

static void handle_setothermode_l(u32 w0, u32 w1) {
    int shift = (w0 >> 8) & 0xFF;
    int len = w0 & 0xFF;
    u32 mask = ((1 << len) - 1) << shift;
    sOtherModeL = (sOtherModeL & ~mask) | (w1 & mask);
    u32 zmode = (sOtherModeL >> 10) & 0x3;
    int force_bl = (sOtherModeL >> 14) & 0x1;
    int newBlend = (zmode == 2 || zmode == 3 || force_bl);
    int new_fog_mode = ((sOtherModeL >> 30) == G_BL_CLR_FOG) || ((sOtherModeL >> 30) == G_BL_A_FOG);

    if (new_fog_mode != sFogMode || newBlend != sBlendingEnabled) {
        sPvrTexDirty = 1;
        sPvrColDirty = 1;
    }

    sFogMode = new_fog_mode;
    sBlendingEnabled = newBlend;
    apply_depth_settings();
    flush_triangles();
    sPolygonOffsetBias = (zmode == 3) ? 0.01f : 0.0f;
    handle_othermode_apply_alpha();
}

static void handle_setothermode_h(u32 w0, u32 w1) {
    int shift = (w0 >> 8) & 0xFF;
    int len = w0 & 0xFF;
    u32 mask = ((1 << len) - 1) << shift;
    u32 prev = sOtherModeH;
    sOtherModeH = (sOtherModeH & ~mask) | (w1 & mask);
    sTlutType = (sOtherModeH >> 14) & 0x3;
    if ((sOtherModeH ^ prev) & (0x3 << 12))
        sPvrTexDirty = 1;
}

static void handle_rdpsetothermode(u32 w0, u32 w1) {
    u32 prevH = sOtherModeH;
    sOtherModeH = w0 & 0x00FFFFFF;
    sOtherModeL = w1;
    sTlutType = (sOtherModeH >> 14) & 0x3;
    if ((sOtherModeH ^ prevH) & (0x3 << 12))
        sPvrTexDirty = 1;
    u32 zmode = (sOtherModeL >> 10) & 0x3;
    int force_bl = (sOtherModeL >> 14) & 0x1;
    int newBlend = (zmode == 2 || zmode == 3 || force_bl);
    int new_fog_mode = ((sOtherModeL >> 30) == G_BL_CLR_FOG) || ((sOtherModeL >> 30) == G_BL_A_FOG);

    if (new_fog_mode != sFogMode || newBlend != sBlendingEnabled) {
        sPvrTexDirty = 1;
        sPvrColDirty = 1;
    }
    
    sBlendingEnabled = newBlend;
    apply_depth_settings();
    flush_triangles();
    sPolygonOffsetBias = (zmode == 3) ? 0.01f : 0.0f;
    handle_othermode_apply_alpha();
}

static void handle_settile(u32 w0, u32 w1) {
    sEmitCacheDirty = 1;
    int tile = (w1 >> 24) & 0x7;
    sTiles[tile].fmt = (w0 >> 21) & 0x7;
    sTiles[tile].siz = (w0 >> 19) & 0x3;
    sTiles[tile].line = (w0 >> 9) & 0x1FF;
    sTiles[tile].tmem = w0 & 0x1FF;
    sTiles[tile].palette = (w1 >> 20) & 0xF;
    sTiles[tile].cmt = (w1 >> 18) & 0x3;
    sTiles[tile].maskt = (w1 >> 14) & 0xF;
    sTiles[tile].shiftt = (w1 >> 10) & 0xF;
    sTiles[tile].cms = (w1 >> 8) & 0x3;
    sTiles[tile].masks = (w1 >> 4) & 0xF;
    sTiles[tile].shifts = w1 & 0xF;
    // N64 hardware: mask=0 means clamp regardless of cms/cmt.
    // 2^0 = 1 texel repeat region, so the HW effectively clamps.
    if (sTiles[tile].masks == 0)
        sTiles[tile].cms = G_TX_CLAMP;
    if (sTiles[tile].maskt == 0)
        sTiles[tile].cmt = G_TX_CLAMP;
    {
        u32 old_cmt = sTiles[0].texCmt;
        u32 old_cms = sTiles[0].texCms;
        if (tile == 0 && (sTiles[0].cmt != old_cmt || sTiles[0].cms != old_cms))
            sPvrTexDirty = 1;
    }
    if (tile != 7 && sTiles[7].tmem == sTiles[tile].tmem && sTiles[7].texAddr != 0) {
        sTiles[tile].texAddr = sTiles[7].texAddr;
        sTiles[tile].src_stride = sTiles[7].src_stride;
    } else if (sTiles[tile].texAddr == 0) {
        for (int j = 0; j < 8; j++) {
            if (j != tile && sTiles[j].tmem == sTiles[tile].tmem && sTiles[j].texAddr != 0) {
                sTiles[tile].texAddr = sTiles[j].texAddr;
                sTiles[tile].src_stride = sTiles[j].src_stride;
                break;
            }
        }
    }
}

static void handle_loadtlut(u32 w0, u32 w1) {
    int tile = (w1 >> 24) & 0x7;
    int count = ((w1 >> 14) & 0x3FF) + 1;
    void* palData = pc_resolve_addr(sCurTexAddr);
    int offset = sTiles[tile].tmem - 256;
    if (offset < 0) offset = 0;

    if (palData && (offset + count) <= 256) {
        /* SAFE palette dedup: skip the whole convert/checksum/re-resolve block when
           this load is byte-identical to the previous one (same content hash, same
           dest region, same TLUT type). sTLUT[], sTlutChecksum, and the bound CI
           texture are all still valid from the prior load. Content-hash (not pointer)
           keyed -> safe even though OoT mutates palettes in place. */
        {
            const u16* ph = (const u16*)palData;
            u32 h = 2166136261u;
            for (int i = 0; i < count; i++) h = (h ^ ph[i]) * 16777619u;
            sTlutCalls++;
            if (h == sLastTlutHash && offset == sLastTlutOffset &&
                count == sLastTlutCount && (int)sTlutType == sLastTlutType) {
                sTlutRedundant++;
                return;
            }
            sLastTlutHash = h;
            sLastTlutOffset = offset;
            sLastTlutCount = count;
            sLastTlutType = (int)sTlutType;
        }
        memcpy(sTlutSwapBuf, palData, count * 2);
        swap_bytes_64(sTlutSwapBuf, count * 2);
        for (int i=offset;i<offset+count;i++) {
            u16 tlut_val = __builtin_bswap16(sTlutSwapBuf[i]);
            if (sTlutType == 3)
                tlut_val = ia16_to_rgba16(tlut_val);
            else
                tlut_val = (tlut_val << 15) | (tlut_val >> 1);
            sTLUT[i] = tlut_val;
        }
        sTlutDirty = 1;

        if (sTiles[G_TX_RENDERTILE].fmt == G_IM_FMT_CI &&
            sTiles[G_TX_RENDERTILE].texAddr != 0 &&
            sTiles[G_TX_RENDERTILE].width > 0 &&
            sTiles[G_TX_RENDERTILE].height > 0) {
            sTlutChecksum = compute_tlut_checksum();
            sTlutDirty = 0;
            flush_triangles();
            sTiles[G_TX_RENDERTILE].texDirty = 0;
            TextureCacheEntry* tex = get_texture(
                sTiles[G_TX_RENDERTILE].texAddr,
                sTiles[G_TX_RENDERTILE].fmt,
                sTiles[G_TX_RENDERTILE].siz,
                sTiles[G_TX_RENDERTILE].width,
                sTiles[G_TX_RENDERTILE].height,
                sTiles[G_TX_RENDERTILE].line,
                sTiles[G_TX_RENDERTILE].texCms,
                sTiles[G_TX_RENDERTILE].texCmt,
                sTiles[G_TX_RENDERTILE].palette,
                sTiles[G_TX_RENDERTILE].src_stride);
            if (tex != NULL) {
                sTiles[G_TX_RENDERTILE].texture = tex;
                sCurrentboundTexture = tex;
                get_texture_uv_scale(tex, &sCurrentTexUScale, &sCurrentTexVScale);
            }
        }
    }
}

static void handle_loadtile(u32 w0, u32 w1) {
    sEmitCacheDirty = 1;
    int tile = (w1 >> 24) & 0x7;
    u32 uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
    u32 lrs = (w1 >> 12) & 0xFFF, lrt = w1 & 0xFFF;
    int uls_int = uls >> 2;
    int ult_int = ult >> 2;
    int w = ((lrs - uls) >> 2) + 1;
    int h = ((lrt - ult) >> 2) + 1;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 2048) w = 2048;
    if (h > 2048) h = 2048;

    u32 src_stride = sCurTexWidth;
    u32 bytes_per_pixel = 1;
    if (sTiles[tile].siz == G_IM_SIZ_16b) bytes_per_pixel = 2;
    else if (sTiles[tile].siz == G_IM_SIZ_32b) bytes_per_pixel = 4;
    else if (sTiles[tile].siz == G_IM_SIZ_4b) bytes_per_pixel = 0;

    u32 tile_offset;
    if (sTiles[tile].siz == G_IM_SIZ_4b)
        tile_offset = (ult_int * src_stride + uls_int) / 2;
    else
        tile_offset = (ult_int * src_stride + uls_int) * bytes_per_pixel;

    u32 tile_addr = sCurTexAddr + tile_offset;

    sTiles[tile].width = w;
    sTiles[tile].height = h;
    sTiles[tile].uls = uls;
    sTiles[tile].ult = ult;
    sTiles[tile].lrs = lrs;
    sTiles[tile].lrt = lrt;
    sTiles[tile].texAddr = tile_addr;
    sTiles[tile].src_stride = src_stride;
    sTiles[tile].texCms = sTiles[tile].cms;
    sTiles[tile].texCmt = sTiles[tile].cmt;
    sTiles[tile].texDirty = 1;

    if (tile == 7 && sTiles[7].tmem == sTiles[0].tmem &&
        (sTiles[0].texAddr == 0 ||
         (u32)(w * h) >= (u32)(sTiles[0].width * sTiles[0].height))) {
        sTiles[0].texAddr = tile_addr;
        sTiles[0].src_stride = src_stride;
        sTiles[0].width = w;
        sTiles[0].height = h;
        sTiles[0].uls = uls;
        sTiles[0].ult = ult;
        sTiles[0].texCms = sTiles[tile].texCms;
        sTiles[0].texCmt = sTiles[tile].texCmt;
        sTiles[0].texDirty = 1;
        sCurrentUls = uls / 4.0f - 0.5f;
        sCurrentUlt = -0.5f;
        flush_triangles();
    }
}

static void handle_settilesize(u32 w0, u32 w1) {
    sEmitCacheDirty = 1;
    int tile = (w1 >> 24) & 0x7;
    u32 uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
    u32 lrs = (w1 >> 12) & 0xFFF, lrt = w1 & 0xFFF;
    int w = (((int)lrs - (int)uls) >> 2) + 1;
    int h = (((int)lrt - (int)ult) >> 2) + 1;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 2048) w = 2048;
    if (h > 2048) h = 2048;

    int tex_w = w, tex_h = h;
    u32 eff_cms = sTiles[tile].cms;
    u32 eff_cmt = sTiles[tile].cmt;
    if (sTiles[tile].masks == 0)
        eff_cms = G_TX_CLAMP;
    if (sTiles[tile].maskt == 0)
        eff_cmt = G_TX_CLAMP;
    if (sTiles[tile].masks > 0) {
        int mask_w = 1 << sTiles[tile].masks;
        if (mask_w < tex_w) {
            tex_w = mask_w;
            eff_cms &= ~G_TX_CLAMP;
        }
    }
    if (sTiles[tile].maskt > 0) {
        int mask_h = 1 << sTiles[tile].maskt;
        if (mask_h < tex_h) {
            tex_h = mask_h;
            eff_cmt &= ~G_TX_CLAMP;
        }
    }

    sTiles[tile].width = tex_w;
    sTiles[tile].height = tex_h;
    sTiles[tile].uls = uls;
    sTiles[tile].ult = ult;
    sTiles[tile].lrs = lrs;
    sTiles[tile].lrt = lrt;
    sTiles[tile].texCms = eff_cms;
    sTiles[tile].texCmt = eff_cmt;
    sTiles[tile].texDirty = 1;
    if (tile == G_TX_RENDERTILE) {
        sCurrentTexWidth = tex_w;
        sCurrentTexHeight = tex_h;
        sCurrentUls = uls / 4.0f - 0.5f;
        sCurrentUlt = ult / 4.0f - 0.5f;
        flush_triangles();
    }
}

static void handle_loadblock(u32 w0, u32 w1) {
    int tile = (w1 >> 24) & 0x7;
    u32 texels = ((w1 >> 12) & 0xFFF) + 1;

    sTiles[tile].texAddr = sCurTexAddr;
    sTiles[tile].fmt = sCurTexFmt;
    sTiles[tile].siz = sCurTexSiz;
    sTiles[tile].width = sCurTexWidth;
    sTiles[tile].src_stride = 0;
    sTiles[tile].texCms = sTiles[tile].cms;
    sTiles[tile].texCmt = sTiles[tile].cmt;
    sTiles[tile].texDirty = 1;

    if (sCurTexWidth > 0) {
        sTiles[tile].height = texels / sCurTexWidth;
        if (sTiles[tile].height < 1) sTiles[tile].height = 1;
    } else {
        sTiles[tile].height = 1;
    }

    if (tile == 7 && sTiles[7].tmem == sTiles[0].tmem) {
        sTiles[0].texAddr = sCurTexAddr;
        sTiles[0].fmt = sCurTexFmt;
        sTiles[0].siz = sCurTexSiz;
        sTiles[0].width = sCurTexWidth;
        sTiles[0].height = sTiles[tile].height;
        sTiles[0].src_stride = 0;
        sTiles[0].texCms = sTiles[tile].texCms;
        sTiles[0].texCmt = sTiles[tile].texCmt;
        sTiles[0].texDirty = 1;
    }
}

static void handle_fillrect(u32 w0, u32 w1) {
    PROF_BEGIN(sPF_Draw2D);
    flush_triangles();
    u32 xl = (w1 >> 12) & 0xFFF;
    u32 yl = w1 & 0xFFF;
    u32 xh = (w0 >> 12) & 0xFFF;
    u32 yh = w0 & 0xFFF;
    float x0 = xl / 4.0f * SCREEN_SCALE;
    float y0 = yl / 4.0f * SCREEN_SCALE;
    float x1 = xh / 4.0f * SCREEN_SCALE;
    float y1 = yh / 4.0f * SCREEN_SCALE;
    
    float combined[4] ;
    // jn64 -- DOUBLECHECK
    get_combiner_color(sFillColor, combined);
    uint8_t r = (uint8_t)(combined[0] * 255.0f);
    uint8_t g = (uint8_t)(combined[1] * 255.0f);
    uint8_t b = (uint8_t)(combined[2] * 255.0f);
    uint8_t a = (uint8_t)(combined[3] * 255.0f);
    u32 cycleType = (sOtherModeH >> 20) & 0x3;
    if (cycleType == 3) { /* G_CYC_FILL - LR corner is inclusive on N64 */
        x1 += SCREEN_SCALE;
        y1 += SCREEN_SCALE;
        r = (uint8_t)(sFillColor[0] * 255.0f);
        g = (uint8_t)(sFillColor[1] * 255.0f);
        b = (uint8_t)(sFillColor[2] * 255.0f);
        a = (uint8_t)(sFillColor[3] * 255.0f);
    }

    {
        u32 _fc = vismono_argb(((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | b);
        r = (_fc >> 16) & 0xFF; g = (_fc >> 8) & 0xFF; b = _fc & 0xFF;
    }

    if (sDoAdd == 3 || a > 0) {
        s2DDepth += OVERLAY_Z_STEP;
        /* Only boost Z for fill-cycle rects on the TR list (letterbox).
           TR autosort would otherwise push them behind 3D geometry.
           On OP list, draw order is correct so no boost needed. */
        float z = (cycleType == 3 && sPvrCurrentList == PVR_LIST_TR_POLY) ? s2DDepth * 1e4f : s2DDepth;
        pvr_submit_quad_col(x0, y0, x1 + 2.0f * SCREEN_SCALE, y1 + 2.0f * SCREEN_SCALE, z,
            (a << 24) | (r << 16) | (g << 8) | b);
    }
    PROF_END(sPF_Draw2D);
}

static TexHandle sBgCachedTexID = 0;
static u32 sBgCachedAddr = 0;
static float sBgCachedUScale = 1.0f;
static float sBgCachedVScale = 1.0f;
static int sBgCachedPW = 0, sBgCachedPH = 0;

static void handle_bg(u32 w0, u32 w1) {
    PROF_BEGIN(sPF_Draw2D);

    uObjBg* bg = (uObjBg*)pc_resolve_addr(w1);
    if (!bg) {
        PROF_END(sPF_Draw2D);
        return;
    }
    int imgW = bg->b.imageW >> 2;
    int imgH = bg->b.imageH >> 2;
    u32 fmt = bg->b.imageFmt;
    u32 siz = bg->b.imageSiz;

    TexHandle texID = 0;
    float u_scale = 1.0f, v_scale = 1.0f;
    int bg_pw = 8, bg_ph = 8;

    if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b) {
        u32 srcAddr = (u32)(uintptr_t)bg->b.imagePtr;

        if (sBgCachedTexID && sBgCachedAddr == srcAddr) {
            texID = sBgCachedTexID;
            u_scale = sBgCachedUScale;
            v_scale = sBgCachedVScale;
            bg_pw = sBgCachedPW;
            bg_ph = sBgCachedPH;
        } else {
            void* src_data = pc_resolve_addr(srcAddr);
            if (!src_data) { PROF_END(sPF_Draw2D); return; }

            u16* pixels = (u16*)src_data;
            u32 pw = 1;
            while (pw < (u32)imgW) pw <<= 1;
            u32 ph = 1;
            while (ph < (u32)imgH) ph <<= 1;
            if (pw > 512) pw = 512;
            if (ph > 512) ph = 512;

            u_scale = (float)imgW / (float)pw;
            v_scale = (float)imgH / (float)ph;

            if (pw * ph > TEXBUF_TEXELS) {
                if (sOomLogged < 64) { sOomLogged++; printf("PVROOM: bg %ux%u exceeds texbuf\n", (unsigned)pw, (unsigned)ph); }
                PROF_END(sPF_Draw2D);
                return;
            }
            u16* tex_data = sTexDataBuf;
            for (u32 i = 0; i < (u32)(imgW * imgH); i++) {
                u16 col16 = pixels[i];
                tex_data[i] = ((col16 & 1) << 15) | (col16 >> 1);
            }

            vismono_bake_texels(tex_data, (u32)(imgW * imgH), PVR_TXRFMT_ARGB1555);

            if ((u32)imgW != pw || (u32)imgH != ph)
                pad_texture_to_pow2(tex_data, imgW, imgH, pw, ph);

            if (sBgCachedTexID) pvr_mem_free(sBgCachedTexID);
            pvr_ptr_t pvr_tex = pvr_mem_malloc(pw * ph * 2);
            if (!pvr_tex) { pvrOOM = 1; if (sOomLogged < 64) { sOomLogged++; printf("PVROOM: 2D/bg alloc %u B failed\n", (unsigned)(pw * ph * 2)); } PROF_END(sPF_Draw2D); return; }
            /* TWIDDLED to match the (now-twiddled) header at the cxt below — pw/ph
               are pow2, and this only re-uploads when the background changes. */
            pvr_txr_load_ex(tex_data, pvr_tex, pw, ph, PVR_TXRLOAD_16BPP);
            texID = pvr_tex;
            bg_pw = pw;
            bg_ph = ph;
            sBgCachedPW = pw;
            sBgCachedPH = ph;
            sBgCachedTexID = texID;
            sBgCachedAddr = srcAddr;
            sBgCachedUScale = u_scale;
            sBgCachedVScale = v_scale;
        }
    } else {
        TextureCacheEntry* entry = get_texture((u32)(uintptr_t)bg->b.imagePtr, fmt, siz, imgW, imgH, imgW, G_TX_CLAMP,
                            G_TX_CLAMP, 0, 0);
        if (!entry) {
            PROF_END(sPF_Draw2D);
            return;
        }

        get_texture_uv_scale(entry, &u_scale, &v_scale);
        {
            bg_pw = entry->pow2Info.padded_w;
            bg_ph = entry->pow2Info.padded_h;
        }
    }

    flush_triangles();

    {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, sPvrCurrentList,
                          PVR_TXRFMT_ARGB1555,// | PVR_TXRFMT_NONTWIDDLED,
                          bg_pw, bg_ph,
                          texID,
                          pvr_tex_filter());
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;//ENABLE;
        pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
        pvr_poly_compile(&hdr, &cxt);
        SHZ_PREFETCH(&hdr);
        shz_sq_memcpy32_1(pvr_dr_target(sDrState), &hdr);
        sPvrFrameBytes += 32;
        pvr_submit_quad_tex(0, 0, DC_FB_W, DC_FB_H, 0.0001f, 0, 0, u_scale, v_scale, 0xFFFFFFFF, 0);
    }
    PROF_END(sPF_Draw2D);
}

float gl_fog_start = 0.0f;
float gl_fog_end = 0.0f;

// the following isn't very rigorous
// I eyeballed it and tweaked the `a` param until it seemed almost right
// and still need to scale it more per-level in `gfx_gldc` when setting gl fog params
static inline float exp_map_0_1000_f(float x) {
    const float a = 138.62943611198894f;
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1000.0f)
        return x * 1.01f; // 1000.0f;

    const float t = x * 0.001f;
    const float num = expm1f(a * (t - 1.0f));
    const float den = expm1f(-a);
    return 1000.0f * (1.0f - shz_divf(num, den));
}

static void walk_dl(Gfx* dl, int depth) {
    __builtin_prefetch(dl);

    if (!dl || depth > 16)
        return;

    /* A malformed branch target (a G_DL/G_BRANCH_Z whose resolved address is
       misaligned) makes the first `dl[i].words.w0` read fault with a CPU address
       error (SH4 evt 0x0E0). Gfx is 8 bytes / 4-aligned, so a non-4-aligned dl is
       garbage — skip it and log the culprit once instead of crashing. (Guard lives
       at entry so it covers every call site with zero per-command cost.) */
    if (((uintptr_t)dl & 3) != 0) {
        static void* sLastBadDl = NULL;
        if (dl != sLastBadDl) {
            sLastBadDl = dl;
            printf("walk_dl: misaligned DL %p (depth=%d) — skipped\n", (void*)dl, depth);
        }
        return;
    }

    for (int i = 0; ; i++) {
        u32 w0 = dl[i].words.w0;
        u8 cmd = (w0 >> 24) & 0xFF;

        /* Fast-skip N64 sync/noop commands — ~1600/frame, pure overhead on DC */
        if (cmd == G_RDPPIPESYNC || cmd == G_RDPLOADSYNC ||
            cmd == G_RDPTILESYNC || 
            cmd == G_RDPFULLSYNC ||
            cmd == G_NOOP || cmd == G_SETCIMG || cmd == G_SETZIMG)
            continue;

        u32 w1 = dl[i].words.w1;
        void* target;

        /* Custom DC-only DL op: "BLND" sentinel (full-word w0, never a real GBI
           command). w1 selects the sub-op. "DFLS" toggles the damage-flash fog-
           color redirect; on the rising edge, arm capture of the next fog color. */
        if (w0 == 0x424C4E44u) {
            if (w1 == 0x44464C53u) {
                sDamageFlash ^= 1;
                if (sDamageFlash) sDamageCapture = 1;
                /* fog_type lives in the poly header — force a rebuild so the flag's
                   transition (fog off entering, on again leaving) actually lands. */
                sPvrColDirty = 1;
                sPvrTexDirty = 1;
                sPvrNeedHeader = 1;
            } else if (w1 == 0x4C4E5331u) {
                /* 'LNS1': lens-actor bracket begin (only if a mask armed the circle) */
                if (sLensMaskSeen) {
                    flush_triangles();
                    sPtBuffering = 0;
                    sTrBuffering = 1;
                    sLensBracket = 1;
                    sPvrNeedHeader = 1;
                }
            } else if (w1 == 0x4C4E5330u) {
                /* 'LNS0': lens-actor bracket end */
                if (sLensBracket) {
                    flush_triangles();
                    sLensBracket = 0;
                    sPvrNeedHeader = 1;
                }
            }
            continue;
        }

        switch (cmd) {
            case G_VTX:
                get_combiner_color(global_dummy, global_combined);
                handle_vtx(w0, w1);
                break;
            case G_TRI1:
                push_triangle(((w0 >> 16) & 0xFF) >> 1, ((w0 >> 8) & 0xFF) >> 1, (w0 & 0xFF) >> 1);
                break;
            case G_TRI2:
                push_triangle(((w0 >> 16) & 0xFF) >> 1, ((w0 >> 8) & 0xFF) >> 1, (w0 & 0xFF) >> 1);
                push_triangle(((w1 >> 16) & 0xFF) >> 1, ((w1 >> 8) & 0xFF) >> 1, (w1 & 0xFF) >> 1);
                break;
            case G_QUAD:
                push_triangle(((w0 >> 16) & 0xFF) >> 1, ((w0 >> 8) & 0xFF) >> 1, (w0 & 0xFF) >> 1);
                push_triangle(((w1 >> 16) & 0xFF) >> 1, ((w1 >> 8) & 0xFF) >> 1, (w1 & 0xFF) >> 1);
                break;
            case G_MTX:
                handle_mtx(w0, w1);
                break;
            case G_POPMTX:
                handle_popmtx(w1);
                break;
            case G_GEOMETRYMODE: {
                flush_triangles();
                u32 prevCull = sGeometryMode & (G_CULL_FRONT | G_CULL_BACK);
                u32 prevFog = sGeometryMode & (G_FOG);
                handle_cleargeometrymode(~w0);
                handle_setgeometrymode(w1);
                if ((sGeometryMode & (G_CULL_FRONT | G_CULL_BACK)) != prevCull) {
                    sPvrColDirty = 1;
                    sPvrTexDirty = 1;
                }
                if ((sGeometryMode & (G_FOG)) != prevFog) {
                    sPvrColDirty = 1;
                    sPvrTexDirty = 1;
                }
                break;
            }
            case G_MOVEMEM: {
                int idx = w0 & 0xFF;
                if (idx == G_MV_VIEWPORT) {
                    Vp* vp = (Vp*)pc_resolve_addr(w1);
                    if (vp) {
                        flush_triangles();
                        sViewportScaleX = vp->vp.vscale[0] / 4.0f;
                        sViewportScaleY = vp->vp.vscale[1] / 4.0f;
                        sViewportTransX = vp->vp.vtrans[0] / 4.0f;
                        sViewportTransY = vp->vp.vtrans[1] / 4.0f;
                    }
                } else if (idx == G_MV_LIGHT) {
                    handle_light(w0, w1);
                }
            } break;
            case G_SETSCISSOR: {
                flush_triangles();
                u32 xl = (w0 >> 12) & 0xFFF;
                u32 yl = w0 & 0xFFF;
                u32 xh = (w1 >> 12) & 0xFFF;
                u32 yh = w1 & 0xFFF;
                int x = xl / 4;
                int y = yl / 4;
                int width = (xh / 4) - x;
                int height = (yh / 4) - y;
                /* PVR user tile clip - 32px tile granularity */
                sPvrScisX1 = x * SCREEN_SCALE_I; sPvrScisY1 = y * SCREEN_SCALE_I;
                sPvrScisX2 = (x + width) * SCREEN_SCALE_I; sPvrScisY2 = (y + height) * SCREEN_SCALE_I;
                pvr_submit_user_clip(sPvrScisX1, sPvrScisY1, sPvrScisX2, sPvrScisY2);
            } break;
            case G_MOVEWORD:
                if (((w0 >> 16) & 0xFF) == G_MW_SEGMENT) {
                    int seg = (w0 & 0xFFFF) >> 2;
                    if (seg < 16)
                        gSegments[seg] = w1;
                } else if (((w0 >> 16) & 0xFF) == G_MW_NUMLIGHT) {
                    handle_setnum_lights(w1);
                } else if (((w0 >> 16) & 0xFF) == G_MW_FOG) {
                    sFogMul = (int16_t) (w1 >> 16);
                    sFogOfs = (int16_t) w1;
                    sFogChanged = 1;
#if 0
                    {   /* DIAG: log fog mul/ofs when it changes */
                        static int16_t lastMul = 0x7fff, lastOfs = 0x7fff;
                        if (sFogMul != lastMul || sFogOfs != lastOfs) {
                            lastMul = sFogMul; lastOfs = sFogOfs;
                            printf("FOG mul=%d ofs=%d\n", (int)sFogMul, (int)sFogOfs);
                        }
                    }
#endif
                }
                break;
            case G_DL:
                target = pc_resolve_addr(w1);
                __builtin_prefetch(target);
                /* OPA->XLU boundary = branch into the XLU buffer. Switch OP->TR
                   once; flush the blends deferred from OPA (shadows etc.) so they
                   composite first on TR (matches N64 OPA-before-XLU order); then
                   stop deferring so XLU blends render live on TR. */
                if (gXluDLStart && target == (void*)gXluDLStart) {
                    pvr_ensure_tr_list();
                    flush_deferred_tr();
                    sTrBuffering = 0;
                }
                if (target)
                    walk_dl((Gfx*)target, depth + 1);
                if ((w0 >> 16) & 0xFF)
                    return;
                break;
            case G_ENDDL:
                return;
            case G_CULLDL: {
                /* AND all clip codes in vertex range — if non-zero, all verts
                   are outside the same frustum plane → skip rest of this DL */
                int vstart = (w0 & 0xFFFF) >> 1;
                int vend = (w1 & 0xFFFF) >> 1;
                if (vstart < UCODE_MAX_VERTS && vend < UCODE_MAX_VERTS && vend >= vstart) {
                    uint8_t andCode = 0xFF;
                    for (int ci = vstart; ci <= vend; ci++)
                        andCode &= sVertexClipCodes[ci];
                    if (andCode & 0x0F) // why did this change at one point
                        return;
                }
            } break;
            case G_RDPHALF_1:
                sRdpHalf1 = w1;
                break;
            case G_RDPHALF_2:
                break;
            case G_BRANCH_Z:
                target = pc_resolve_addr(sRdpHalf1);
                __builtin_prefetch(target);
                if (target)
                    walk_dl((Gfx*)target, depth + 1);
                return;
            case G_TEXRECT:
            case G_TEXRECTFLIP:
                if (i + 2 < 5000) {
                    u32 h1w0 = dl[i + 1].words.w0, h1w1 = dl[i + 1].words.w1;
                    u32 h2w0 = dl[i + 2].words.w0, h2w1 = dl[i + 2].words.w1;
                    if (((h1w0 >> 24) & 0xFF) == G_RDPHALF_1 && ((h2w0 >> 24) & 0xFF) == G_RDPHALF_2) {
                        draw_texrect(w0, w1, h1w1, h2w1, cmd == G_TEXRECTFLIP);
                        i += 2;
                    } else {
                        draw_texrect(w0, w1, h1w0, h1w1, cmd == G_TEXRECTFLIP);
                        i += 1;
                    }
                }
                break;
            case G_TEXTURE: {
                rsp.texture_scale_s = ((w1 >> 16) & 0xFFFF) * 0.03125f;
                rsp.texture_scale_t = (w1 & 0xFFFF) * 0.03125f;
                int texOn = (w0 >> 1) & 1;
                if (!texOn) {
                    flush_triangles();
                    sCurrentboundTexture = NULL;
                    sPvrColDirty = 1;
                }
            } break;
            case G_FILLRECT:
                handle_fillrect(w0, w1);
                break;
            case G_SETTIMG:
                sCurTexFmt = (w0 >> 21) & 0x7;
                sCurTexSiz = (w0 >> 19) & 0x3;
                sCurTexWidth = (w0 & 0xFFF) + 1;
                sCurTexAddr = w1;
                break;
            case G_SETPRIMCOLOR:
                sEmitCacheDirty = 1;
                sPrimLodFrac = (w0 & 0xFF) / 255.0f;  // lodfrac from bits[7:0]
                sPrimColor[0] = ((w1 >> 24) & 0xFF) / 255.0f;
                sPrimColor[1] = ((w1 >> 16) & 0xFF) / 255.0f;
                sPrimColor[2] = ((w1 >> 8) & 0xFF) / 255.0f;
                sPrimColor[3] = (w1 & 0xFF) / 255.0f;
                get_combiner_color(global_dummy, global_combined);
                break;
            case G_SETENVCOLOR:
                sEmitCacheDirty = 1;
                sEnvColor[0] = ((w1 >> 24) & 0xFF) / 255.0f;
                sEnvColor[1] = ((w1 >> 16) & 0xFF) / 255.0f;
                sEnvColor[2] = ((w1 >> 8) & 0xFF) / 255.0f;
                sEnvColor[3] = (w1 & 0xFF) / 255.0f;
                if (sEnvColor[0])
                get_combiner_color(global_dummy, global_combined);
                break;
            case G_SETFILLCOLOR:
                uint16_t col16 = (uint16_t) w1;
                uint32_t r = (col16 >> 11) & 0x1f;
                uint32_t g = (col16 >> 6) & 0x1f;
                uint32_t b = (col16 >> 1) & 0x1f;
                uint32_t a = col16 & 1;
                sFillColor[0] = r / 31.0f;
                sFillColor[1] = g / 31.0f;
                sFillColor[2] = b / 31.0f;
                sFillColor[3] = (float)a;
                break;
            case G_SETFOGCOLOR:
                if (sDamageCapture) {
                    /* Grab this object's own fog color (e.g. Link's red flash)
                       for the per-vertex bake; bypasses the global-register gate
                       so it doesn't recolor the rest of the scene. */
                    sDamageR = (w1 >> 24) & 0xFF;
                    sDamageG = (w1 >> 16) & 0xFF;
                    sDamageB = (w1 >>  8) & 0xFF;
                    sDamageCapture = 0;
                }
#if 0
                {   /* DIAG: log every distinct fog color + whether it's applied
                       (the per-frame gate drops all but the first). */
                    static u32 lastCol = 0xFFFFFFFF;
                    if (w1 != lastCol) {
                        lastCol = w1;
                        printf("FOGCOL r=%d g=%d b=%d a=%d applied=%d\n",
                               (int)((w1 >> 24) & 0xFF), (int)((w1 >> 16) & 0xFF),
                               (int)((w1 >> 8) & 0xFF), (int)(w1 & 0xFF), (int)!sFogColChanged);
                    }
                }
#endif
                if (!sFogColChanged) {
                    sFogColor[0] = (((w1 >> 24) & 0xFF) / 255.0f);
                    sFogColor[1] = (((w1 >> 16) & 0xFF) / 255.0f);
                    sFogColor[2] = (((w1 >>  8) & 0xFF) / 255.0f);
                    sFogColor[3] = (w1 & 0xFF) / 255.0f;
                    {
                        u32 _fog = vismono_argb(0xFF000000u |
                            ((u32)(u8)(sFogColor[0] * 255.0f) << 16) |
                            ((u32)(u8)(sFogColor[1] * 255.0f) << 8) |
                             (u32)(u8)(sFogColor[2] * 255.0f));
                        pvr_vertfog_color(sFogColor[3], ((_fog >> 16) & 0xFF) * (1.0f / 255.0f),
                                          ((_fog >> 8) & 0xFF) * (1.0f / 255.0f),
                                          (_fog & 0xFF) * (1.0f / 255.0f));
                    }
                    sFogColChanged = 1;
                    sPvrColDirty = 1;
                    sPvrTexDirty = 1;
                    sPvrNeedHeader = 1;
                }
                break;
            case G_SETCOMBINE:
                TIMED(sWalkCombineNs, handle_setcombine(w0, w1));
                TIMED(sWalkCombineNs, get_combiner_color(global_dummy, global_combined));
                break;
            case G_SETOTHERMODE_L:
                TIMED(sWalkOmodeNs, handle_setothermode_l(w0, w1));
                break;
            case G_SETOTHERMODE_H:
                TIMED(sWalkOmodeNs, handle_setothermode_h(w0, w1));
                break;
            case G_RDPSETOTHERMODE:
                TIMED(sWalkOmodeNs, handle_rdpsetothermode(w0, w1));
                break;
            case G_SETTILE:
                TIMED(sWalkTexNs, handle_settile(w0, w1));
                break;
            case G_LOADTLUT:
                TIMED(sWalkTexNs, handle_loadtlut(w0, w1));
                break;
            case G_BG_1CYC:
            case G_BG_COPY:
                handle_bg(w0, w1);
                break;
            case G_LOADTILE:
                TIMED(sWalkTexNs, handle_loadtile(w0, w1));
                break;
            case G_SETTILESIZE:
                TIMED(sWalkTexNs, handle_settilesize(w0, w1));
                break;
            case G_LOADBLOCK:
                TIMED(sWalkTexNs, handle_loadblock(w0, w1));
                break;
            case G_SETPRIMDEPTH:
                /* tracked for the Lens of Truth depth-mask texrect (0 = nearest) */
                sPrimDepthZ = (w1 >> 16) & 0xFFFF;
                break;
        }

        __builtin_prefetch((void*)&dl[i] + 32);
    }
}
bool profiler_stop();
void profiler_clean_up();
static int check_exit(void) {
    maple_device_t* cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (cont) {
        cont_state_t* state = (cont_state_t*)maple_dev_status(cont);
        if (state && (state->buttons & CONT_START) && (state->buttons & CONT_Y)) {
//            profiler_stop();
  //          profiler_clean_up();
            return 1;
        }
    }
    return 0;
}

void pc_process_displaylist(Gfx* dl) {
    if (check_exit()) {
        printf("Exiting...\n");
        exit(0);
    }

    uint64 frameStart = timer_ns_gettime64();

    if (pvrOOM) {
        pvrOOM = 0;
        printf("TEXWIPE: pvrOOM end-of-frame\n");
        flush_texture_cache_external();
    }

    /* VisMono toggle/fade: rebake everything through the transform. Naive:
       a ramping alpha (sepia cmd) re-wipes every frame -- revisit if slow. */
    if (sVisMonoWant != sVisMonoCur) {
        static u32 sVisMonoLog = 0;
        sVisMonoCur = sVisMonoWant;
        flush_texture_cache_external();
        if (sBgCachedTexID) { pvr_mem_free(sBgCachedTexID); sBgCachedTexID = 0; }
        sEmitCacheDirty = 1;
        sPvrTexDirty = 1;
        sPvrColDirty = 1;
        if (sVisMonoLog < 12) {
            sVisMonoLog++;
            printf("VISMONO: %08lx\n", (unsigned long)sVisMonoCur);
        }
    }

 //   int count = sVblCounter;
///    while (sVblCounter - count < 3) {
   //     thd_pass();
    //}
    pvr_scene_begin();

    sPvrFrameBytes = 0;
    s2DDepth = OVERLAY_Z_START;
    sMultiTexVertCount = 0;
    sMultiTexBatchCount = 0;
    sLensBracket = 0;
    sLensMaskSeen = 0;
    sLensVolNeeded = 0;
    sPtVertCount = 0;
    sPtBatchCount = 0;
    sPtBuffering = 0;
    sTrVertCount = 0;
    sTrBatchCount = 0;
    sTrBuffering = 0;
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_dr_init(&sDrState);
    sPvrCurrentList = PVR_LIST_OP_POLY;
    sPvrOnTrList = 0;
    sPvrOnPtList = 0;
    sPvrNeedHeader = 1;
    sPvrColDirty = 1;
    sPvrTexDirty = 1;
    sPvrNoDepthColList = -1;
    sPvrScisX1 = 0; sPvrScisY1 = 0;
    sPvrScisX2 = DC_FB_W; sPvrScisY2 = DC_FB_H;
    pvr_submit_user_clip(0, 0, DC_FB_W, DC_FB_H);
    pvr_set_zclip(0);

    rsp_reset();

    uint64 _tWalk0 = timer_ns_gettime64();
    walk_dl(dl, 0);
    uint64 _tWalk1 = timer_ns_gettime64();
    sFrameWalkNs += _tWalk1 - _tWalk0;
    flush_triangles();
    sPtBuffering = 0;  /* stop buffering before list finalization */
    sTrBuffering = 0;

    if (sPvrOnTrList) {
        /* Overflow-fallback path: a one-way switch happened this frame. */
        flush_multitex_pass2();
        flush_deferred_tr();
        pvr_list_finish();
    } else {
        pvr_list_finish();                       /* finish OP (still open all frame) */
        pvr_list_begin(PVR_LIST_TR_POLY);
        flush_multitex_pass2();                  /* pass 2 (skybox/detail) first */
        flush_deferred_tr();                     /* then deferred blended geometry */
        pvr_list_finish();
    }

    /* PT list: flush buffered alpha-test-only geometry */
    pvr_list_begin(PVR_LIST_PT_POLY);
    if (sPtVertCount > 0) {
        pvr_dr_init(&sDrState);
        pvr_submit_user_clip(0, 0, DC_FB_W, DC_FB_H);
        flush_pt_buffer();
    }
    pvr_list_finish();

    /* Lens of Truth: circle modifier volume. Affects only the modifier-flagged
       (bracketed lens-actor) TR geometry. Two screen-parallel triangle-fan caps
       over the circle; the prism side walls are degenerate in screen space and
       contribute no spans. */
    if (sLensVolNeeded) {
        #define LENS_VOL_SEGS 24
        static pvr_mod_hdr_t __attribute__((aligned(32))) mh_other, mh_last;
        static pvr_modifier_vol_t __attribute__((aligned(32))) mtri;
        /* Circle volume as a STRIP triangulation of the 24-gon: the classic
           strip vertex order 0, 1, N-1, 2, N-2, ... taken as consecutive
           triples. The fan (all triangles sharing one anchor) only ever
           registered a wedge of the circle on HW; whether that was triangle
           order or per-tile modifier-bin pressure is unproven -- the strip
           changes both. Every triangle carries its own header (KOS-example
           pattern); the last closes the inclusion volume. */
        float px[LENS_VOL_SEGS], py[LENS_VOL_SEGS];
        int seq[LENS_VOL_SEGS];
        {
            const float c = 0.9659258f, sn = 0.2588190f;   /* cos/sin(2*pi/24) */
            float dx = 1.0f, dy = 0.0f;
            for (int i = 0; i < LENS_VOL_SEGS; i++) {
                px[i] = sLensCx + sLensRx * dx;
                py[i] = sLensCy + sLensRy * dy;
                float ndx = dx * c - dy * sn;
                dy = dx * sn + dy * c;
                dx = ndx;
            }
            int lo = 0, hi = LENS_VOL_SEGS;
            for (int i = 0; i < LENS_VOL_SEGS; i++)
                seq[i] = (i & 1) ? --hi : lo++;
            /* seq = 0, 23, 1, 22, 2, ... : consecutive triples tile the polygon */
        }
        pvr_list_begin(PVR_LIST_TR_MOD);
        pvr_dr_init(&sDrState);
        pvr_mod_compile(&mh_other, PVR_LIST_TR_MOD, PVR_MODIFIER_OTHER_POLY, PVR_CULLING_NONE);
        pvr_mod_compile(&mh_last, PVR_LIST_TR_MOD, PVR_MODIFIER_INCLUDE_LAST_POLY, PVR_CULLING_NONE);
        mtri.flags = PVR_CMD_VERTEX_EOL;
        mtri.d1 = mtri.d2 = mtri.d3 = mtri.d4 = mtri.d5 = mtri.d6 = 0;
        for (int t = 0; t < LENS_VOL_SEGS - 2; t++) {
            /* HW finding (fan + strip builds): only the triangle paired with
               the INCLUDE_LAST_POLY header ever registers -- plain OTHER_POLY
               triangles are inert. So close every triangle as its own
               one-triangle volume; the strip triangles do not overlap, so the
               union of the volumes is the polygon. */
            pvr_mod_hdr_t* mh = &mh_last;
            SHZ_PREFETCH(mh);
            shz_sq_memcpy32_1(pvr_dr_target(sDrState), mh);
            mtri.ax = px[seq[t]];     mtri.ay = py[seq[t]];     mtri.az = 8.0e8f;
            mtri.bx = px[seq[t + 1]]; mtri.by = py[seq[t + 1]]; mtri.bz = 8.0e8f;
            mtri.cx = px[seq[t + 2]]; mtri.cy = py[seq[t + 2]]; mtri.cz = 8.0e8f;
            uint32_t *w = (uint32_t *)pvr_dr_target(sDrState);
            memcpy(w, &mtri, 32); pvr_dr_commit(w);
            w = (uint32_t *)pvr_dr_target(sDrState);
            memcpy(w, ((const u8*)&mtri) + 32, 32); pvr_dr_commit(w);
        }
                pvr_list_finish();
    }

    uint64 _tFlush1 = timer_ns_gettime64();
    sFrameFlushNs += _tFlush1 - _tWalk1;   /* list-finalize + TR/PT flushes (CPU submit) */
    pvr_scene_finish();
    uint64 _tFin1 = timer_ns_gettime64();
    sFrameFinishNs += _tFin1 - _tFlush1;   /* GPU wait */
    sFrameCount++;

    /*  FPS report every 10s */
    uint64 frameEnd = timer_ns_gettime64();
    uint64 frameTime = frameEnd - frameStart;
    sPA_Total += frameTime;
    if (frameTime < sPA_FrameMin) sPA_FrameMin = frameTime;
    if (frameTime > sPA_FrameMax) sPA_FrameMax = frameTime;
    sPC_Frames++;

    uint32 now_ms = timer_ms_gettime64();
    if (sProf_ReportTimer == 0) sProf_ReportTimer = now_ms;
    if (now_ms - sProf_ReportTimer >= PROF_REPORT_INTERVAL_MS && sPC_Frames > 0) {
#if WALK_STATS
        /* One-shot: measure timer_ns_gettime64 cost so we can judge how much of
           sPF_Tri is real work vs. the profiler measuring itself (2 reads/triangle). */
        static int sCalibrated = 0;
        if (!sCalibrated) {
            sCalibrated = 1;
            volatile uint64 sink = 0;
            uint64 c0 = timer_ns_gettime64();
            for (int k = 0; k < 100000; k++) sink += timer_ns_gettime64();
            uint64 c1 = timer_ns_gettime64();
            printf("  TIMER CAL: %.1f ns/call  (tri overhead ~= 2*this*in/frame)\n",
                   (double)(c1 - c0) / 100000.0);
        }
#endif
        float elapsed_s = (now_ms - sProf_ReportTimer) / 1000.0f;
        float fps = sPC_Frames / elapsed_s;
        float avgMs = NS_TO_MS(sPA_Total / sPC_Frames);
        float minMs = NS_TO_MS(sPA_FrameMin);
        float maxMs = NS_TO_MS(sPA_FrameMax);
        printf("FPS: %.1f  avg %.1fms  min %.1fms  max %.1fms  (%lu frames)\n",
               fps, avgMs, minMs, maxMs, (unsigned long)sPC_Frames);
#if WALK_STATS
        printf("  frame split ms: walk=%.1f (cpu DL) flush=%.1f (cpu submit) gpuwait=%.1f\n",
               NS_TO_MS(sFrameWalkNs / sPC_Frames), NS_TO_MS(sFrameFlushNs / sPC_Frames),
               NS_TO_MS(sFrameFinishNs / sPC_Frames));
        printf("  walk internals ms: combine=%.1f othermode=%.1f tex/tile=%.1f (rest = geom+dispatch)\n",
               NS_TO_MS(sWalkCombineNs / sPC_Frames), NS_TO_MS(sWalkOmodeNs / sPC_Frames),
               NS_TO_MS(sWalkTexNs / sPC_Frames));
        printf("  loadtlut: %lu/frame, %lu redundant (same content) = %lu%%\n",
               (unsigned long)(sTlutCalls / sPC_Frames),
               (unsigned long)(sTlutRedundant / sPC_Frames),
               (unsigned long)(sTlutCalls ? (100 * sTlutRedundant / sTlutCalls) : 0));
        printf("  emit: in=%lu (%lu/frame) fast=%lu clip=%lu emitted=%lu/frame\n",
               (unsigned long)sDbgTriIn, (unsigned long)(sDbgTriIn / sPC_Frames),
               (unsigned long)sDbgFastEmit, (unsigned long)sDbgClipEmit,
               (unsigned long)((sDbgFastEmit + sDbgClipEmit) / sPC_Frames));
        printf("  polys/frame: OP=%lu TR=%lu PT=%lu total=%lu\n",
               (unsigned long)(sDbgPolyOP / sPC_Frames),
               (unsigned long)(sDbgPolyTR / sPC_Frames),
               (unsigned long)(sDbgPolyPT / sPC_Frames),
               (unsigned long)((sDbgPolyOP + sDbgPolyTR + sDbgPolyPT) / sPC_Frames));
        printf("  cpu ms/frame: tex=%.2f (upload=%.2f) vtx=%.2f tri=%.2f mtx=%.2f comb=%.2f 2d=%.2f\n",
               NS_TO_MS(sPF_Tex / sPC_Frames), NS_TO_MS(sPF_TexUpload / sPC_Frames),
               NS_TO_MS(sPF_Vtx / sPC_Frames), NS_TO_MS(sPF_Tri / sPC_Frames),
               NS_TO_MS(sPF_Mtx / sPC_Frames), NS_TO_MS(sPF_Combiner / sPC_Frames),
               NS_TO_MS(sPF_Draw2D / sPC_Frames));
#endif
        sDbgFastEmit = 0; sDbgClipEmit = 0; sDbgTriIn = 0;
        sDbgPolyOP = 0; sDbgPolyTR = 0; sDbgPolyPT = 0;
        sPF_Tex = sPF_TexUpload = sPF_Vtx = sPF_Tri = 0;
        sPF_Mtx = sPF_Combiner = sPF_Draw2D = 0;
        sPA_Total = 0; sPA_FrameMin = UINT64_MAX; sPA_FrameMax = 0;
        sFrameWalkNs = 0; sFrameFlushNs = 0; sFrameFinishNs = 0;
        sWalkCombineNs = 0; sWalkOmodeNs = 0; sWalkTexNs = 0;
        sTlutCalls = 0; sTlutRedundant = 0;
        sPC_Frames = 0; sProf_ReportTimer = now_ms;
    }

    WaitForVBlank();
}

int main(int argc, char** argv) {
    memset(&sTextureCache, 0, sizeof(sTextureCache));

    printf("=== OOT DC ===\n");
#ifdef RENDER_240P
    if (vid_check_cable() != CT_VGA) {
        vid_set_mode(DM_320x240_NTSC, PM_RGB565);
    } else {
        vid_set_mode(DM_320x240_VGA, PM_RGB565);
    }
#endif
    pvr_init_params_t pvr_params = {
        /* TR_MOD (index 3) enabled for the Lens of Truth circle modifier volume */
        { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_16, PVR_BINSIZE_32 },
        1024 * 1024, 1, 0, 0, 6
    };
    pvr_init(&pvr_params);
    /* PT alpha reference -- discards texels with alpha <= 128 */
    *(volatile uint32_t *)0xA05F811C = 0x80;
#ifdef RENDER_240P
    PVR_SET(PVR_SCALER_CFG, 0x400);
#endif

    /* Pre-compile the 2D overlay colored header for fillrect.  */
    {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = /* sDepthTestEnabled ? */ PVR_DEPTHCMP_GEQUAL /* : PVR_DEPTHCMP_ALWAYS */;
        cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
        pvr_poly_compile(&sPvrHdrNoDepthCol, &cxt);
        sPvrNoDepthColList = PVR_LIST_OP_POLY;
    }

    {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                      PVR_TXRFMT_ARGB1555,// | PVR_TXRFMT_NONTWIDDLED,
                      8, 8,
                      0,
                      PVR_FILTER_NONE);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
        cxt.gen.specular = 1;
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
//        cxt.gen.alpha = 1;
        pvr_poly_compile(&sPvrHdrTex, &cxt);
    }

    {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
        cxt.gen.specular = 1;
//        cxt.gen.alpha = 1;
        pvr_poly_compile(&sPvrHdrCol, &cxt);
    }

    {
        extern void DcFs_Probe(void);
        DcFs_Probe(); /* /pc (dcload) or /cd (disc): sets gDcFsRoot for all asset loads */
    }

    if (gDcEnableAudio) {
        // Load audio data into RAM before audio system init
        extern void AudioRom_LoadAll(void);
        AudioRom_LoadAll();
    }

    {
        extern void DmaMgr_LoadAll(void);
        DmaMgr_LoadAll();
    }

    sMainThread = thd_get_current();
    vblank_handler_add(OnVBlank, NULL);

    __osTimerServicesInit();
    Main(NULL);
    return 0;
}
