
#include <kos.h>
#include <dc/sound/stream.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

bool audio_dc_init(void) {
    return snd_stream_init();
}

 
/* Forward declare - defined in audio engine (thread.c) */
struct AudioTask;
extern struct AudioTask* AudioThread_Update(void);

static volatile uint64_t sAudioVblTicker = 0;

static void audio_vblank_handler(uint32_t code, void* data) {
    (void)code; (void)data;
    sAudioVblTicker++;
    genwait_wake_one((void*)&sAudioVblTicker);
}

// Set to 1 to disable audio processing
volatile int g_audio_disabled = 0;

static void* dc_audio_thread(void* arg) {
    (void)arg;
    uint64_t lastTick = sAudioVblTicker;
#if 0
    uint64_t audioTimeAccum = 0;
    uint32_t audioFrameCount = 0;
    uint64_t audioTimeMax = 0;
    uint64_t lastReport = timer_us_gettime64();
#endif
    if (g_audio_disabled)
        return NULL;

    while (1) {
        while (sAudioVblTicker <= lastTick)
#if KOS_VERSION_BELOW(2, 2, 3)
            genwait_wait((void*)&sAudioVblTicker, NULL, 5, NULL);
#else
            genwait_wait((void*)&sAudioVblTicker, NULL, 5);
#endif
        lastTick = sAudioVblTicker;

#if 0
        if (g_audio_disabled)
            continue;

        uint64_t t0 = timer_us_gettime64();
#endif

        AudioThread_Update();

#if 0
        uint64_t dt = timer_us_gettime64() - t0;

        audioTimeAccum += dt;
        audioFrameCount++;
        if (dt > audioTimeMax) audioTimeMax = dt;
        /* Print every 2 seconds */
        if (timer_us_gettime64() - lastReport >= 2000000) {
            uint32_t avg = audioFrameCount ? (uint32_t)(audioTimeAccum / audioFrameCount) : 0;
            int buffered = audio_dc_get_buffered_bytes();
            printf("AUDIO: avg=%uus max=%uus frames=%u buf=%d bytes\n",
                   (unsigned)avg, (unsigned)audioTimeMax, (unsigned)audioFrameCount, buffered);
            audioTimeAccum = 0;
            audioFrameCount = 0;
            audioTimeMax = 0;
            lastReport = timer_us_gettime64();
        }
#endif
    }
    return NULL;
}

void audio_dc_start_thread(void) {
    vblank_handler_add(&audio_vblank_handler, NULL);

    kthread_attr_t attr;
    attr.create_detached = 1;
    attr.stack_size = 16384;
    attr.stack_ptr = NULL;
    attr.prio = 2;
    attr.label = "AUDIO";
    thd_create_ex(&attr, &dc_audio_thread, NULL);

    printf("DC Audio: Audio thread started (60Hz vblank-driven)\n");
}

#if 0
#include "dcaudio/audio_dc.h"

/* OoT runs at 32kHz, reduced to 26.8kHz on DC for performance */
#define DC_AUDIO_FREQUENCY 26800
#define DC_AUDIO_CHANNELS 2
#define DC_STEREO_AUDIO (DC_AUDIO_CHANNELS == 2)

#define RING_BUFFER_MAX_BYTES 8192

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

/* Sound stream handle */
static volatile snd_stream_hnd_t shnd = SND_STREAM_INVALID;
static uint8_t __attribute__((aligned(4096))) cb_buf_internal[2][RING_BUFFER_MAX_BYTES];
static bool audio_started = false;

typedef struct {
    uint8_t* buf;
    uint32_t cap;
    uint32_t head;
    uint32_t tail;
} ring_t;

static ring_t __attribute__((aligned(32))) cb_ring[2];
static ring_t* r[2] = { &cb_ring[0], &cb_ring[1] };

static bool cb_init(int N, size_t capacity) {
    r[N]->cap = 1u << (32 - __builtin_clz(capacity - 1));
    r[N]->buf = cb_buf_internal[N];
    r[N]->head = 0;
    r[N]->tail = 0;
    return true;
}

static void cb_write_data(int N, const void* src, size_t n) {
    uint32_t head = r[N]->head;
    uint32_t tail = r[N]->tail;
    uint32_t free = r[N]->cap - (head - tail);
    if (n > free) return;

    uint32_t idx = head & (r[N]->cap - 1);
    uint32_t first = MIN(n, r[N]->cap - idx);
    if (first) memcpy(r[N]->buf + idx, src, first);
    if (n - first) memcpy(r[N]->buf, (uint8_t*)src + first, n - first);
    r[N]->head = head + n;
}

static void cb_read_data(int N, void* dst, size_t n) {
    uint32_t tail = r[N]->tail;
    uint32_t head = r[N]->head;
    uint32_t avail = head - tail;
    if (n > avail) {
        memset(dst, 0, n);
        return;
    }
    uint32_t idx = tail & (r[N]->cap - 1);
    uint32_t first = MIN(n, r[N]->cap - idx);
    memcpy(dst, r[N]->buf + idx, first);
    if (n - first) memcpy((uint8_t*)dst + first, r[N]->buf, n - first);
    r[N]->tail = tail + n;
}

/* KOS stream callback: AICA requests more samples */
static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t rr, size_t req) {
    (void)hnd;
    cb_read_data(0, (void*)l, req >> 1);
    cb_read_data(1, (void*)rr, req >> 1);
    return req;
}

bool audio_dc_init(void) {
    if (snd_stream_init()) {
        printf("AICA INIT FAILURE!\n");
        return false;
    }

    thd_set_hz(300);

    memset(cb_buf_internal, 0, sizeof(cb_buf_internal));

    if (!cb_init(0, RING_BUFFER_MAX_BYTES)) {
        printf("CB INIT FAILURE!\n");
        return false;
    }
    if (!cb_init(1, RING_BUFFER_MAX_BYTES)) {
        printf("CB INIT FAILURE!\n");
        return false;
    }

    printf("DC Audio: Initialized. %d Hz, ring buf: %u bytes/ch\n",
           DC_AUDIO_FREQUENCY, (unsigned)RING_BUFFER_MAX_BYTES);

    shnd = snd_stream_alloc(NULL, 4096);
    if (shnd == SND_STREAM_INVALID) {
        printf("SND: Stream allocation failure!\n");
        return false;
    }
    snd_stream_set_callback_direct(shnd, audio_cb);
    snd_stream_volume(shnd, 160);

    printf("DC Audio: Sound init complete!\n");
    return true;
}

void audio_dc_play(uint8_t* bufL, uint8_t* bufR, size_t len) {
    cb_write_data(0, bufL, len);
    cb_write_data(1, bufR, len);

    if (!audio_started) {
        audio_started = true;
        snd_stream_start(shnd, DC_AUDIO_FREQUENCY, DC_STEREO_AUDIO);
    }

    if (audio_started) {
        snd_stream_poll(shnd);
    }
}

int audio_dc_get_buffered_bytes(void) {
    /* Return bytes buffered in left channel ring buffer */
    return (int)(r[0]->head - r[0]->tail);
}

static int audio_dc_buffered(void) {
    return 2176;
}

static int audio_dc_get_desired_buffered(void) {
    return 2200;
}

static void audio_dc_play_api(uint8_t* bufL, uint8_t* bufR, size_t len) {
    audio_dc_play(bufL, bufR, len);
}

struct AudioAPI audio_dc = {
    audio_dc_init,
    audio_dc_buffered,
    audio_dc_get_desired_buffered,
    (void (*)(uint8_t*, uint8_t*, size_t))audio_dc_play_api
};

#endif