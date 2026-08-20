#include "ultra64.h"

OSId osGetThreadId(OSThread* thread) {
#ifdef LINUX
    return 1;
#else
    if (thread == NULL) {
        thread = __osRunningThread;
    }
    return thread->id;
#endif
}