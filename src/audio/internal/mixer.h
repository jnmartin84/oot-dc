#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ultra64/abi.h"

/* Undefine all N64 Acmd macros so we can redirect them to software implementations */
#undef aSegment
#undef aClearBuffer
#undef aSetBuffer
#undef aLoadBuffer
#undef aSaveBuffer
#undef aDMEMMove
#undef aMix
#undef aEnvMixer
#undef aEnvSetup1
#undef aEnvSetup2
#undef aResample
#undef aInterleave
#undef aInterl
#undef aSetLoop
#undef aLoadADPCM
#undef aADPCMdec
#undef aS8Dec
#undef aHiLoGain
#undef aFilter
#undef aDuplicate
#undef aAddMixer
#undef aResampleZoh
#undef aUnkCmd3
#undef aUnkCmd19
#undef aSetVolume
#undef aSetVolume32
#undef aPoleFilter

/* On DC, virtual == physical */
#undef VIRTUAL_TO_PHYSICAL2
#undef OS_K0_TO_PHYSICAL
#define VIRTUAL_TO_PHYSICAL2(x) (x)
#define OS_K0_TO_PHYSICAL(x) (x)

/* ---- Implementation function prototypes ---- */

void aClearBufferImpl(uint16_t addr, int nbytes);
void aLoadBufferImpl(const void* source_addr, uint16_t dest_addr, uint16_t nbytes, uint8_t copy);
void aSaveBufferImpl(uint16_t source_addr, int16_t* dest_addr, uint16_t nbytes);
void aLoadADPCMImpl(int num_entries_times_16, const int16_t* book_source_addr);
void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes);
void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes);
void aSetLoopImpl(ADPCM_STATE* adpcm_loop_state);
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state);
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state);
void aEnvSetup1Impl(uint8_t initialVolReverb, uint16_t rampReverb, uint16_t rampLeft, uint16_t rampRight);
void aEnvSetup2Impl(uint16_t initialVolLeft, uint16_t initialVolRight);
void aEnvMixerImpl(uint16_t dmemIn, uint16_t count, uint8_t swapLR, uint8_t x0, uint8_t x1,
                   uint8_t x2, uint8_t x3, uint32_t dmemDests, uint32_t opBits);
void aInterleaveImpl(uint16_t outAddr, uint16_t inL, uint16_t inR, uint16_t count);
void aInterlImpl(uint16_t in_addr, uint16_t out_addr, uint16_t n_samples);
void aMixImpl(uint16_t count, int16_t gain, uint16_t in_addr, uint16_t out_addr);
void aFilterImpl(uint8_t flags, uint16_t countOrBuf, int16_t* state_or_filter);
void aHiLoGainImpl(uint8_t g, uint16_t count, uint16_t dmemIn);
void aDuplicateImpl(uint16_t count, uint16_t in_addr, uint16_t out_addr);
void aAddMixerImpl(uint16_t count, uint16_t in_addr, uint16_t out_addr, uint16_t a4);
void aS8DecImpl(uint8_t flags, ADPCM_STATE state);
void aResampleZohImpl(uint16_t pitch, uint16_t startFract);
void aUnkCmd3Impl(uint16_t a1, uint16_t a2, uint16_t a3);
void aUnkCmd19Impl(uint8_t f, uint16_t count, uint16_t out_addr, uint16_t in_addr);

/* ---- Macro redirections (strip pkt, call Impl directly) ---- */

#define aSegment(pkt, s, b) \
    do { } while (0)

#define aClearBuffer(pkt, d, c) \
    aClearBufferImpl(d, c)

#define aLoadBuffer(pkt, s, d, c, copy) \
    aLoadBufferImpl(s, d, c, copy)

#define aSaveBuffer(pkt, s, d, c) \
    aSaveBufferImpl(s, (int16_t*)(d), c)

#define aLoadADPCM(pkt, c, d) \
    aLoadADPCMImpl(c, d)

#define aSetBuffer(pkt, f, i, o, c) \
    aSetBufferImpl(f, i, o, c)

#define aDMEMMove(pkt, i, o, c) \
    aDMEMMoveImpl(i, o, c)

#define aSetLoop(pkt, a) \
    aSetLoopImpl(a)

#define aADPCMdec(pkt, f, s) \
    aADPCMdecImpl(f, s)

#define aResample(pkt, f, p, s) \
    aResampleImpl(f, p, s)

#define aEnvSetup1(pkt, a, b, c, d) \
    aEnvSetup1Impl(a, b, c, d)

#define aEnvSetup2(pkt, volLeft, volRight) \
    aEnvSetup2Impl(volLeft, volRight)

/* OoT aEnvMixer: 10 params (pkt, dmemi, count, swapLR, x0, x1, x2, x3, m, bits) */
#define aEnvMixer(pkt, dmemi, count, swapLR, x0, x1, x2, x3, m, bits) \
    aEnvMixerImpl(dmemi, count, swapLR, x0, x1, x2, x3, m, bits)

/* OoT aInterleave: 5 params (pkt, o, l, r, c) */
#define aInterleave(pkt, o, l, r, c) \
    aInterleaveImpl(o, l, r, c)

#define aInterl(pkt, dmemi, dmemo, count) \
    aInterlImpl(dmemi, dmemo, count)

#define aMix(pkt, f, g, i, o) \
    aMixImpl(f, g, i, o)

#define aFilter(pkt, f, countOrBuf, addr) \
    aFilterImpl(f, countOrBuf, (int16_t*)(addr))

/* OoT aHiLoGain: 5 params (pkt, gain, count, dmem, a4) */
#define aHiLoGain(pkt, gain, count, dmem, a4) \
    aHiLoGainImpl(gain, count, dmem)

#define aDuplicate(pkt, c, s, d) \
    aDuplicateImpl(c, s, d)

/* OoT aAddMixer: 5 params (pkt, count, dmemi, dmemo, a4) */
#define aAddMixer(pkt, count, dmemi, dmemo, a4) \
    aAddMixerImpl(count, dmemi, dmemo, a4)

#define aS8Dec(pkt, f, s) \
    aS8DecImpl(f, s)

#define aResampleZoh(pkt, pitch, startFract) \
    aResampleZohImpl(pitch, startFract)

#define aUnkCmd3(pkt, a1, a2, a3) \
    aUnkCmd3Impl(a1, a2, a3)

#define aUnkCmd19(pkt, a1, a2, a3, a4) \
    aUnkCmd19Impl(a1, a2, a3, a4)

/* These are unused on DC but synthesis.c may reference them */
#define aSetVolume(pkt, f, v, t, r)   do { } while (0)
#define aSetVolume32(pkt, f, v, tr)    do { } while (0)
#define aPoleFilter(pkt, f, g, s)      do { } while (0)
