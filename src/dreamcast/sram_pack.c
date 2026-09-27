#include "sram_pack.h"

#include <string.h>

/*
 * The payload is a virtual stream: the image with omitted backup slots cut
 * out. The three save files each have a backup copy that is normally
 * identical, and unused slots and most of a save are long runs of 0x00/0xFF,
 * so dropping duplicates and run-length coding the rest is enough.
 *
 * RLE control byte: 0x00-0x7F = c+1 literal bytes follow,
 *                   0x80-0xFF = the next byte repeated c-125 times (3..130).
 */

#define MAX_LITERAL 128
#define MIN_RUN 3
#define MAX_RUN 130

typedef struct {
    uint32_t start;
    uint32_t len;
} Span;

typedef struct {
    Span spans[4];
    int count;
    uint32_t total;
} Stream;

static uint32_t SlotOffset(int i) {
    return SRAMPACK_SLOT_BASE + (uint32_t)i * SRAMPACK_SLOT_SIZE;
}

/* The image minus backup slots 3..5 whose bit is set in dupMask. */
static void Stream_Init(Stream* s, uint8_t dupMask) {
    uint32_t pos = 0;
    int i;

    s->count = 0;
    s->total = 0;
    for (i = 0; i < 3; i++) {
        uint32_t slot = SlotOffset(i + 3);

        if (dupMask & (1 << i)) {
            s->spans[s->count].start = pos;
            s->spans[s->count].len = slot - pos;
            s->total += slot - pos;
            s->count++;
            pos = slot + SRAMPACK_SLOT_SIZE;
        }
    }
    s->spans[s->count].start = pos;
    s->spans[s->count].len = SRAMPACK_IMAGE_SIZE - pos;
    s->total += SRAMPACK_IMAGE_SIZE - pos;
    s->count++;
}

static uint32_t Stream_Map(const Stream* s, uint32_t i) {
    int k;

    for (k = 0; k < s->count; k++) {
        if (i < s->spans[k].len) {
            return s->spans[k].start + i;
        }
        i -= s->spans[k].len;
    }
    return 0; /* unreachable for i < total */
}

static uint32_t Adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;

    while (n--) {
        a = (a + *p++) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void Put32(uint8_t* p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static uint32_t Get32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t RunLength(const uint8_t* image, const Stream* s, uint32_t i) {
    uint8_t b = image[Stream_Map(s, i)];
    uint32_t n = 1;

    while (i + n < s->total && n < MAX_RUN && image[Stream_Map(s, i + n)] == b) {
        n++;
    }
    return n;
}

/* RLE-codes the stream into out (NULL = count only). Returns the byte count. */
static size_t Rle_Encode(const uint8_t* image, const Stream* s, uint8_t* out) {
    size_t n = 0;
    uint32_t i = 0;

    while (i < s->total) {
        uint32_t run = RunLength(image, s, i);

        if (run >= MIN_RUN) {
            if (out != NULL) {
                out[n] = (uint8_t)(run + 125);
                out[n + 1] = image[Stream_Map(s, i)];
            }
            n += 2;
            i += run;
        } else {
            uint32_t j = i;
            uint32_t k;

            while (j < s->total && (j - i) < MAX_LITERAL) {
                if (j + MIN_RUN <= s->total && RunLength(image, s, j) >= MIN_RUN) {
                    break;
                }
                j++;
            }
            if (out != NULL) {
                out[n] = (uint8_t)(j - i - 1);
                for (k = i; k < j; k++) {
                    out[n + 1 + (k - i)] = image[Stream_Map(s, k)];
                }
            }
            n += 1 + (j - i);
            i = j;
        }
    }
    return n;
}

static int Rle_Decode(const uint8_t* in, size_t inLen, const Stream* s, uint8_t* image) {
    size_t p = 0;
    uint32_t i = 0;

    while (p < inLen) {
        uint8_t c = in[p++];

        if (c < 0x80) {
            uint32_t len = (uint32_t)c + 1;

            if (p + len > inLen || i + len > s->total) {
                return -1;
            }
            while (len--) {
                image[Stream_Map(s, i++)] = in[p++];
            }
        } else {
            uint32_t len = (uint32_t)c - 125;

            if (p >= inLen || i + len > s->total) {
                return -1;
            }
            while (len--) {
                image[Stream_Map(s, i++)] = in[p];
            }
            p++;
        }
    }
    return (i == s->total) ? 0 : -1;
}

static uint8_t DupMask(const uint8_t* image) {
    uint8_t mask = 0;
    int i;

    for (i = 0; i < 3; i++) {
        if (memcmp(image + SlotOffset(i), image + SlotOffset(i + 3), SRAMPACK_SLOT_SIZE) == 0) {
            mask |= 1 << i;
        }
    }
    return mask;
}

size_t SramPack_Encode(const uint8_t* image, uint8_t* out) {
    Stream s;
    uint8_t dupMask = DupMask(image);
    uint8_t flags = 0;
    size_t payloadLen;

    Stream_Init(&s, dupMask);
    payloadLen = Rle_Encode(image, &s, NULL);
    if (payloadLen >= SRAMPACK_IMAGE_SIZE) {
        flags = SRAMPACK_FLAG_RAW;
        dupMask = 0;
        payloadLen = SRAMPACK_IMAGE_SIZE;
    }

    if (out != NULL) {
        memcpy(out, "OOTZ", 4);
        out[4] = SRAMPACK_VERSION;
        out[5] = flags;
        out[6] = dupMask;
        out[7] = 0;
        Put32(out + 8, (uint32_t)payloadLen);
        Put32(out + 12, Adler32(image, SRAMPACK_IMAGE_SIZE));
        if (flags & SRAMPACK_FLAG_RAW) {
            memcpy(out + SRAMPACK_HEADER_SIZE, image, SRAMPACK_IMAGE_SIZE);
        } else {
            Rle_Encode(image, &s, out + SRAMPACK_HEADER_SIZE);
        }
    }
    return SRAMPACK_HEADER_SIZE + payloadLen;
}

int SramPack_IsPacked(const uint8_t* in, size_t inLen) {
    return inLen >= SRAMPACK_HEADER_SIZE && memcmp(in, "OOTZ", 4) == 0;
}

int SramPack_Decode(const uint8_t* in, size_t inLen, uint8_t* image) {
    Stream s;
    uint8_t flags, dupMask;
    size_t payloadLen;
    int i;

    if (!SramPack_IsPacked(in, inLen)) {
        return -1;
    }
    if (in[4] != SRAMPACK_VERSION) {
        return -2;
    }
    flags = in[5];
    dupMask = in[6];
    payloadLen = Get32(in + 8);
    if (payloadLen > SRAMPACK_IMAGE_SIZE || SRAMPACK_HEADER_SIZE + payloadLen > inLen || (dupMask & ~7)) {
        return -3;
    }

    if (flags & SRAMPACK_FLAG_RAW) {
        if (payloadLen != SRAMPACK_IMAGE_SIZE) {
            return -3;
        }
        memcpy(image, in + SRAMPACK_HEADER_SIZE, SRAMPACK_IMAGE_SIZE);
    } else {
        Stream_Init(&s, dupMask);
        if (Rle_Decode(in + SRAMPACK_HEADER_SIZE, payloadLen, &s, image) < 0) {
            return -4;
        }
        for (i = 0; i < 3; i++) {
            if (dupMask & (1 << i)) {
                memcpy(image + SlotOffset(i + 3), image + SlotOffset(i), SRAMPACK_SLOT_SIZE);
            }
        }
    }

    if (Adler32(image, SRAMPACK_IMAGE_SIZE) != Get32(in + 12)) {
        return -5;
    }
    return 0;
}
