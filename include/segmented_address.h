#ifndef SEGMENTED_ADDRESS_H
#define SEGMENTED_ADDRESS_H

#include "ultra64.h"
#include "stdint.h"

extern uintptr_t gSegments[NUM_SEGMENTS];

#ifdef LINUX

static inline void* segmented_to_virtual(void* addr) {
    uintptr_t uip_addr = (uintptr_t)addr;
    size_t segment = uip_addr >> 24;
    
    // Valid N64 segments are 0x00-0x0F
    // If segment > 0x0F, this is already a valid PC pointer - pass through unchanged
    // This handles overlay-local static Vtx[]/Gfx[] arrays that are already direct pointers
    if (segment > 0x0F) {
        return addr;
    }
    
    size_t offset = uip_addr & 0x00FFFFFF;
    return (void*)(gSegments[segment] + offset);
}

#define SEGMENTED_TO_VIRTUAL(addr) segmented_to_virtual((void*)(addr))

#else

#define SEGMENTED_TO_VIRTUAL(addr) (void*)(gSegments[SEGMENT_NUMBER(addr)] + SEGMENT_OFFSET(addr) + K0BASE)

#endif

#endif