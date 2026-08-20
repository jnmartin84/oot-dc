#include "ultra64.h"

void osSetThreadPri(OSThread* thread, OSPri pri) {
#ifdef LINUX
    if (thread == NULL) {
        thread = __osRunningThread;
    }
    if (thread != NULL) {
        thread->priority = pri;
    }
#else
    register u32 prevInt = __osDisableInt();

    if (thread == NULL) {
        thread = __osRunningThread;
    }

    if (thread->priority != pri) {
        thread->priority = pri;
        if (thread != __osRunningThread && thread->state != OS_STATE_STOPPED) {
            __osDequeueThread(thread->queue, thread);
            __osEnqueueThread(thread->queue, thread);
        }
        if (__osRunningThread->priority < __osRunQueue->priority) {
            __osRunningThread->state = OS_STATE_RUNNABLE;
            __osEnqueueAndYield(&__osRunQueue);
        }
    }

    __osRestoreInt(prevInt);
#endif
}