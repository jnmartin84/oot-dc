#ifndef AUDIO_DC_H
#define AUDIO_DC_H

#include "audio_api.h"

extern struct AudioAPI audio_dc;

bool audio_dc_init(void);
void audio_dc_play(uint8_t* bufL, uint8_t* bufR, size_t len);
int audio_dc_get_buffered_bytes(void);
void audio_dc_start_thread(void);

#endif
