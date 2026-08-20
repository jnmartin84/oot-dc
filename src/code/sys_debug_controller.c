#include "sys_debug_controller.h"
#include "stdbool.h"
#include "ultra64/ultratypes.h"
#include "padmgr.h"

u32 gIsCtrlr2Valid = false;

void func_800D31F0(void) {
#ifdef LINUX
    // Force controller 2 valid on Linux so GfxPrint debug text is displayed
    gIsCtrlr2Valid = true;
#else
    gIsCtrlr2Valid = (gPadMgr.validCtrlrsMask & 2) != 0;
#endif
}

void func_800D3210(void) {
#ifndef LINUX
    gIsCtrlr2Valid = false;
#endif
}
