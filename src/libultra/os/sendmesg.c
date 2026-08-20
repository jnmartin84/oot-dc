#include "ultra64.h"

#include "ultra64.h"

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
#ifdef LINUX
    if (mq == NULL) {
        return -1;
    }
    
    if (MQ_IS_FULL(mq)) {
        return -1;  // Can't block on PC
    }

    u32 index = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[index] = msg;
    mq->validCount++;

    return 0;
#else
    register u32 prevInt = __osDisableInt();
    register u32 index;

    while (MQ_IS_FULL(mq)) {
        if (flag == OS_MESG_BLOCK) {
            __osRunningThread->state = OS_STATE_WAITING;
            __osEnqueueAndYield(&mq->fullqueue);
        } else {
            __osRestoreInt(prevInt);
            return -1;
        }
    }

    index = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[index] = msg;
    mq->validCount++;

    if (mq->mtqueue->next != NULL) {
        osStartThread(__osPopThread(&mq->mtqueue));
    }

    __osRestoreInt(prevInt);

    return 0;
#endif
}