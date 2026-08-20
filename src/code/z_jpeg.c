#include "jpeg.h"

#include "array_count.h"
#include "attributes.h"
#include "gfx.h"
#include "printf.h"
#include "sys_ucode.h"
#include "terminal.h"
#include "translation.h"
#include "ultra64.h"

#if !PLATFORM_N64
#include <math.h>
#include <string.h>
#endif

#define MARKER_ESCAPE 0x00
#define MARKER_SOI 0xD8
#define MARKER_SOF 0xC0
#define MARKER_DHT 0xC4
#define MARKER_DQT 0xDB
#define MARKER_DRI 0xDD
#define MARKER_SOS 0xDA
#define MARKER_APP0 0xE0
#define MARKER_APP1 0xE1
#define MARKER_APP2 0xE2
#define MARKER_COM 0xFE
#define MARKER_EOI 0xD9

#if PLATFORM_N64
/**
 * Configures and schedules a JPEG decoder task and waits for it to finish.
 */
void Jpeg_ScheduleDecoderTask(JpegContext* ctx) {
    static OSTask sJpegTask = {
        M_NJPEGTASK,                     // type
        0,                               // flags
        NULL,                            // ucode_boot
        0,                               // ucode_boot_size
        njpgdspMainTextStart,            // ucode
        SP_UCODE_SIZE,                   // ucode_size
        njpgdspMainDataStart,            // ucode_data
        SP_UCODE_DATA_SIZE,              // ucode_data_size
        NULL,                            // dram_stack
        0,                               // dram_stack_size
        NULL,                            // output_buff
        NULL,                            // output_buff_size
        NULL,                            // data_ptr
        sizeof(JpegTaskData),            // data_size
        NULL,                            // yield_data_ptr
        sizeof(ctx->workBuf->yieldData), // yield_data_size
    };

    JpegWork* workBuf = ctx->workBuf;
    s32 pad[2];

    workBuf->taskData.address = OS_K0_TO_PHYSICAL(&workBuf->data);
    workBuf->taskData.mode = ctx->mode;
    workBuf->taskData.mbCount = 4;
    workBuf->taskData.qTableYPtr = OS_K0_TO_PHYSICAL(&workBuf->qTableY);
    workBuf->taskData.qTableUPtr = OS_K0_TO_PHYSICAL(&workBuf->qTableU);
    workBuf->taskData.qTableVPtr = OS_K0_TO_PHYSICAL(&workBuf->qTableV);

    sJpegTask.t.flags = 0;
    sJpegTask.t.ucode_boot = SysUcode_GetUCodeBoot();
    sJpegTask.t.ucode_boot_size = SysUcode_GetUCodeBootSize();
    sJpegTask.t.yield_data_ptr = workBuf->yieldData;
    sJpegTask.t.data_ptr = (u64*)&workBuf->taskData;

    ctx->scTask.next = NULL;
    ctx->scTask.flags = OS_SC_NEEDS_RSP;
    ctx->scTask.msgQueue = &ctx->mq;
    ctx->scTask.msg = NULL;
    ctx->scTask.framebuffer = NULL;
    ctx->scTask.list = sJpegTask;

    osSendMesg(&gScheduler.cmdQueue, (OSMesg)&ctx->scTask, OS_MESG_BLOCK);
    Sched_Notify(&gScheduler);
    osRecvMesg(&ctx->mq, NULL, OS_MESG_BLOCK);
}
#endif

#if !PLATFORM_N64
/**
 * Software JPEG IDCT + YUV->RGBA5551 decoder.
 * Replaces the N64 RSP microcode (njpgdspMain) on non-N64 platforms.
 * Based on Mupen64 HLE RSP implementation by Bobby Smiles / Richard Goedeken / Hacktarux.
 */

#define SUBBLOCK_SIZE 64

static const u32 sJpegZigZagTable[SUBBLOCK_SIZE] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

static s16 Jpeg_ClampS16(s32 x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (s16)x;
}

static void Jpeg_MultSubBlocks(s16* dst, const s16* src1, const s16* src2, u32 shift) {
    u32 i;
    for (i = 0; i < SUBBLOCK_SIZE; i++) {
        s32 v = ((s32)src1[i] * (s32)src2[i]) << shift;
        dst[i] = Jpeg_ClampS16(v);
    }
}

static void Jpeg_ZigZagSubBlock(s16* dst, const s16* src) {
    u32 i;
    for (i = 0; i < SUBBLOCK_SIZE; i++) {
        dst[i] = src[sJpegZigZagTable[i]];
    }
}

/* IDCT constants - normalized such that C4 = 1 */
#define JIDCT_C3   1.175875602f
#define JIDCT_C6   0.541196100f
#define JIDCT_K1   0.765366865f
#define JIDCT_K2  -1.847759065f
#define JIDCT_K3  -0.390180644f
#define JIDCT_K4  -1.961570561f
#define JIDCT_K5   1.501321110f
#define JIDCT_K6   2.053119869f
#define JIDCT_K7   3.072711027f
#define JIDCT_K8   0.298631336f
#define JIDCT_K9  -0.899976223f
#define JIDCT_K10 -2.562915448f

static void Jpeg_InverseDCT1D(const f32* x, f32* dst, u32 stride) {
    f32 e[4];
    f32 f[4];
    f32 x26, x1357, x15, x37, x17, x35;

    x15   = JIDCT_K3  * (x[1] + x[5]);
    x37   = JIDCT_K4  * (x[3] + x[7]);
    x17   = JIDCT_K9  * (x[1] + x[7]);
    x35   = JIDCT_K10 * (x[3] + x[5]);
    x1357 = JIDCT_C3  * (x[1] + x[3] + x[5] + x[7]);
    x26   = JIDCT_C6  * (x[2] + x[6]);

    f[0] = x[0] + x[4];
    f[1] = x[0] - x[4];
    f[2] = x26 + JIDCT_K1 * x[2];
    f[3] = x26 + JIDCT_K2 * x[6];

    e[0] = x1357 + x15 + JIDCT_K5 * x[1] + x17;
    e[1] = x1357 + x37 + JIDCT_K7 * x[3] + x35;
    e[2] = x1357 + x15 + JIDCT_K6 * x[5] + x35;
    e[3] = x1357 + x37 + JIDCT_K8 * x[7] + x17;

    *dst = f[0] + f[2] + e[0]; dst += stride;
    *dst = f[1] + f[3] + e[1]; dst += stride;
    *dst = f[1] - f[3] + e[2]; dst += stride;
    *dst = f[0] - f[2] + e[3]; dst += stride;
    *dst = f[0] - f[2] - e[3]; dst += stride;
    *dst = f[1] - f[3] - e[2]; dst += stride;
    *dst = f[1] + f[3] - e[1]; dst += stride;
    *dst = f[0] + f[2] - e[0];
}

static void Jpeg_InverseDCTSubBlock(s16* dst, const s16* src) {
    f32 x[8];
    f32 block[SUBBLOCK_SIZE];
    u32 i, j;

    /* IDCT 1D on rows (+ transposition) */
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) {
            x[j] = (f32)src[i * 8 + j];
        }
        Jpeg_InverseDCT1D(x, &block[i], 8);
    }

    /* IDCT 1D on columns (thanks to previous transposition) */
    for (i = 0; i < 8; i++) {
        Jpeg_InverseDCT1D(&block[i * 8], x, 1);
        /* C4 = 1 normalization implies a division by 8 */
        for (j = 0; j < 8; j++) {
            f32 val = x[j] * 0.125f;
            if (val > 32767.0f) val = 32767.0f;
            if (val < -32768.0f) val = -32768.0f;
            dst[i + j * 8] = (s16)val;
        }
    }
}

static void Jpeg_DecodeMacroblockPS(s16* macroblock, u32 subblock_count, const s16 qtables[3][SUBBLOCK_SIZE]) {
    u32 sb;
    u32 q = 0;

    for (sb = 0; sb < subblock_count; sb++) {
        s16 tmp_sb[SUBBLOCK_SIZE];
        int isChroma = (subblock_count - sb <= 2);

        if (isChroma) {
            q++;
        }

        Jpeg_MultSubBlocks(macroblock, macroblock, qtables[q], 4);
        Jpeg_ZigZagSubBlock(tmp_sb, macroblock);
        Jpeg_InverseDCTSubBlock(macroblock, tmp_sb);

        macroblock += SUBBLOCK_SIZE;
    }
}

static u16 Jpeg_GetRGBA5551(s16 y, s16 u, s16 v) {
    f32 fY = (f32)y + 2048.0f;
    f32 fU = (f32)u;
    f32 fV = (f32)v;

    s32 r = (s32)(fY + 1.4025f * fV);
    s32 g = (s32)(fY - 0.3443f * fU - 0.7144f * fV);
    s32 b = (s32)(fY + 1.7729f * fU);

    /* Scale from 0-4095 range to 0-31 (5-bit) */
    r = (r < 0) ? 0 : (r > 4095) ? 31 : (r >> 7);
    g = (g < 0) ? 0 : (g > 4095) ? 31 : (g >> 7);
    b = (b < 0) ? 0 : (b > 4095) ? 31 : (b >> 7);

    return (u16)((r << 11) | (g << 6) | (b << 1) | 1);
}

static void Jpeg_EmitRGBATileLine(const s16* y, const s16* u, u16* out) {
    const s16* v  = u + SUBBLOCK_SIZE;
    const s16* y2 = y + SUBBLOCK_SIZE;

    out[0]  = Jpeg_GetRGBA5551(y[0],  u[0], v[0]);
    out[1]  = Jpeg_GetRGBA5551(y[1],  u[0], v[0]);
    out[2]  = Jpeg_GetRGBA5551(y[2],  u[1], v[1]);
    out[3]  = Jpeg_GetRGBA5551(y[3],  u[1], v[1]);
    out[4]  = Jpeg_GetRGBA5551(y[4],  u[2], v[2]);
    out[5]  = Jpeg_GetRGBA5551(y[5],  u[2], v[2]);
    out[6]  = Jpeg_GetRGBA5551(y[6],  u[3], v[3]);
    out[7]  = Jpeg_GetRGBA5551(y[7],  u[3], v[3]);
    out[8]  = Jpeg_GetRGBA5551(y2[0], u[4], v[4]);
    out[9]  = Jpeg_GetRGBA5551(y2[1], u[4], v[4]);
    out[10] = Jpeg_GetRGBA5551(y2[2], u[5], v[5]);
    out[11] = Jpeg_GetRGBA5551(y2[3], u[5], v[5]);
    out[12] = Jpeg_GetRGBA5551(y2[4], u[6], v[6]);
    out[13] = Jpeg_GetRGBA5551(y2[5], u[6], v[6]);
    out[14] = Jpeg_GetRGBA5551(y2[6], u[7], v[7]);
    out[15] = Jpeg_GetRGBA5551(y2[7], u[7], v[7]);
}

static void Jpeg_EmitTilesMode0(const s16* macroblock, u16* out) {
    u32 y_offset = 0;
    u32 u_offset = 2 * SUBBLOCK_SIZE;
    u32 i;

    for (i = 0; i < 8; i++) {
        Jpeg_EmitRGBATileLine(&macroblock[y_offset], &macroblock[u_offset], &out[i * 16]);
        y_offset += 8;
        u_offset += 8;
    }
}

static void Jpeg_EmitTilesMode2(const s16* macroblock, u16* out) {
    u32 y_offset = 0;
    u32 u_offset = 4 * SUBBLOCK_SIZE;
    u32 i;

    for (i = 0; i < 8; i++) {
        Jpeg_EmitRGBATileLine(&macroblock[y_offset],     &macroblock[u_offset], &out[(i * 2) * 16]);
        Jpeg_EmitRGBATileLine(&macroblock[y_offset + 8], &macroblock[u_offset], &out[(i * 2 + 1) * 16]);
        y_offset += (i == 3) ? (SUBBLOCK_SIZE + 16) : 16;
        u_offset += 8;
    }
}

static void Jpeg_SoftwareDecode(JpegWork* workBuf, u32 mode) {
    s16 macroblock[6 * SUBBLOCK_SIZE];
    u16 tile[16 * 16];
    s16 qtables[3][SUBBLOCK_SIZE];
    u32 subblock_count = mode + 4;
    u32 macroblock_size = subblock_count * SUBBLOCK_SIZE;
    u32 mb;

    /* Copy quantization tables (u16 -> s16) */
    memcpy(qtables[0], workBuf->qTableY.table, SUBBLOCK_SIZE * sizeof(s16));
    memcpy(qtables[1], workBuf->qTableU.table, SUBBLOCK_SIZE * sizeof(s16));
    memcpy(qtables[2], workBuf->qTableV.table, SUBBLOCK_SIZE * sizeof(s16));

    for (mb = 0; mb < 4; mb++) {
        /* Copy coefficients to local buffer */
        memcpy(macroblock, workBuf->data[mb], macroblock_size * sizeof(s16));

        /* Dequantize + zigzag + IDCT each subblock */
        Jpeg_DecodeMacroblockPS(macroblock, subblock_count, (const s16(*)[SUBBLOCK_SIZE])qtables);

        /* Convert YUV to RGBA5551 tiles */
        if (mode == 0) {
            Jpeg_EmitTilesMode0(macroblock, tile);
        } else {
            Jpeg_EmitTilesMode2(macroblock, tile);
        }

        /* Write back to work buffer (16x16 = 256 u16s) */
        memcpy(workBuf->data[mb], tile, 16 * 16 * sizeof(u16));
    }
}
#endif

/**
 * Copies a 16x16 block of decoded image data to the Z-buffer.
 */
void Jpeg_CopyToZbuffer(u16* src, u16* zbuffer, s32 x, s32 y) {
    u16* dst = zbuffer + (((y * SCREEN_WIDTH) + x) * 16);
    s32 i;

    for (i = 0; i < 16; i++) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        dst[4] = src[4];
        dst[5] = src[5];
        dst[6] = src[6];
        dst[7] = src[7];
        dst[8] = src[8];
        dst[9] = src[9];
        dst[10] = src[10];
        dst[11] = src[11];
        dst[12] = src[12];
        dst[13] = src[13];
        dst[14] = src[14];
        dst[15] = src[15];

        src += 16;
        dst += SCREEN_WIDTH;
    }
}

/**
 * Reads an u16 from a possibly unaligned address in memory.
 *
 * Replaces unaligned 16-bit reads with a pair of aligned reads, allowing for reading the possibly
 * unaligned values in JPEG header files.
 */
u16 Jpeg_GetUnalignedU16(u8* ptr) {
#if PLATFORM_N64
    if (((uintptr_t)ptr & 1) == 0) {
        // Read the value normally if it's aligned to a 16-bit address.
        return *(u16*)ptr;
    } else {
        // Read unaligned values using two separate aligned memory accesses when it's not.
        return *(u16*)(ptr - 1) << 8 | (*(u16*)(ptr + 1) >> 8);
    }
#else
    // JPEG header data is big-endian; read bytes explicitly for little-endian hosts.
    return (ptr[0] << 8) | ptr[1];
#endif
}

/**
 * Parses the markers in the JPEG file, storing information such as the pointer to the image data
 * in `ctx` for later processing.
 */
void Jpeg_ParseMarkers(u8* ptr, JpegContext* ctx) {
    u32 exit = false;

    ctx->dqtCount = 0;
    ctx->dhtCount = 0;

    while (true) {
        if (exit) {
            break;
        }

        // 0xFF indicates the start of a JPEG marker, so look for the next.
        if (*ptr++ == 0xFF) {
            switch (*ptr++) {
                case MARKER_ESCAPE: {
                    // Compressed value 0xFF is stored as 0xFF00 to escape it, so ignore it.
                    break;
                }
                case MARKER_SOI: {
                    // Start of Image
                    PRINTF("MARKER_SOI\n");
                    break;
                }
                case MARKER_APP0: {
                    // Application marker for JFIF
                    PRINTF("MARKER_APP0 %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_APP1: {
                    // Application marker for EXIF
                    PRINTF("MARKER_APP1 %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_APP2: {
                    PRINTF("MARKER_APP2 %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_DQT: {
                    // Define Quantization Table, stored for later processing
                    PRINTF("MARKER_DQT %d %d %02x\n", ctx->dqtCount, Jpeg_GetUnalignedU16(ptr), ptr[2]);
                    ctx->dqtPtr[ctx->dqtCount++] = ptr + 2;
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_DHT: {
                    // Define Huffman Table, stored for later processing
                    PRINTF("MARKER_DHT %d %d %02x\n", ctx->dhtCount, Jpeg_GetUnalignedU16(ptr), ptr[2]);
                    ctx->dhtPtr[ctx->dhtCount++] = ptr + 2;
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_DRI: {
                    // Define Restart Interval
                    PRINTF("MARKER_DRI %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_SOF: {
                    // Start of Frame, stores important metadata of the image.
                    // Only used for extracting the sampling factors (ctx->mode).
                    PRINTF(T("MARKER_SOF   %d "
                             "精度%02x 垂直%d 水平%d compo%02x "
                             "(1:Y)%d (H0=2,V0=1(422) or 2(420))%02x (量子化テーブル)%02x "
                             "(2:Cb)%d (H1=1,V1=1)%02x (量子化テーブル)%02x "
                             "(3:Cr)%d (H2=1,V2=1)%02x (量子化テーブル)%02x\n",
                             "MARKER_SOF   %d "
                             "accuracy%02x vertical%d horizontal%d compo%02x "
                             "(1:Y)%d (H0=2,V0=1(422) or 2(420))%02x (quantization tables)%02x "
                             "(2:Cb)%d (H1=1,V1=1)%02x (quantization tables)%02x "
                             "(3:Cr)%d (H2=1,V2=1)%02x (quantization tables)%02x\n"),
                           Jpeg_GetUnalignedU16(ptr),
                           // precision, height, width, component count (assumed to be 3)
                           ptr[2], Jpeg_GetUnalignedU16(ptr + 3), Jpeg_GetUnalignedU16(ptr + 5), ptr[7],
                           //
                           ptr[8], ptr[9], ptr[10],   // Y component
                           ptr[11], ptr[12], ptr[13], // Cb component
                           ptr[14], ptr[15], ptr[16]  // Cr component
                    );

                    if (ptr[9] == 0x21) {
                        // component Y : V0 == 1
                        ctx->mode = 0;
                    } else if (ptr[9] == 0x22) {
                        // component Y : V0 == 2
                        ctx->mode = 2;
                    }
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
                case MARKER_SOS: {
                    // Start of Scan marker, indicates the start of the image data.
                    PRINTF("MARKER_SOS %d\n", Jpeg_GetUnalignedU16(ptr));
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    ctx->imageData = ptr;
                    break;
                }
                case MARKER_EOI: {
                    // End of Image
                    PRINTF("MARKER_EOI\n");
                    exit = true;
                    break;
                }
                default: {
                    PRINTF(T("マーカー不明 %02x\n", "Unknown marker %02x\n"), ptr[-1]);
                    ptr += Jpeg_GetUnalignedU16(ptr);
                    break;
                }
            }
        }
    }
}

s32 Jpeg_Decode(void* data, void* zbuffer, void* work, u32 workSize) {
    s32 y;
    s32 x;
    u32 j;
    u32 i;
    JpegContext ctx;
    JpegHuffmanTable hTables[4];
    JpegDecoder decoder;
    JpegDecoderState state;
    JpegWork* workBuff;
    UNUSED_NDEBUG OSTime diff;
    OSTime time;
    OSTime curTime;

    workBuff = work;

    time = osGetTime();
    // (?) I guess MB_SIZE=0x180, PROC_OF_MBS=5 which means data is not a part of JpegWork
    ASSERT(workSize >= sizeof(JpegWork), "worksize >= sizeof(JPEGWork) + MB_SIZE * (PROC_OF_MBS - 1)", "../z_jpeg.c",
           527);

#if PLATFORM_N64
    osCreateMesgQueue(&ctx.mq, &ctx.msg, 1);
    Sched_FlushTaskQueue();
#endif

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** fifoバッファの同期待ち time = %6.3f ms ***\n",
             "*** Wait for synchronization of fifo buffer time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    ctx.workBuf = workBuff;
    Jpeg_ParseMarkers(data, &ctx);

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** 各セグメントのマーカーのチェック time = %6.3f ms ***\n",
             "*** Check markers for each segment time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    switch (ctx.dqtCount) {
        case 1:
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[0], &workBuff->qTableY, 3);
            break;
        case 2:
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[0], &workBuff->qTableY, 1);
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[1], &workBuff->qTableU, 1);
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[1], &workBuff->qTableV, 1);
            break;
        case 3:
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[0], &workBuff->qTableY, 1);
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[1], &workBuff->qTableU, 1);
            JpegUtils_ProcessQuantizationTable(ctx.dqtPtr[2], &workBuff->qTableV, 1);
            break;
        default:
            return -1;
    }

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** 量子化テーブル作成 time = %6.3f ms ***\n", "*** Create quantization table time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    switch (ctx.dhtCount) {
        case 1:
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[0], &hTables[0], workBuff->codesLengths, workBuff->codes, 4)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            break;
        case 4:
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[0], &hTables[0], workBuff->codesLengths, workBuff->codes, 1)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[1], &hTables[1], workBuff->codesLengths, workBuff->codes, 1)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[2], &hTables[2], workBuff->codesLengths, workBuff->codes, 1)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            if (JpegUtils_ProcessHuffmanTable(ctx.dhtPtr[3], &hTables[3], workBuff->codesLengths, workBuff->codes, 1)) {
                PRINTF("Error : Cant' make huffman table.\n");
            }
            break;
        default:
            return -1;
    }

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** ハフマンテーブル作成 time = %6.3f ms ***\n", "*** Huffman table creation time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    decoder.imageData = ctx.imageData;
    decoder.mode = ctx.mode;
    decoder.unk_05 = 2;
    decoder.hTablePtrs[0] = &hTables[0];
    decoder.hTablePtrs[1] = &hTables[1];
    decoder.hTablePtrs[2] = &hTables[2];
    decoder.hTablePtrs[3] = &hTables[3];
    decoder.unk_18 = 0;

    x = y = 0;
    for (i = 0; i < 300; i += 4) {
        if (JpegDecoder_Decode(&decoder, (u16*)workBuff->data, 4, i != 0, &state)) {
            PRINTF_COLOR_RED();
            PRINTF("Error : Can't decode jpeg\n");
            PRINTF_RST();
        } else {
#if PLATFORM_N64
            Jpeg_ScheduleDecoderTask(&ctx);
            osInvalDCache(&workBuff->data, sizeof(workBuff->data[0]));
#else
            Jpeg_SoftwareDecode(workBuff, ctx.mode);
#endif

            for (j = 0; j < ARRAY_COUNT(workBuff->data); j++) {
                Jpeg_CopyToZbuffer(workBuff->data[j], zbuffer, x, y);
                x++;

                if (x >= 20) {
                    x = 0;
                    y++;
                }
            }
        }
    }

    curTime = osGetTime();
    diff = curTime - time;
    time = curTime;
    PRINTF(T("*** 展開 & 描画 time = %6.3f ms ***\n", "*** Unfold & draw time = %6.3f ms ***\n"),
           OS_CYCLES_TO_USEC(diff) / 1000.0f);

    return 0;
}
