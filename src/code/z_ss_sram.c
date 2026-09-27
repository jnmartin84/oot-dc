#include "ultra64.h"
#include "printf.h"
#include "ss_sram.h"

typedef struct SsSramContext {
    /* 0x00 */ OSPiHandle piHandle;
    /* 0x74 */ OSIoMesg ioMesg;
    /* 0x8C */ OSMesgQueue msgQueue;
} SsSramContext; // size = 0xA4

SsSramContext sSsSramContext = { 0 };

void SsSram_Init(s32 addr, u8 handleType, u8 handleDomain, u8 handleLatency, u8 handlePageSize, u8 handleRelDuration,
                 u8 handlePulse, u32 handleSpeed) {
    u32 prevInt;
    OSPiHandle* handle = &sSsSramContext.piHandle;

    if ((u32)OS_PHYSICAL_TO_K1(addr) != (*handle).baseAddress) {
        sSsSramContext.piHandle.type = handleType;
        (*handle).baseAddress = (u32)OS_PHYSICAL_TO_K1(addr);
        sSsSramContext.piHandle.latency = handleLatency;
        sSsSramContext.piHandle.pulse = handlePulse;
        sSsSramContext.piHandle.pageSize = handlePageSize;
        sSsSramContext.piHandle.relDuration = handleRelDuration;
        sSsSramContext.piHandle.domain = handleDomain;
        sSsSramContext.piHandle.speed = handleSpeed;

        bzero(&sSsSramContext.piHandle.transferInfo, sizeof(__OSTranxInfo));

        prevInt = __osDisableInt();
        sSsSramContext.piHandle.next = __osPiTable;
        __osPiTable = &sSsSramContext.piHandle;
        __osRestoreInt(prevInt);

        sSsSramContext.ioMesg.hdr.pri = OS_MESG_PRI_NORMAL;
        sSsSramContext.ioMesg.hdr.retQueue = &sSsSramContext.msgQueue;
        sSsSramContext.ioMesg.devAddr = addr;
    }
}

void SsSram_Dma(void* dramAddr, size_t size, s32 direction) {
    OSMesg msg;

    osCreateMesgQueue(&sSsSramContext.msgQueue, &msg, 1);
    sSsSramContext.ioMesg.dramAddr = dramAddr;
    sSsSramContext.ioMesg.size = size;
    osWritebackDCache(dramAddr, size);
    osEPiStartDma(&sSsSramContext.piHandle, &sSsSramContext.ioMesg, direction);
    osRecvMesg(&sSsSramContext.msgQueue, &msg, OS_MESG_BLOCK);
    osInvalDCache(dramAddr, size);
}

#ifdef __DREAMCAST__
#include <kos.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "save.h"
#include "sram.h"
#include "sfx.h"
#include "z_lib.h"
#include "libu64/gfxprint.h"
#include "dreamcast/sram_pack.h"

/*
 * Dreamcast SRAM: a 32 KiB RAM mirror of the N64 save SRAM, backed by one VMU
 * package file (oot.rec) on the first memory card found.
 *
 * The file holds the mirror packed by sram_pack.c (identical backup slots
 * dropped, the rest run-length coded): a few blocks for a fresh card instead of
 * the 66 a raw mirror needs. Files from before packing (a raw 32 KiB mirror)
 * still load, and are replaced by a packed file on the next save.
 *
 * KOS's fs_vmu reads the whole file on open and rewrites the whole file on
 * close, so the mirror is loaded once and each SsSram write is one full
 * rewrite; reads never touch the VMU. Without a memory card the mirror still
 * serves the session; a card inserted later is adopted on the next access
 * unless the mirror holds writes that no card has seen yet.
 *
 * Failures used to go only to the serial console; they now also show a
 * warning on screen (DcSram_DrawStatus, called from GameState_Draw).
 */
#define DC_SRAM_FILE "oot.rec"

_Static_assert(SRAM_SIZE == SRAMPACK_IMAGE_SIZE, "sram_pack image size");
_Static_assert(SRAM_HEADER_SIZE + 0x10 == SRAMPACK_SLOT_BASE, "sram_pack slot base");
_Static_assert(sizeof(SaveContext) + 0x28 == SRAMPACK_SLOT_SIZE, "sram_pack slot size");

static u8 sDcSram[SRAM_SIZE] __attribute__((aligned(32)));
static u8 sDcSramLoaded = 0;  /* mirror initialised (from a VMU, or zeros) */
static u8 sDcSramOnVmu = 0;   /* mirror contents match the VMU file */
static u8 sDcSramDirty = 0;   /* mirror has writes no VMU has stored yet */
static u8 sDcSramDamaged = 0; /* VMU file unreadable: never overwrite it */
static u8 sDcSramUnread = 0;  /* card present, file not read yet: don't overwrite it */

typedef enum DcSramWarning {
    DC_SRAM_WARN_NONE,
    DC_SRAM_WARN_NO_VMU,
    DC_SRAM_WARN_VMU_FULL,
    DC_SRAM_WARN_WRITE_FAILED,
    DC_SRAM_WARN_DAMAGED
} DcSramWarning;

#define DC_SRAM_WARN_FRAMES 120 /* ~6 s at the game's 20 fps */

static DcSramWarning sDcSramWarning = DC_SRAM_WARN_NONE;
static s32 sDcSramWarnFrames = 0;
static u8 sDcSramWarnSfx = 0;       /* play the error sound on the next draw */
static s32 sDcSramNeedBlocks = 0;   /* for DC_SRAM_WARN_VMU_FULL */

static void DcSram_Warn(DcSramWarning warning) {
    sDcSramWarning = warning;
    sDcSramWarnFrames = DC_SRAM_WARN_FRAMES;
    sDcSramWarnSfx = 1;
}

/* VMU icon: ocarina.ico (32x32, 16-colour, 4bpp) from the asset root, i.e. the
 * repo dir over dcload or the disc root (Makefile.dc ships it with -f). Falls
 * back to a solid tile if the file is missing. */
extern const char* gDcFsRoot;
static u8 sDcSramIcon[512];

static maple_device_t* DcSram_Vmu(char* path, size_t pathSize) {
    maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);

    if (dev != NULL) {
        snprintf(path, pathSize, "/vmu/%c%d/%s", 'a' + dev->port, dev->unit, DC_SRAM_FILE);
    }
    return dev;
}

/* The data of a whole VMU package file -> mirror. Returns 1 on success, 0 if
 * the file is damaged. Packed files are decoded; anything else is a raw mirror
 * from before packing (zero-padded if short, as it always was). */
static int DcSram_Parse(u8* file, size_t fileLen, const char* path) {
    vmu_pkg_t pkg;
    int rc;

    /* vmu_pkg_parse checks lengths against fileLen and the CRC, but reads the
     * header before checking that there is one. */
    if (fileLen < sizeof(vmu_hdr_t) || vmu_pkg_parse(file, fileLen, &pkg) < 0) {
        printf("SRAM: %s has no valid VMU package header\n", path);
        return 0;
    }
    if (SramPack_IsPacked(pkg.data, pkg.data_len)) {
        rc = SramPack_Decode(pkg.data, pkg.data_len, sDcSram);
        if (rc < 0) {
            printf("SRAM: %s is damaged (%d)\n", path, rc);
            return 0;
        }
        printf("SRAM: loaded %s (%d byte package)\n", path, (int)pkg.data_len);
    } else {
        size_t n = (pkg.data_len < SRAM_SIZE) ? pkg.data_len : SRAM_SIZE;

        memcpy(sDcSram, pkg.data, n);
        memset(sDcSram + n, 0, SRAM_SIZE - n);
        printf("SRAM: loaded %u raw bytes from %s (packed on the next save)\n", (unsigned)n, path);
    }
    return 1;
}

typedef enum DcSramLoad {
    DC_SRAM_LOAD_NO_VMU, /* no memory card: nothing to adopt yet */
    DC_SRAM_LOAD_OK,     /* mirror reflects the card (blank if no file, or damaged) */
    DC_SRAM_LOAD_RETRY   /* card present but not read (out of memory); try again later */
} DcSramLoad;

/* Whole VMU file -> mirror. The file is opened raw (O_META) and its package
 * header parsed here: fs_vmu's own header handling clamps reads without
 * counting the header, so a bad length in the file could read past its buffer. */
static DcSramLoad DcSram_Load(void) {
    char path[32];
    file_t f;
    ssize_t total;
    ssize_t n;
    u8* file;
    int ok;

    if (DcSram_Vmu(path, sizeof(path)) == NULL) {
        return DC_SRAM_LOAD_NO_VMU;
    }
    f = fs_open(path, O_RDONLY | O_META);
    if (f == FILEHND_INVALID) {
        printf("SRAM: no %s on the VMU yet, starting blank\n", path);
        memset(sDcSram, 0, SRAM_SIZE);
        return DC_SRAM_LOAD_OK;
    }
    total = fs_total(f);
    file = (total > 0) ? malloc(total) : NULL;
    if (file == NULL && total > 0) {
        printf("SRAM: no memory to load %s (%ld bytes)\n", path, (long)total);
        fs_close(f);
        return DC_SRAM_LOAD_RETRY;
    }
    n = (file != NULL) ? fs_read(f, file, total) : -1;
    fs_close(f);

    ok = (n == total) && DcSram_Parse(file, total, path);
    free(file);
    if (!ok) {
        /* Keep the damaged file: the session runs from a blank mirror and
         * saves stay in RAM until the player deletes the file. */
        memset(sDcSram, 0, SRAM_SIZE);
        sDcSramDamaged = 1;
        DcSram_Warn(DC_SRAM_WARN_DAMAGED);
    }
    return DC_SRAM_LOAD_OK;
}

/* Adopts the card's file if possible. A card whose file could not be read
 * (DC_SRAM_LOAD_RETRY) must not be written over from a blank mirror. */
static void DcSram_TryLoad(void) {
    DcSramLoad rc = DcSram_Load();

    sDcSramOnVmu = (rc == DC_SRAM_LOAD_OK);
    sDcSramUnread = (rc == DC_SRAM_LOAD_RETRY);
}

/* Mirror -> whole VMU file, as a proper VMU package holding the packed mirror.
 * Returns DC_SRAM_WARN_NONE on success, otherwise why it failed. */
static DcSramWarning DcSram_Store(void) {
    char path[32];
    file_t f;
    vmu_pkg_t pkg;
    u8* packed;
    size_t packedLen;
    ssize_t n;
    int rc;
    int err;

    if (DcSram_Vmu(path, sizeof(path)) == NULL) {
        return DC_SRAM_WARN_NO_VMU;
    }
    packedLen = SramPack_Encode(sDcSram, NULL);
    packed = malloc(packedLen);
    if (packed == NULL) {
        printf("SRAM: no memory to pack the save (%u bytes)\n", (unsigned)packedLen);
        return DC_SRAM_WARN_WRITE_FAILED;
    }
    SramPack_Encode(sDcSram, packed);

    f = fs_open(path, O_WRONLY | O_TRUNC);
    if (f == FILEHND_INVALID) {
        printf("SRAM: cannot open %s for writing\n", path);
        free(packed);
        return DC_SRAM_WARN_WRITE_FAILED;
    }

    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.desc_short, "Ocarina of Time");      /* <= 15 chars */
    strcpy(pkg.desc_long, "Ocarina of Time save data"); /* <= 31 chars */
    strcpy(pkg.app_id, "OOT-DC");
    pkg.icon_cnt = 1;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.icon_data = sDcSramIcon;
    {
        char icon[64];
        snprintf(icon, sizeof(icon), "%s/ocarina.ico", gDcFsRoot);
        if (vmu_pkg_load_icon(&pkg, icon) < 0) {
            printf("SRAM: %s not loadable, using plain icon\n", icon);
            memset(sDcSramIcon, 0, sizeof(sDcSramIcon));
            pkg.icon_cnt = 1;
            pkg.icon_pal[0] = 0xFFC3; /* ARGB4444 gold */
        }
    }
    pkg.data_len = packedLen; /* fs_vmu recomputes on close */
    pkg.data = packed;
    if (fs_vmu_set_header(f, &pkg) < 0) {
        printf("SRAM: fs_vmu_set_header failed\n");
    }

    n = fs_write(f, packed, packedLen);
    errno = 0;
    rc = fs_close(f); /* fs_vmu writes the file here; returns -1, errno ENOSPC = VMU full */
    err = errno;
    free(packed);
    if (n != (ssize_t)packedLen || rc < 0) {
        printf("SRAM: write to %s failed (wrote %ld, close %d, errno %d)\n", path, (long)n, rc, err);
        if (rc < 0 && err == ENOSPC) {
            /* package header + icon take 2 blocks on top of the data */
            sDcSramNeedBlocks = (packedLen + 511) / 512 + 2;
            return DC_SRAM_WARN_VMU_FULL;
        }
        return DC_SRAM_WARN_WRITE_FAILED;
    }
    printf("SRAM: saved %u packed bytes to %s\n", (unsigned)packedLen, path);
    return DC_SRAM_WARN_NONE;
}

void DcSram_DrawStatus(Gfx** gfxP) {
    GfxPrint printer;
    const char* line1;
    const char* line2;
    char need[32];

    if (sDcSramWarnFrames <= 0) {
        return;
    }
    sDcSramWarnFrames--;
    if (sDcSramWarnSfx) {
        sDcSramWarnSfx = 0;
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
    }

    switch (sDcSramWarning) {
        case DC_SRAM_WARN_NO_VMU:
            line1 = "NO VMU FOUND - NOT SAVED";
            line2 = "SAVE KEPT UNTIL POWER OFF";
            break;
        case DC_SRAM_WARN_VMU_FULL:
            line1 = "VMU FULL - NOT SAVED";
            snprintf(need, sizeof(need), "NEEDS %d FREE BLOCKS", (int)sDcSramNeedBlocks);
            line2 = need;
            break;
        case DC_SRAM_WARN_WRITE_FAILED:
            line1 = "VMU WRITE ERROR - NOT SAVED";
            line2 = "SAVE KEPT UNTIL POWER OFF";
            break;
        case DC_SRAM_WARN_DAMAGED:
            line1 = "OOT FILE ON VMU IS DAMAGED";
            line2 = "DELETE IT IN THE DC MENU";
            break;
        default:
            return;
    }

    GfxPrint_Init(&printer);
    GfxPrint_Open(&printer, *gfxP);
    GfxPrint_SetColor(&printer, 255, 255, 80, 255);
    GfxPrint_SetPos(&printer, 4, 24);
    GfxPrint_Printf(&printer, "%s", line1);
    GfxPrint_SetColor(&printer, 255, 255, 255, 255);
    GfxPrint_SetPos(&printer, 4, 25);
    GfxPrint_Printf(&printer, "%s", line2);
    *gfxP = GfxPrint_Close(&printer);
    GfxPrint_Destroy(&printer);
}
#endif

void SsSram_ReadWrite(s32 addr, void* dramAddr, size_t size, s32 direction) {
    PRINTF("ssSRAMReadWrite:%08x %08x %08x %d\n", addr, dramAddr, size, direction);
#ifndef __DREAMCAST__
    SsSram_Init(addr, DEVICE_TYPE_SRAM, PI_DOMAIN2, 5, 0xD, 2, 0xC, 0);
    SsSram_Dma(dramAddr, size, direction);
#else
    /* Callers pass OS_K1_TO_PHYSICAL(0xA8000000) + offset; undo it the same way
     * (the macro is the identity on this build, a real translation on N64). */
    u32 offset = (u32)addr - (u32)OS_K1_TO_PHYSICAL(0xA8000000);

    if (offset >= SRAM_SIZE || size > SRAM_SIZE - offset) {
        printf("SRAM: out-of-range access offset 0x%X size 0x%X\n", (unsigned)offset, (unsigned)size);
        return;
    }

    if (!sDcSramLoaded) {
        memset(sDcSram, 0, SRAM_SIZE);
        DcSram_TryLoad();
        sDcSramLoaded = 1;
    }

    if (direction == OS_READ) {
        /* A VMU that appeared after boot, or one we could not read yet: adopt
         * its file, unless the mirror holds writes it has never stored. */
        if (!sDcSramOnVmu && !sDcSramDirty) {
            DcSram_TryLoad();
        }
        memcpy(dramAddr, sDcSram + offset, size);
    } else if (direction == OS_WRITE) {
        if (!sDcSramOnVmu && !sDcSramDirty) {
            DcSram_TryLoad(); /* don't clobber an existing file with a blank mirror */
        }
        if (memcmp(sDcSram + offset, dramAddr, size) == 0 && sDcSramOnVmu) {
            return; /* nothing changed (e.g. file-select header rewrites): skip the rewrite */
        }
        memcpy(sDcSram + offset, dramAddr, size);
        if (sDcSramDamaged) {
            /* Never replace a file we could not read; the player deletes it. */
            sDcSramDirty = 1;
            DcSram_Warn(DC_SRAM_WARN_DAMAGED);
            printf("SRAM: VMU file damaged, save kept in RAM for this session only\n");
        } else if (sDcSramUnread) {
            /* The card holds a file we never managed to read. Writing the
             * mirror now would replace it with whatever this session has, so
             * the card stays untouched until the next boot. */
            sDcSramDirty = 1;
            DcSram_Warn(DC_SRAM_WARN_WRITE_FAILED);
            printf("SRAM: VMU file not loaded yet, save kept in RAM for this session only\n");
        } else {
            DcSramWarning result = DcSram_Store();

            if (result == DC_SRAM_WARN_NONE) {
                sDcSramOnVmu = 1;
                sDcSramDirty = 0;
                /* A save is two writes (slot, then backup); the first can fail
                 * where the smaller second fits. Only the outcome counts. */
                if (sDcSramWarning != DC_SRAM_WARN_DAMAGED) {
                    sDcSramWarnFrames = 0;
                    sDcSramWarnSfx = 0;
                }
            } else {
                sDcSramOnVmu = 0;
                sDcSramDirty = 1;
                DcSram_Warn(result);
                printf("SRAM: save kept in RAM for this session only\n");
            }
        }
    }
#endif
}
