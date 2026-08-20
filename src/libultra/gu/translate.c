#include "ultra64.h"

void guTranslateF(f32 m[4][4], f32 x, f32 y, f32 z) {
#ifdef __DREAMCAST__
    shz_mat4x4_init_translation((shz_mat4x4_t*)m, x, y, z);
#else
    guMtxIdentF(m);

    m[3][0] = x;
    m[3][1] = y;
    m[3][2] = z;
#endif
}

void guTranslate(Mtx* m, f32 x, f32 y, f32 z) {
#ifdef __DREAMCAST__
    guTranslateF((void*)m, x, y, z);
#else
    f32 mf[4][4];

    guTranslateF(mf, x, y, z);

    guMtxF2L(mf, m);
#endif
}
