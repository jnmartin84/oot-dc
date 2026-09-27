#ifndef SRAM_PACK_H
#define SRAM_PACK_H

/*
 * Compact container for the 32 KiB SRAM mirror stored on the VMU (oot.rec).
 * Plain C with no KOS or game dependencies so it can be tested on the host.
 *
 * Layout (all multi-byte fields little-endian):
 *   0x00  "OOTZ"
 *   0x04  u8  version (SRAMPACK_VERSION)
 *   0x05  u8  flags   (SRAMPACK_FLAG_RAW: payload is the whole image, uncompressed)
 *   0x06  u8  dupMask (bit i: backup slot i+3 equals slot i and is omitted)
 *   0x07  u8  reserved (0)
 *   0x08  u32 payload length
 *   0x0C  u32 Adler-32 of the decoded SRAMPACK_IMAGE_SIZE image
 *   0x10  payload: the image minus omitted backup slots, PackBits-style RLE
 */

#include <stddef.h>
#include <stdint.h>

#define SRAMPACK_IMAGE_SIZE 0x8000 /* SRAM_SIZE */
#define SRAMPACK_SLOT_BASE 0x20    /* SLOT_OFFSET(0) = SRAM_HEADER_SIZE + 0x10 */
#define SRAMPACK_SLOT_SIZE 0x1450  /* SLOT_SIZE = sizeof(SaveContext) + 0x28 */
#define SRAMPACK_HEADER_SIZE 0x10
#define SRAMPACK_MAX_SIZE (SRAMPACK_HEADER_SIZE + SRAMPACK_IMAGE_SIZE)
#define SRAMPACK_VERSION 1
#define SRAMPACK_FLAG_RAW 0x01

/* Returns the packed size. With out == NULL only the size is computed. The
 * result never exceeds SRAMPACK_MAX_SIZE. */
size_t SramPack_Encode(const uint8_t* image, uint8_t* out);

/* 1 if the buffer starts with the container magic. */
int SramPack_IsPacked(const uint8_t* in, size_t inLen);

/* Decodes into image (SRAMPACK_IMAGE_SIZE bytes). Returns 0 on success, < 0 on a
 * bad magic, version, length or checksum; image is unspecified on failure. */
int SramPack_Decode(const uint8_t* in, size_t inLen, uint8_t* image);

#endif
