#include "ultra64.h"

void guRotateF(f32 m[4][4], f32 a, f32 x, f32 y, f32 z) {
#ifdef __DREAMCAST__
    shz_mat4x4_init_rotation((shz_mat4x4_t*)m, a, x, y, z);
#else
    static f32 dtor = M_PI / 180.0f;
    f32 sine;
    f32 cosine;
    f32 ab;
    f32 bc;
    f32 ca;
    f32 t;
#if LIBULTRA_VERSION >= LIBULTRA_VERSION_K
    f32 xs;
    f32 ys;
    f32 zs;
#endif

    guNormalize(&x, &y, &z);

    a *= dtor;

    sine = sinf(a);
    cosine = cosf(a);

    t = 1.0f - cosine;
    ab = x * y * t;
    bc = y * z * t;
    ca = z * x * t;

    guMtxIdentF(m);

#if LIBULTRA_VERSION >= LIBULTRA_VERSION_K
    xs = x * sine;
    ys = y * sine;
    zs = z * sine;
#else
#define xs (x * sine)
#define ys (y * sine)
#define zs (z * sine)
#endif

    t = x * x;
    m[0][0] = t + cosine * (1.0f - t);
    m[2][1] = bc - xs;
    m[1][2] = bc + xs;
    t = y * y;
    m[1][1] = t + cosine * (1.0f - t);
    m[2][0] = ca + ys;
    m[0][2] = ca - ys;
    t = z * z;
    m[2][2] = t + cosine * (1.0f - t);
    m[1][0] = ab - zs;
    m[0][1] = ab + zs;
#endif
}

void guRotate(Mtx* m, f32 a, f32 x, f32 y, f32 z) {
#if __DREAMCAST__
    guRotateF((void*)m, a, x, y, z);
#else
    f32 mf[4][4];

    guRotateF(mf, a, x, y, z);

    guMtxF2L(mf, m);
#endif
}
