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
#include <string.h>
#include "sram.h"

/*
 * Dreamcast SRAM: a 32 KiB RAM mirror of the N64 save SRAM, backed by one VMU
 * package file (oot.rec, 66 blocks) on the first memory card found.
 *
 * KOS's fs_vmu reads the whole file on open and rewrites the whole file on
 * close, so the mirror is loaded once and each SsSram write is one full
 * rewrite; reads never touch the VMU. Without a memory card the mirror still
 * serves the session; a card inserted later is adopted on the next access
 * unless the mirror holds writes that no card has seen yet.
 */
#define DC_SRAM_FILE "oot.rec"

static u8 sDcSram[SRAM_SIZE] __attribute__((aligned(32)));
static u8 sDcSramLoaded = 0; /* mirror initialised (from a VMU, or zeros) */
static u8 sDcSramOnVmu = 0;  /* mirror contents match the VMU file */
static u8 sDcSramDirty = 0;  /* mirror has writes no VMU has stored yet */

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

/* Whole VMU file -> mirror. Returns 1 if a VMU was present (mirror now
 * reflects it, blank if the file does not exist yet), 0 if no VMU. */
static int DcSram_Load(void) {
    char path[32];
    file_t f;
    ssize_t n;

    if (DcSram_Vmu(path, sizeof(path)) == NULL) {
        return 0;
    }
    f = fs_open(path, O_RDONLY);
    if (f == FILEHND_INVALID) {
        printf("SRAM: no %s on the VMU yet, starting blank\n", path);
        memset(sDcSram, 0, SRAM_SIZE);
        return 1;
    }
    n = fs_read(f, sDcSram, SRAM_SIZE);
    fs_close(f);
    if (n < 0) {
        printf("SRAM: read of %s failed (%ld)\n", path, (long)n);
        n = 0;
    }
    if ((size_t)n < SRAM_SIZE) {
        memset(sDcSram + n, 0, SRAM_SIZE - n);
    }
    printf("SRAM: loaded %ld bytes from %s\n", (long)n, path);
    return 1;
}

/* Mirror -> whole VMU file, as a proper VMU package. Returns 1 on success. */
static int DcSram_Store(void) {
    char path[32];
    file_t f;
    vmu_pkg_t pkg;
    ssize_t n;
    int rc;

    if (DcSram_Vmu(path, sizeof(path)) == NULL) {
        return 0;
    }
    f = fs_open(path, O_WRONLY | O_TRUNC);
    if (f == FILEHND_INVALID) {
        printf("SRAM: cannot open %s for writing\n", path);
        return 0;
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
    pkg.data_len = SRAM_SIZE; /* fs_vmu recomputes on close */
    pkg.data = sDcSram;
    if (fs_vmu_set_header(f, &pkg) < 0) {
        printf("SRAM: fs_vmu_set_header failed\n");
    }

    n = fs_write(f, sDcSram, SRAM_SIZE);
    rc = fs_close(f); /* fs_vmu writes the file here; -7 = VMU full */
    if (n != (ssize_t)SRAM_SIZE || rc < 0) {
        printf("SRAM: write to %s failed (wrote %ld, close %d)\n", path, (long)n, rc);
        return 0;
    }
    printf("SRAM: saved to %s\n", path);
    return 1;
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
        sDcSramOnVmu = DcSram_Load();
        sDcSramLoaded = 1;
    }

    if (direction == OS_READ) {
        /* A VMU that appeared after boot: adopt its file, unless the mirror
         * holds writes it has never stored. */
        if (!sDcSramOnVmu && !sDcSramDirty && DcSram_Load()) {
            sDcSramOnVmu = 1;
        }
        memcpy(dramAddr, sDcSram + offset, size);
    } else if (direction == OS_WRITE) {
        if (!sDcSramOnVmu && !sDcSramDirty && DcSram_Load()) {
            sDcSramOnVmu = 1; /* don't clobber an existing file with a blank mirror */
        }
        if (memcmp(sDcSram + offset, dramAddr, size) == 0 && sDcSramOnVmu) {
            return; /* nothing changed (e.g. file-select header rewrites): skip the 66-block rewrite */
        }
        memcpy(sDcSram + offset, dramAddr, size);
        if (DcSram_Store()) {
            sDcSramOnVmu = 1;
            sDcSramDirty = 0;
        } else {
            sDcSramOnVmu = 0;
            sDcSramDirty = 1;
            printf("SRAM: no VMU, save kept in RAM for this session only\n");
        }
    }
#endif
}
