#ifndef AICA_SYNTH_H
#define AICA_SYNTH_H

/*
 * AICA hardware-mixed voice driver. Replaces the software synthesis/mixer
 * (synthesis.c AudioSynth_DoOneAudioUpdate path + mixer.c) on Dreamcast by
 * mapping each active N64 NoteSubEu to an AICA hardware channel via the KOS
 * command queue.
 *
 * Integration:
 *   - AicaSynth_Init()   : call once during audio init (after AudioRom load,
 *                          after gAudioCtx.numNotes is known). Uploads the
 *                          synthetic wavetables to ARAM and resets state.
 *   - AicaSynth_Update() : call once per audio frame from AudioSynth_Update,
 *                          AFTER the AudioSeq_ProcessSequences tick loop has
 *                          finalized the NoteSubEu set.
 *   - AicaSynth_RefreshActive(tick) : call inside the tick loop after each
 *                          func_800DB03C(tick), to keep sub-frame pan/vol/freq
 *                          resolution for fast pan/tremolo/vibrato.
 */

void AicaSynth_Init(void);
void AicaSynth_Update(void);
void AicaSynth_RefreshActive(s32 tick);

/* Set by the asset loader: base pointer of the resident adpcm_pool.bin blob. */
extern const unsigned char* gAicaAdpcmPoolBase;

#endif /* AICA_SYNTH_H */
