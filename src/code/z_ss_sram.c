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
static char full_fn[20];
char *get_vmu_fn(maple_device_t *vmudev, char *fn) {
	if (fn)
		sprintf(full_fn, "/vmu/%c%d/%s", 'a'+vmudev->port, vmudev->unit, fn);
	else
		sprintf(full_fn, "/vmu/%c%d", 'a'+vmudev->port, vmudev->unit);

	return full_fn;
}
#endif

void SsSram_ReadWrite(s32 addr, void* dramAddr, size_t size, s32 direction) {
    PRINTF("ssSRAMReadWrite:%08x %08x %08x %d\n", addr, dramAddr, size, direction);
#if 1 
//ndef __DREAMCAST__
    SsSram_Init(addr, DEVICE_TYPE_SRAM, PI_DOMAIN2, 5, 0xD, 2, 0xC, 0);
    SsSram_Dma(dramAddr, size, direction);
#else
    file_t eeprom_file = FILEHND_INVALID;
#if 0
    maple_device_t* vmudev = NULL;

    vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if (!vmudev) {
        SsSram_Init(addr, DEVICE_TYPE_SRAM, PI_DOMAIN2, 5, 0xD, 2, 0xC, 0);
        SsSram_Dma(dramAddr, size, direction);
        return;
    }
#endif
    eeprom_file = fs_open(/* get_vmu_fn(vmudev, "oot.rec") */ "/pc/oot.rec", O_RDONLY /* | O_META */);
    if (FILEHND_INVALID == eeprom_file) {
        uint32_t wzero = 0;
#if 0
        vmu_pkg_t pkg_data;
        strcpy(pkg_data.desc_short, "OoT data");
        strcpy(pkg_data.desc_long, "Ocarina of Time savedata");
        strcpy(pkg_data.app_id, "Ocarina of Time");
        pkg_data.icon_cnt = 0;
        pkg_data.icon_anim_speed = 0;
        pkg_data.eyecatch_type = 0;
        pkg_data.data_len = 32768;
        pkg_data.icon_data = NULL;
        pkg_data.eyecatch_data = NULL;
        pkg_data.data = NULL;
#endif
        eeprom_file = fs_open(/* get_vmu_fn(vmudev, "oot.rec") */"/pc/oot.rec", O_RDWR | O_CREAT /* | O_META */);
//        fs_vmu_set_header(eeprom_file, &pkg_data);
        for(int i=0;i<32768;i+=4) {
            fs_write(eeprom_file, &wzero, 4);
        }
        fs_close(eeprom_file);
        eeprom_file = FILEHND_INVALID;
    } else {
        fs_close(eeprom_file);
    }

    eeprom_file = fs_open(/* get_vmu_fn(vmudev, "oot.rec") */"/pc/oot.rec", O_RDWR /* | O_META */);
    if (eeprom_file == FILEHND_INVALID) {
        return;
    }
    fs_seek(eeprom_file, (u32)0xA8000000 - (u32)addr, SEEK_SET);

    if (direction == OS_READ) {
        fs_read(eeprom_file, dramAddr, size);
    } else if (direction == OS_WRITE) {
        fs_write(eeprom_file, dramAddr, size);
    }

    fs_close(eeprom_file);
#endif
}
