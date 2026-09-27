/*
 * Host test for src/dreamcast/sram_pack.c, the VMU save codec.
 * Run with: make -f Makefile.dc test-sram
 *
 * Round-trips representative and random SRAM images, checks that damaged
 * containers are rejected, and prints the VMU block count of typical saves.
 */
#include "sram_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SIZE SRAMPACK_IMAGE_SIZE

static uint8_t sImage[IMAGE_SIZE];
static uint8_t sPacked[SRAMPACK_MAX_SIZE + 512];
static uint8_t sDecoded[IMAGE_SIZE];
static unsigned sSeed = 12345;
static int sFailures = 0;

static unsigned Rand(void) {
    sSeed = sSeed * 1103515245u + 12345u;
    return sSeed >> 16;
}

#define CHECK(cond, ...)                   \
    do {                                   \
        if (!(cond)) {                     \
            printf("FAIL: " __VA_ARGS__);  \
            printf("\n");                  \
            sFailures++;                   \
        }                                  \
    } while (0)

/* VMU blocks for the file: packed data rounded up, plus 2 for the package header and icon. */
static int Blocks(size_t len) {
    return (int)((len + 511) / 512) + 2;
}

static uint8_t* Slot(int i) {
    return sImage + SRAMPACK_SLOT_BASE + i * SRAMPACK_SLOT_SIZE;
}

static void CopySlot(int from, int to) {
    memcpy(Slot(to), Slot(from), SRAMPACK_SLOT_SIZE);
}

/* A plausible save slot: mostly zero, some 0xFF runs, scattered values. */
static void FillSlot(int i, unsigned densityPercent) {
    uint8_t* p = Slot(i);
    int k;

    for (k = 0; k < SRAMPACK_SLOT_SIZE; k++) {
        p[k] = (Rand() % 100 < densityPercent) ? (uint8_t)Rand() : 0;
    }
    memset(p + 0x74, 0xFF, 24);
    memset(p + 0x100, 0xFF, 40);
    memcpy(p + 0x1C, "ZELDAZ", 6);
}

/* Packs sImage, checks both encode passes agree, and unpacks it again (with the
 * zero padding a VMU file adds after the data). Returns the packed size. */
static size_t RoundTrip(const char* name, int report) {
    size_t sizeOnly = SramPack_Encode(sImage, NULL);
    size_t len = SramPack_Encode(sImage, sPacked);
    size_t padded = (len + 511) & ~(size_t)511;
    int rc;

    CHECK(sizeOnly == len, "%s: size pass %zu != encode %zu", name, sizeOnly, len);
    CHECK(len <= SRAMPACK_MAX_SIZE, "%s: %zu bytes exceeds the maximum", name, len);
    memset(sPacked + len, 0, padded - len);
    memset(sDecoded, 0xAA, IMAGE_SIZE);
    rc = SramPack_Decode(sPacked, padded, sDecoded);
    CHECK(rc == 0, "%s: decode returned %d", name, rc);
    CHECK(memcmp(sImage, sDecoded, IMAGE_SIZE) == 0, "%s: decoded image differs", name);
    if (report) {
        printf("  %-38s %6zu bytes = %2d blocks\n", name, len, Blocks(len));
    }
    return len;
}

static void SetPayloadLen(uint32_t v) {
    sPacked[8] = v;
    sPacked[9] = v >> 8;
    sPacked[10] = v >> 16;
    sPacked[11] = v >> 24;
}

static uint32_t PayloadLen(void) {
    return sPacked[8] | (sPacked[9] << 8) | (sPacked[10] << 16) | ((uint32_t)sPacked[11] << 24);
}

static void TestRepresentative(void) {
    int i;

    printf("Representative images:\n");
    memset(sImage, 0, IMAGE_SIZE);
    RoundTrip("blank", 1);

    memset(sImage, 0xFF, IMAGE_SIZE);
    RoundTrip("all 0xFF", 1);

    for (i = 0; i < IMAGE_SIZE; i++) {
        sImage[i] = Rand();
    }
    RoundTrip("random (stored raw)", 1);
    CHECK(sPacked[5] & SRAMPACK_FLAG_RAW, "random image should fall back to raw");

    memset(sImage, 0, IMAGE_SIZE);
    memcpy(sImage, "\x00\x00\x00\x98\x09\x10\x21ZELDA", 12);
    FillSlot(0, 15);
    CopySlot(0, 3);
    RoundTrip("1 file, backup matches", 1);
    CHECK(sPacked[6] == 7, "all three backups should be deduplicated");

    /* A save writes the slot, then its backup: the first write sees them differ. */
    FillSlot(0, 15);
    RoundTrip("1 file, mid-save (backup stale)", 1);
    CHECK(sPacked[6] == 6, "slot 0 backup should not be deduplicated while stale");
    CopySlot(0, 3);
    RoundTrip("1 file, save complete", 1);

    FillSlot(0, 40);
    FillSlot(1, 40);
    FillSlot(2, 40);
    CopySlot(0, 3);
    CopySlot(1, 4);
    CopySlot(2, 5);
    RoundTrip("3 files, dense (synthetic)", 1);
}

static void TestFuzz(void) {
    int t;
    int k;
    int s;
    int i;

    for (t = 0; t < 3000; t++) {
        int regions = Rand() % 40;

        memset(sImage, (t & 1) ? 0xFF : 0, IMAGE_SIZE);
        for (k = 0; k < regions; k++) {
            int at = Rand() % IMAGE_SIZE;
            int len = Rand() % 300;
            int mode = Rand() % 3;

            for (i = at; i < at + len && i < IMAGE_SIZE; i++) {
                sImage[i] = (mode == 0) ? (uint8_t)Rand() : (mode == 1) ? (uint8_t)k : (uint8_t)(i & 1);
            }
        }
        for (s = 0; s < 3; s++) {
            if (Rand() & 1) {
                CopySlot(s, s + 3);
            }
        }
        RoundTrip("fuzz", 0);
    }
    printf("Fuzz: 3000 random images round-tripped\n");
}

static void TestRejects(void) {
    size_t len;
    size_t padded;
    uint32_t payloadLen;
    int t;
    int missed = 0;

    printf("Damaged containers:\n");

    memset(sImage, 0, IMAGE_SIZE);
    FillSlot(1, 30);
    CopySlot(1, 4);
    len = SramPack_Encode(sImage, sPacked);
    padded = (len + 511) & ~(size_t)511;
    memset(sPacked + len, 0, padded - len);

    sPacked[20] ^= 0x5A;
    CHECK(SramPack_Decode(sPacked, padded, sDecoded) != 0, "flipped payload byte not detected");
    sPacked[20] ^= 0x5A;

    CHECK(SramPack_Decode(sPacked, len - 1, sDecoded) != 0, "truncated container not detected");

    sPacked[4] = 9;
    CHECK(SramPack_Decode(sPacked, padded, sDecoded) == -2, "unknown version not rejected");
    sPacked[4] = SRAMPACK_VERSION;

    payloadLen = PayloadLen();
    SetPayloadLen(padded); /* claims more than the file holds */
    CHECK(SramPack_Decode(sPacked, padded, sDecoded) == -3, "length past end of data not rejected");
    SetPayloadLen(SRAMPACK_IMAGE_SIZE + 1);
    CHECK(SramPack_Decode(sPacked, padded, sDecoded) == -3, "length over image size not rejected");
    SetPayloadLen(payloadLen);

    sPacked[6] = 0x08; /* dupMask bit for a slot that doesn't exist */
    CHECK(SramPack_Decode(sPacked, padded, sDecoded) == -3, "invalid dupMask not rejected");

    memset(sDecoded, 0, 16);
    CHECK(!SramPack_IsPacked(sDecoded, 16), "raw (pre-packing) image mistaken for packed");

    /* Damage in the middle of the payload, as a bad VMU block would. */
    for (t = 0; t < 500; t++) {
        memset(sImage, 0, IMAGE_SIZE);
        for (int k = 0; k < 20 + t % 200; k++) {
            sImage[Rand() % IMAGE_SIZE] = Rand();
        }
        len = SramPack_Encode(sImage, sPacked);
        padded = (len + 511) & ~(size_t)511;
        memset(sPacked + len, 0, padded - len);
        sPacked[SRAMPACK_HEADER_SIZE + PayloadLen() / 2] ^= 0xA5;
        if (SramPack_Decode(sPacked, padded, sDecoded) == 0) {
            missed++;
        }
    }
    CHECK(missed == 0, "%d of 500 mid-payload corruptions not detected", missed);
    printf("  all damage cases rejected\n");
}

int main(void) {
    TestRepresentative();
    TestFuzz();
    TestRejects();
    printf("%s (%d failure%s)\n", sFailures ? "FAILED" : "PASSED", sFailures, sFailures == 1 ? "" : "s");
    return sFailures != 0;
}
