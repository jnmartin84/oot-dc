
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "ultra64.h"
#include "mixer.h"

#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v)  (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~0xf)

/*
 * DMEM buffer: 8KB starting at address 0.
 * OoT uses DMEM addresses from 0x3C0 (DMEM_TEMP) up to ~0xE20 + data.
 */
#define DMEM_BUF_SIZE 0x1000
#define BUF_U8(a)  (rspa.buf + (a))
#define BUF_S16(a) ((int16_t*)(rspa.buf + (a)))
#define BUF_S32(a) ((int16_t*)(rspa.buf + (a)))

/* ---- Static mixer state (simulated RSP DMEM + registers) ---- */

static struct {
    uint8_t buf[DMEM_BUF_SIZE];

    uint16_t in;
    uint16_t out;
    uint16_t nbytes;

    uint16_t vol[2];       /* current vol L, R */
    uint16_t rate[2];      /* ramp rate L, R */
    uint16_t vol_wet;      /* reverb volume */
    uint16_t rate_wet;     /* reverb volume ramp */

    int16_t adpcm_loop_state[16]; /* copied locally */
    float adpcm_table[16][2][8];  /* pre-divided by 2048 */

    uint16_t filter_count;
    int16_t filter[8];

} __attribute__((aligned(32))) rspa;

/* ---- Float resample table (raw int16 values cast to float) ---- */
static float resample_table_f[64][4] = {
    { 3129, 26285, 3398, -33 }, { 2873, 26262, 3679, -40 },
    { 2628, 26217, 3971, -48 }, { 2394, 26150, 4276, -56 },
    { 2173, 26061, 4592, -65 }, { 1963, 25950, 4920, -74 },
    { 1764, 25817, 5260, -84 }, { 1576, 25663, 5611, -95 },
    { 1399, 25487, 5974, -106 }, { 1233, 25291, 6347, -118 },
    { 1077, 25075, 6732, -130 }, { 932, 24838, 7127, -143 },
    { 796, 24583, 7532, -156 }, { 671, 24309, 7947, -170 },
    { 554, 24016, 8371, -184 }, { 446, 23706, 8804, -198 },
    { 347, 23379, 9246, -212 }, { 257, 23036, 9696, -226 },
    { 174, 22678, 10153, -240 }, { 99, 22304, 10618, -254 },
    { 31, 21917, 11088, -268 }, { -30, 21517, 11564, -280 },
    { -84, 21104, 12045, -293 }, { -132, 20679, 12531, -304 },
    { -173, 20244, 13020, -314 }, { -210, 19799, 13512, -323 },
    { -241, 19345, 14006, -330 }, { -267, 18882, 14501, -336 },
    { -289, 18413, 14997, -340 }, { -306, 17937, 15493, -341 },
    { -320, 17456, 15988, -340 }, { -330, 16970, 16480, -337 },
    { -337, 16480, 16970, -330 }, { -340, 15988, 17456, -320 },
    { -341, 15493, 17937, -306 }, { -340, 14997, 18413, -289 },
    { -336, 14501, 18882, -267 }, { -330, 14006, 19345, -241 },
    { -323, 13512, 19799, -210 }, { -314, 13020, 20244, -173 },
    { -304, 12531, 20679, -132 }, { -293, 12045, 21104, -84 },
    { -280, 11564, 21517, -30 }, { -268, 11088, 21917, 31 },
    { -254, 10618, 22304, 99 }, { -240, 10153, 22678, 174 },
    { -226, 9696, 23036, 257 }, { -212, 9246, 23379, 347 },
    { -198, 8804, 23706, 446 }, { -184, 8371, 24016, 554 },
    { -170, 7947, 24309, 671 }, { -156, 7532, 24583, 796 },
    { -143, 7127, 24838, 932 }, { -130, 6732, 25075, 1077 },
    { -118, 6347, 25291, 1233 }, { -106, 5974, 25487, 1399 },
    { -95, 5611, 25663, 1576 }, { -84, 5260, 25817, 1764 },
    { -74, 4920, 25950, 1963 }, { -65, 4592, 26061, 2173 },
    { -56, 4276, 26150, 2394 }, { -48, 3971, 26217, 2628 },
    { -40, 3679, 26262, 2873 }, { -33, 3398, 26285, 3129 },
};

/* ---- Nybble-to-float LUT for 4-bit ADPCM (256 byte pairs) ---- */
static float nyblls_as_floats[256][2];
/* ---- Dibit-to-float LUT for 2-bit ADPCM (256 byte quads) ---- */
static float dibits_as_floats[256][4];
static int lut_initialized = 0;

static void init_luts(void) {
    if (lut_initialized) return;
    static const float nybval[16] = {0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1};
    static const float dibval[4] = {0, 1, -2, -1};
    for (int b = 0; b < 256; b++) {
        nyblls_as_floats[b][0] = nybval[b >> 4];
        nyblls_as_floats[b][1] = nybval[b & 0xf];
        dibits_as_floats[b][0] = dibval[(b >> 6) & 3];
        dibits_as_floats[b][1] = dibval[(b >> 4) & 3];
        dibits_as_floats[b][2] = dibval[(b >> 2) & 3];
        dibits_as_floats[b][3] = dibval[b & 3];
    }
    lut_initialized = 1;
}

static inline float shift_to_float(uint8_t shift) {
    static const float tbl[16] = {
        1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f,
        256.0f, 512.0f, 1024.0f, 2048.0f, 4096.0f, 8192.0f, 16384.0f, 32768.0f
    };
    return tbl[shift & 0xf];
}

static inline int16_t clamp16f(float v) {
    return (int16_t)shz_clampf(v, -32768.0f, 32767.0f);
}

/* ---- Utility functions ---- */

static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000) return -0x8000;
    if (v > 0x7fff) return 0x7fff;
    return (int16_t)v;
}

/* ---- Basic buffer operations ---- */

void aClearBufferImpl(uint16_t addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memset(BUF_U8(addr), 0, nbytes);
}

u8 *bufPtr;

void aLoadBufferImpl(const void* source_addr, uint16_t dest_addr, uint16_t nbytes, uint8_t copy) {
    if (!copy)
        bufPtr = source_addr;
    else
        shz_memcpy(BUF_U8(dest_addr), source_addr, ROUND_DOWN_16(nbytes));
}

void aSaveBufferImpl(uint16_t source_addr, int16_t* dest_addr, uint16_t nbytes) {
    shz_memcpy(dest_addr, BUF_S16(source_addr), ROUND_DOWN_16(nbytes));
}
#define recip2048 0.00048828f
void aLoadADPCMImpl(int num_entries_times_16, const int16_t* book_source_addr) {
    /* Convert ADPCM book to float, pre-divided by 2048 (bakes in the >>11 shift) */
    /* Book data is already byte-swapped to native endian by AudioLoad_ByteSwapAdpcmBook */
    init_luts();
    float* fp = (float*)rspa.adpcm_table;
    for (int i = 0; i < num_entries_times_16 / 2; i++) {
        fp[i] = recip2048 * (float)(int32_t)book_source_addr[i];
    }
}

void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    rspa.in = in;
    rspa.out = out;
    rspa.nbytes = nbytes;
}

void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    shz_memcpy(BUF_U8(out_addr), BUF_U8(in_addr), nbytes);
}

void aSetLoopImpl(ADPCM_STATE* adpcm_loop_state) {
    shz_memcpy(rspa.adpcm_loop_state, adpcm_loop_state, 16 * sizeof(int16_t));
}

/* ---- ADPCM decoder ---- */
/* Float XMTRX/FIPR version — adapted from MK DC port with 2-bit ADPCM support */

void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) {
    uint8_t* in = bufPtr + rspa.in;//BUF_U8(rspa.in);
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_32(rspa.nbytes);
#if 0
    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
#endif
    if (flags & A_INIT) {
        shz_memset2_16(out, 0);
    } else if (flags & A_LOOP) {
        shz_memcpy2_16(out, rspa.adpcm_loop_state);
    } else {
        shz_memcpy2_16(out, state);
    }

    out += 16;

    SHZ_PREFETCH(in);

    float prev1 = (float)out[-1];
    float prev2 = (float)out[-2];

    while (nbytes > 0) {
        const uint8_t header = *in++;
        const unsigned table_index = header & 0xf;
        const float shift = shift_to_float(header >> 4);
        const float(*tbl)[8] = rspa.adpcm_table[table_index];
        float instr[2][8];

        SHZ_PREFETCH(tbl);

        /* Unpack input samples to float based on ADPCM mode */
        if (flags & 4) {
            /* 2-bit ADPCM: 2 bytes per half = 4 bytes total */
            for (int i = 0; i < 2; i++) {
                const float* d0 = dibits_as_floats[*in++];
                const float* d1 = dibits_as_floats[*in++];
                instr[i][0] = d0[0] * shift; instr[i][1] = d0[1] * shift;
                instr[i][2] = d0[2] * shift; instr[i][3] = d0[3] * shift;
                instr[i][4] = d1[0] * shift; instr[i][5] = d1[1] * shift;
                instr[i][6] = d1[2] * shift; instr[i][7] = d1[3] * shift;
            }
        } else {
            /* 4-bit ADPCM: 4 bytes per half = 8 bytes total */
            for (int i = 0; i < 2; i++) {
                for (int j = 0; j < 4; j++) {
                    const float* nyb = nyblls_as_floats[*in++];
                    instr[i][j * 2]     = nyb[0] * shift;
                    instr[i][j * 2 + 1] = nyb[1] * shift;
                }
            }
        }

        SHZ_PREFETCH(in);

        /* Decode both halves using XMTRX/FIPR */
        for (int i = 0; i < 2; i++) {
            const float* ins = instr[i];
            float accf[8];
            shz_vec4_t acc_vec;
            const shz_vec4_t in_vec = { .x = prev2, .y = prev1, .z = 1.0f, .w = 0.0f };

            /* First FTRV: base terms for j=0..3
             * Matrix columns: tbl[0][0..3], tbl[1][0..3], ins[0..3], [0,0,0,1]
             * Input: (prev2, prev1, 1.0, 0.0)
             * Result[j] = tbl[0][j]*prev2 + tbl[1][j]*prev1 + ins[j]*1.0 */
            shz_xmtrx_load_cols_4x3((const shz_vec4_t*)&tbl[0][0], (const shz_vec4_t*)&tbl[1][0], (const shz_vec4_t*)&ins[0]);
            acc_vec = shz_xmtrx_transform_vec4(in_vec);
            accf[0] = acc_vec.x;
            accf[1] = acc_vec.y;
            accf[2] = acc_vec.z;
            accf[3] = acc_vec.w;

            /* Second FTRV: base terms for j=4..7 */
            shz_xmtrx_load_cols_4x3((const shz_vec4_t*)&tbl[0][4], (const shz_vec4_t*)&tbl[1][4], (const shz_vec4_t*)&ins[4]);
            acc_vec = shz_xmtrx_transform_vec4(in_vec);
            accf[4] = acc_vec.x;
            accf[5] = acc_vec.y;
            accf[6] = acc_vec.z;
            accf[7] = acc_vec.w;

            /* Cross-terms: add tbl[1][k] * ins[j-1-k] contributions */
            {
                float ins0 = ins[0], ins1 = ins[1], ins2 = ins[2];

                /* j=1: +tbl[1][0]*ins[0] */
                accf[1] += tbl[1][0] * ins0;

                /* j=2: +tbl[1][1]*ins[0] + tbl[1][0]*ins[1] via FIPR */
                accf[2] = shz_dot8f(1.0f, ins0, ins1, ins2,
                                    accf[2], tbl[1][1], tbl[1][0], 0.0f);

                /* j=7: first batch of cross-terms via FIPR */
                accf[7] = shz_dot8f(1.0f, ins0, ins1, ins2,
                                    accf[7], tbl[1][6], tbl[1][5], tbl[1][4]);

                /* j=3..6: cross-terms via FTRV
                 * Matrix columns: accf[3..6], tbl[1][2..5], tbl[1][1..4], tbl[1][0..3]
                 * Input: (1.0, ins[0], ins[1], ins[2])
                 * Adds: ins[0]*tbl[1][col] + ins[1]*tbl[1][col-1] + ins[2]*tbl[1][col-2] */
                shz_xmtrx_load_cols_4x4((const shz_vec4_t*)&accf[3], (const shz_vec4_t*)&tbl[1][2], (const shz_vec4_t*)&tbl[1][1], (const shz_vec4_t*)&tbl[1][0]);
                shz_vec4_t cross_in = { .x = 1.0f, .y = ins0, .z = ins1, .w = ins2 };
                acc_vec = shz_xmtrx_transform_vec4(cross_in);
                accf[3] = acc_vec.x;
                accf[4] = acc_vec.y;
                accf[5] = acc_vec.z;
                accf[6] = acc_vec.w;
            }

            {
                float ins3 = ins[3], ins4 = ins[4], ins5 = ins[5], ins6 = ins[6];

                /* j=4: +tbl[1][0]*ins[3] */
                accf[4] += tbl[1][0] * ins3;

                /* j=5: +tbl[1][1]*ins[3] + tbl[1][0]*ins[4] */
                accf[5] += tbl[1][1] * ins3 + tbl[1][0] * ins4;

                /* j=6: remaining cross-terms via FIPR */
                accf[6] += shz_dot8f(ins3, ins4, ins5, ins6,
                                     tbl[1][2], tbl[1][1], tbl[1][0], 0.0f);

                /* j=7: remaining cross-terms via FIPR */
                accf[7] += shz_dot8f(ins3, ins4, ins5, ins6,
                                     tbl[1][3], tbl[1][2], tbl[1][1], tbl[1][0]);
            }

            /* Write output samples */
            for (int j = 0; j < 6; j++) {
                *out++ = clamp16f(accf[j]);
            }
            prev2 = (float)clamp16f(accf[6]);
            *out++ = (int16_t)prev2;
            prev1 = (float)clamp16f(accf[7]);
            *out++ = (int16_t)prev1;
        }

        nbytes -= 16 * sizeof(int16_t);
    }

//    memcpy(state, out - 16, 16 * sizeof(int16_t));
    shz_memcpy2_16(state, (out - 16));
}

/* ---- Polyphase FIR resampler ---- */
#if 1
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    int16_t tmp[16];
    int16_t* in_initial = BUF_S16(rspa.in);
    int16_t* in = in_initial;
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_16(rspa.nbytes);
    uint32_t pitch_accumulator;
    int i;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(int16_t));
    } else {
        memcpy(tmp, state, 16 * sizeof(int16_t));
    }

    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / sizeof(int16_t);
    }

    in -= 4;
    pitch_accumulator = (uint16_t)tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));

    SHZ_PREFETCH(in);

    do {
        SHZ_PREFETCH(out);
        for (i = 0; i < 8; i++) {
            float* tbl_f = resample_table_f[pitch_accumulator >> 10];
            float sample_f = shz_dot8f(
                (float)in[0], (float)in[1], (float)in[2], (float)in[3],
                tbl_f[0], tbl_f[1], tbl_f[2], tbl_f[3]) * (1.0f / 32768.0f);
            *out++ = clamp16f(sample_f);
            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            pitch_accumulator &= 0xFFFF;
        }
        SHZ_PREFETCH(in);
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);

    state[4] = (int16_t)pitch_accumulator;
    memcpy(state, in, 4 * sizeof(int16_t));
    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(int16_t));
}
#endif
#define recip32768 0.00003052f
#define MEM_BARRIER() asm volatile("" : : : "memory");
#define MEM_BARRIER_PREF(ptr) asm volatile("pref @%0" : : "r"((ptr)) : "memory")
#if 0
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    int16_t __attribute__((aligned(32))) tmp[32] = { 0 };
    int16_t* in_initial = BUF_S16(rspa.in);
    int16_t* in = in_initial;
    MEM_BARRIER_PREF(in);
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_16(rspa.nbytes);
    uint32_t pitch_accumulator = 0;
    int i = 0;
    float* tbl_f = NULL;
    float sample_f = 0;
    size_t l;

    int16_t *dp, *sp;
    int32_t *wdp, *wsp;

    if (!(flags & A_INIT)) {
        dp = tmp;
        sp = state;

        wdp = (int32_t*) dp;
        wsp = (int32_t*) sp;

        if ((((uintptr_t) wdp | (uintptr_t) wsp) & 3) == 0) {
            for (l = 0; l < 8; l++)
                *wdp++ = *wsp++;
        } else {
            for (l = 0; l < 16; l++)
                *dp++ = *sp++;
        }
    }

    in -= 4;
    pitch_accumulator = (uint16_t) tmp[4];
    tbl_f = /* rspa. */resample_table_f[pitch_accumulator >> 10];
    SHZ_PREFETCH(tbl_f);

    dp = in;
    sp = tmp;
    for (l = 0; l < 4; l++)
        *dp++ = *sp++;

    do {
        SHZ_PREFETCH(out);
        for (i = 0; i < 8; i++) {

            float in_f[4] = { (float) (int) in[0], (float) (int) in[1], (float) (int) in[2], (float) (int) in[3] };

            sample_f =
                shz_dot8f(in_f[0], in_f[1], in_f[2], in_f[3], tbl_f[0], tbl_f[1], tbl_f[2], tbl_f[3]) * recip32768;

            MEM_BARRIER();
            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            MEM_BARRIER_PREF(in);
            pitch_accumulator &= 0xffff; //%= 0x10000;
            MEM_BARRIER();
            *out++ = clamp16f(sample_f);
            MEM_BARRIER();
            tbl_f = /* rspa. */resample_table_f[pitch_accumulator >> 10];
            MEM_BARRIER_PREF(tbl_f);
        }
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);

    state[4] = (int16_t) pitch_accumulator;
    dp = (int16_t*) (state);
    sp = in;
    for (l = 0; l < 4; l++)
        *dp++ = *sp++;

    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    dp = (int16_t*) (state + 8);
    sp = in;
    for (l = 0; l < 8; l++)
        *dp++ = *sp++;
}
#endif
/* ---- Envelope setup (OoT stereo version) ---- */

void aEnvSetup1Impl(uint8_t initialVolReverb, uint16_t rampReverb, uint16_t rampLeft, uint16_t rampRight) {
    rspa.vol_wet = (uint16_t)(initialVolReverb << 8);
    rspa.rate_wet = rampReverb;
    rspa.rate[0] = rampLeft;
    rspa.rate[1] = rampRight;
}

void aEnvSetup2Impl(uint16_t initialVolLeft, uint16_t initialVolRight) {
    rspa.vol[0] = initialVolLeft;
    rspa.vol[1] = initialVolRight;
}

/* ---- Envelope mixer (OoT version with 4 output destinations) ---- */

void aEnvMixerImpl(uint16_t dmemIn, uint16_t count, uint8_t swapLR, uint8_t x0, uint8_t x1,
                   uint8_t x2, uint8_t x3, uint32_t dmemDests, uint32_t opBits) {
    (void)x0; (void)x1; (void)x2; (void)x3; (void)opBits;

    /* Decode the 4 destination DMEM addresses from the packed word */
    uint16_t destDryL = ((dmemDests >> 24) & 0xFF) << 4;
    uint16_t destDryR = ((dmemDests >> 16) & 0xFF) << 4;
    uint16_t destWetL = ((dmemDests >>  8) & 0xFF) << 4;
    uint16_t destWetR = ((dmemDests >>  0) & 0xFF) << 4;

    int16_t* inBuf = BUF_S16(dmemIn);
    int16_t* dryL  = BUF_S16(destDryL);
    int16_t* dryR  = BUF_S16(destDryR);
    int16_t* wetL  = BUF_S16(destWetL);
    int16_t* wetR  = BUF_S16(destWetR);

    uint16_t volL = rspa.vol[0];
    uint16_t volR = rspa.vol[1];
    int16_t rateL = (int16_t)rspa.rate[0];
    int16_t rateR = (int16_t)rspa.rate[1];
    uint16_t volWet = rspa.vol_wet;
    int16_t rateWet = (int16_t)rspa.rate_wet;

    int n = ROUND_UP_16(count);

    SHZ_PREFETCH(inBuf);
    SHZ_PREFETCH(dryL);
    SHZ_PREFETCH(dryR);

    if (swapLR) {
        for (int i = 0; i < n; i += 8) {
            for (int j = 0; j < 8; j++) {
                int16_t sample = inBuf[i + j];
                int32_t sampleL = ((int32_t)sample * (int32_t)volR) >> 16;
                int32_t sampleR = ((int32_t)sample * (int32_t)volL) >> 16;

                dryL[i + j] = clamp16((int32_t)dryL[i + j] + sampleL);
                dryR[i + j] = clamp16((int32_t)dryR[i + j] + sampleR);
            }

            SHZ_PREFETCH(&inBuf[i + 16]);
            SHZ_PREFETCH(&dryL[i + 16]);
            SHZ_PREFETCH(&dryR[i + 16]);

            volL += rateL;
            volR += rateR;
        }
    } else {
        for (int i = 0; i < n; i += 8) {
            for (int j = 0; j < 8; j++) {
                int16_t sample = inBuf[i + j];
                int32_t sampleL = ((int32_t)sample * (int32_t)volL) >> 16;
                int32_t sampleR = ((int32_t)sample * (int32_t)volR) >> 16;

                dryL[i + j] = clamp16((int32_t)dryL[i + j] + sampleL);
                dryR[i + j] = clamp16((int32_t)dryR[i + j] + sampleR);
            }

            SHZ_PREFETCH(&inBuf[i + 16]);
            SHZ_PREFETCH(&dryL[i + 16]);
            SHZ_PREFETCH(&dryR[i + 16]);

            volL += rateL;
            volR += rateR;
        }
    }
}

/* ---- Interleave (stereo output from separate L/R) ---- */
#if 0
void aInterleaveImpl(uint16_t outAddr, uint16_t inL, uint16_t inR, uint16_t count) {
    int16_t* left  = BUF_S16(inL);
    int16_t* right = BUF_S16(inR);
    int16_t* out   = BUF_S16(outAddr);
    int n = count / 2;

    SHZ_PREFETCH(left);
    SHZ_PREFETCH(right);

    for (int i = 0; i < n; i++) {
        *out++ = left[i];
        *out++ = right[i];
    }
}
#endif
/* ---- Deinterleave (take every other sample) ---- */
#if 0
void aInterlImpl(uint16_t in_addr, uint16_t out_addr, uint16_t n_samples) {
    int16_t* in  = BUF_S16(in_addr);
    int16_t* out = BUF_S16(out_addr);
    int n = ROUND_UP_8(n_samples);

    SHZ_PREFETCH(in);

    do {
        SHZ_PREFETCH(out);
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        SHZ_PREFETCH(in);
        n -= 8;
    } while (n > 0);
}
#endif

void aInterlImpl(uint16_t in_addr, uint16_t out_addr, uint16_t n_samples) {
    int16_t* in  = BUF_S16(in_addr);
    int16_t* out = BUF_S16(out_addr);
    int n = ROUND_UP_8(n_samples);

    SHZ_PREFETCH(out);

#if 1
    if(n) {
        uint32_t scratch;
        asm volatile(R"(
            .align 2
        0:
            mov     %[in], %[scr]
            mov.w   @(0, %[in]), r0
            add     #32, %[scr]
            mov.w   r0, @(0, %[out])
            pref    @%[scr]
            mov.w   @(4, %[in]), r0
            mov     %[out], %[scr]
            mov.w   r0, @(2, %[out])
            add     #16, %[scr]
            mov.w   @(8, %[in]), r0
            pref    @%[out]
            mov.w   r0, @(4, %[out])
            mov.w   @(12, %[in]), r0
            add     #16, %[in]
            mov.w   r0, @(6, %[out])
            mov.w   @(0, %[in]), r0
            mov.w   r0, @(8, %[out])
            mov.w   @(4, %[in]), r0
            mov.w   r0, @(10, %[out])
            mov.w   @(8, %[in]), r0
            add     #-8, %[cnt]
            mov.w   r0, @(12, %[out])
            cmp/pl  %[cnt]
            mov.w   @(12, %[in]), r0
            add     #16, %[in]
            mov.w   r0, @(14, %[out])
            bt.s    0b
            add     #16, %[out]
        )"
        : [in] "+&r" (in), [out] "+&r" (out), [cnt] "+&r" (n), [scr] "=&r" (scratch),
          "=m" (*(uint16_t (*)[])out)
        : "m" (*(const uint16_t (*)[])in)
        : "t", "r0");
    }
#else
    do {
        SHZ_PREFETCH(out);
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;

        SHZ_PREFETCH(in);
        n -= 8;
    } while (n > 0);
#endif
}

/* ---- Mix: out = ((out * 0x7FFF) + (in * gain) + 0x4000) >> 15 ---- */

void aMixImpl(uint16_t count, int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    int nbytes = ROUND_UP_32(ROUND_DOWN_16(count << 4));
    int16_t* in  = BUF_S16(in_addr);
    int16_t* out = BUF_S16(out_addr);
    int i;
    int32_t sample;

    SHZ_PREFETCH(in);
    SHZ_PREFETCH(out);

    if (gain == -0x8000) {
        while (nbytes > 0) {
            SHZ_PREFETCH(in + 16);
            SHZ_PREFETCH(out + 16);
            for (i = 0; i < 16; i++) {
                sample = *out - *in++;
                *out++ = clamp16(sample);
            }
            nbytes -= 16 * sizeof(int16_t);
        }
        return;
    }

    while (nbytes > 0) {
        SHZ_PREFETCH(in + 16);
        SHZ_PREFETCH(out + 16);
        for (i = 0; i < 16; i++) {
            sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;
            *out++ = clamp16(sample);
        }
        nbytes -= 16 * sizeof(int16_t);
    }
}

/* ---- 8-tap filter for reverb ---- */

void aFilterImpl(uint8_t flags, uint16_t count_or_buf, int16_t* state_or_filter) {
    if (flags > A_INIT) {
        /* Load filter coefficients and store count */
        rspa.filter_count = ROUND_UP_16(count_or_buf);
        shz_memcpy(rspa.filter, state_or_filter, sizeof(rspa.filter));
    } else {
        int16_t tmp[16], tmp2[8];
        int count = rspa.filter_count;
        int16_t* buf = BUF_S16(count_or_buf);

        if (flags == A_INIT) {
            memset(tmp, 0, 8 * sizeof(int16_t));
            memset(tmp2, 0, 8 * sizeof(int16_t));
        } else {
            shz_memcpy(tmp, state_or_filter, 8 * sizeof(int16_t));
            shz_memcpy(tmp2, state_or_filter + 8, 8 * sizeof(int16_t));
        }

        /* Average coefficients with saved state */
        for (int i = 0; i < 8; i++) {
            rspa.filter[i] = (tmp2[i] + rspa.filter[i]) / 2;
        }

        SHZ_PREFETCH(buf);

        do {
            shz_memcpy(tmp + 8, buf, 8 * sizeof(int16_t));
            SHZ_PREFETCH(buf + 8);
            for (int i = 0; i < 8; i++) {
                int64_t sample = 0x4000; /* round term */
                for (int j = 0; j < 8; j++) {
                    sample += tmp[i + j] * rspa.filter[7 - j];
                }
                buf[i] = clamp16((int32_t)(sample >> 15));
            }
            shz_memcpy(tmp, tmp + 8, 8 * sizeof(int16_t));

            buf += 8;
            count -= 8 * sizeof(int16_t);
        } while (count > 0);

        shz_memcpy(state_or_filter, tmp, 8 * sizeof(int16_t));
        shz_memcpy(state_or_filter + 8, rspa.filter, 8 * sizeof(int16_t));
    }
}

/* ---- HiLo gain ---- */
#if 0
void aHiLoGainImpl(uint8_t g, uint16_t count, uint16_t dmemIn) {
    int16_t* samples = BUF_S16(dmemIn);
    int nbytes = ROUND_UP_32(count);

    SHZ_PREFETCH(samples);

    do {
        SHZ_PREFETCH(samples + 8);
        *samples = clamp16((*samples * g) >> 4); samples++;
        *samples = clamp16((*samples * g) >> 4); samples++;
        *samples = clamp16((*samples * g) >> 4); samples++;
        *samples = clamp16((*samples * g) >> 4); samples++;
        *samples = clamp16((*samples * g) >> 4); samples++;
        *samples = clamp16((*samples * g) >> 4); samples++;
        *samples = clamp16((*samples * g) >> 4); samples++;
        *samples = clamp16((*samples * g) >> 4); samples++;
        nbytes -= 8;
    } while (nbytes > 0);
}
#else
void aHiLoGainImpl(uint8_t g, uint16_t count, uint16_t addr) {
    int32_t* samples = (int32_t*) BUF_U8(addr & ~3);
    int nbytes = ROUND_UP_32(count) + 8;

    do {
        uint32_t s1, s2, s3, s4;
        s1 = samples[0];
        s2 = (s1 >> 16) & 0xffff;
        s1 &= 0xffff;
        s3 = samples[1];
        s4 = (s3 >> 16) & 0xffff;
        s3 &= 0xffff;

        MEM_BARRIER();

        s1 = clamp16(s1 * g) >> 4;
        s2 = clamp16(s2 * g) >> 4;
        s3 = clamp16(s3 * g) >> 4;
        s4 = clamp16(s4 * g) >> 4;

        MEM_BARRIER();

        *samples++ = (s2 << 16) | s1;
        *samples++ = (s3 << 16) | s2;
        nbytes -= 4;
    } while (nbytes > 0);
}

#endif

/* ---- Duplicate 128-byte blocks ---- */

void aDuplicateImpl(uint16_t count, uint16_t in_addr, uint16_t out_addr) {
    uint8_t* in  = BUF_U8(in_addr);
    uint8_t* out = BUF_U8(out_addr);
//    uint8_t tmp[128];

//    shz_memcpy(tmp, in, 128);
    do {
        shz_memcpy(out, in/* tmp */, 128);
        out += 128;
    } while (count-- > 0);
}

/* ---- Additive mixer with clamping (for Haas effect) ---- */

void aAddMixerImpl(uint16_t count, uint16_t in_addr, uint16_t out_addr, uint16_t a4) {
    (void)a4; /* Unused - RSP does pure saturating add */
    int16_t* in  = BUF_S16(in_addr);
    int16_t* out = BUF_S16(out_addr);
    int nbytes = ROUND_UP_64(ROUND_DOWN_16(count));

    SHZ_PREFETCH(in);
    SHZ_PREFETCH(out);

    do {
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        SHZ_PREFETCH(in);
        SHZ_PREFETCH(out);
        nbytes -= 16 * sizeof(int16_t);
    } while (nbytes > 0);
}

/* ---- S8 decode: 8-bit signed to 16-bit ---- */

void aS8DecImpl(uint8_t flags, ADPCM_STATE state) {
    uint8_t* in = bufPtr + rspa.in;//BUF_U8(rspa.in);
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_32(rspa.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        shz_memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    } else {
        shz_memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;

    while (nbytes > 0) {
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        nbytes -= 16 * sizeof(int16_t);
    }

    shz_memcpy(state, out - 16, 16 * sizeof(int16_t));
}

/* ---- Zero-order hold resample (for Haas pitch adjustment) ---- */

void aResampleZohImpl(uint16_t pitch, uint16_t start_fract) {
    int16_t* in  = BUF_S16(rspa.in);
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_8(rspa.nbytes);
    uint32_t pos = start_fract;
    uint32_t pitch_add = pitch << 2;

    do {
        *out++ = in[pos >> 17];
        pos += pitch_add;
        *out++ = in[pos >> 17];
        pos += pitch_add;
        *out++ = in[pos >> 17];
        pos += pitch_add;
        *out++ = in[pos >> 17];
        pos += pitch_add;
        nbytes -= 4 * sizeof(int16_t);
    } while (nbytes > 0);
}

/* ---- Unknown commands ---- */

void aUnkCmd3Impl(uint16_t a, uint16_t b, uint16_t c) {
    (void)a; (void)b; (void)c;
}

void aUnkCmd19Impl(uint8_t f, uint16_t count, uint16_t out_addr, uint16_t in_addr) {
    int nbytes = ROUND_UP_64(count);
    int16_t* in  = BUF_S16(in_addr + f);
    int16_t* out = BUF_S16(out_addr);
    int16_t tbl[32];

    shz_memcpy(tbl, in, 32 * sizeof(int16_t));
    do {
        for (int i = 0; i < 32; i++) {
            out[i] = clamp16(out[i] * tbl[i]);
        }
        out += 32;
        nbytes -= 32 * sizeof(int16_t);
    } while (nbytes > 0);
}
