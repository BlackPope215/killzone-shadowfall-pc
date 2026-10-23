/* SPDX-License-Identifier: GPL-2.0-or-later
 * Camera pose math adapted from pearlxcore/KillzoneShadowFall-PC (src/kzsf_free_camera.h,
 * kzsf_camera_call_bridge.h). */
#ifndef GOW3_FREE_CAMERA_HOOK_H
#define GOW3_FREE_CAMERA_HOOK_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "signature.h"

/* Killzone Shadow Fall CUSA00190 v01.81, the player view's camera update (0xd92a00): it stores the
 * view's final transform at [rbx+0x10] (4x4: right, forward, up, position; world up is +Z) and
 * then refreshes the view's derived matrices with "mov rax,[rbx]; mov rdi,rbx; call [rax+0x90]"
 * at 0xd92e8e. The 12 bytes become a jump to a stub that calls visit(view, refresh): the free
 * camera writes its own transform there first, then visit calls refresh(view) as the game did. */
enum { GOW3_FREE_CAMERA_LENGTH = 12, GOW3_FREE_CAMERA_STUB = 32 };
static const Gow3Signature gow3_free_camera_site = {
    "c5 f8 29 43 40 48 8b 03 48 89 df ff 90 90 00 00 00 48 8d 05 ?? ?? ?? ?? c5 fa 10 43 70", 5};
static const unsigned char gow3_free_camera_original[GOW3_FREE_CAMERA_LENGTH] = {
    0x48, 0x8b, 0x03, 0x48, 0x89, 0xdf, 0xff, 0x90, 0x90, 0x00, 0x00, 0x00};

/* The stub at `out` (address out_address); 0 when the way back is out of rel32 range. The call
 * keeps the original call's stack alignment; rax, rsi and rdi are free (the call clobbers them). */
static inline unsigned gow3_free_camera_build_stub(unsigned char *out, uintptr_t out_address, uintptr_t visit,
                                                   uintptr_t resume) {
    static const unsigned char code[GOW3_FREE_CAMERA_STUB] = {
        0x48, 0x8b, 0x03,                         /* mov rax,[rbx] */
        0x48, 0x8b, 0xb0, 0x90, 0x00, 0x00, 0x00, /* mov rsi,[rax+0x90]: the refresh */
        0x48, 0x89, 0xdf,                         /* mov rdi,rbx: the view */
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,       /* movabs rax,visit */
        0xff, 0xd0,                               /* call rax */
        0xe9, 0, 0, 0, 0,                         /* jmp resume */
        0xcc, 0xcc};
    const int64_t back = (int64_t)resume - (int64_t)(out_address + 30);
    if (back < INT32_MIN || back > INT32_MAX) return 0;
    const int32_t rel = (int32_t)back;
    memcpy(out, code, sizeof(code));
    memcpy(out + 15, &visit, 8);
    memcpy(out + 26, &rel, 4);
    return sizeof(code);
}

typedef struct { float matrix[16]; int active; float heading, pitch; } Gow3FreeCamera;
/* Per second: right, up, forward (-1..1), yaw/pitch rates (rad/s); plus a turn (rad) and speed. */
typedef struct { float right, up, forward, yaw_rate, pitch_rate, yaw, pitch, speed; } Gow3FreeCameraInput;

static inline int gow3_free_camera_valid(const float *m) {
    for (int i = 0; i < 16; ++i)
        if (!isfinite(m[i]) || fabsf(m[i]) > 10000000.f) return 0;
    if (fabsf(m[3]) > .001f || fabsf(m[7]) > .001f || fabsf(m[11]) > .001f || fabsf(m[15] - 1.f) > .001f) return 0;
    for (int a = 0; a < 3; ++a)
        for (int b = a; b < 3; ++b) {
            float d = 0;
            for (int i = 0; i < 3; ++i) d += m[a * 4 + i] * m[b * 4 + i];
            if (fabsf(d - (a == b ? 1.f : 0.f)) > .025f) return 0;
        }
    return 1;
}
static inline void gow3_free_camera_basis(Gow3FreeCamera *s) {
    const float ch = cosf(s->heading), sh = sinf(s->heading), cp = cosf(s->pitch), sp = sinf(s->pitch);
    float *m = s->matrix;
    m[0] = sh;       m[1] = -ch;      m[2] = 0;
    m[4] = ch * cp;  m[5] = sh * cp;  m[6] = sp;
    m[8] = -ch * sp; m[9] = -sh * sp; m[10] = cp;
}
/* Starts at the game's view (`native`); 0 when it is not a rigid transform. */
static inline int gow3_free_camera_start(Gow3FreeCamera *s, const float *native) {
    if (!gow3_free_camera_valid(native)) return 0;
    memcpy(s->matrix, native, sizeof(s->matrix));
    const float fx = native[4], fy = native[5], fz = fmaxf(-1.f, fminf(1.f, native[6]));
    s->heading = fx * fx + fy * fy > 1e-8f ? atan2f(fy, fx) : atan2f(native[0], -native[1]);
    const float limit = 1.55334303427f; /* 89 degrees: no flip at the poles */
    s->pitch = fmaxf(-limit, fminf(limit, asinf(fz)));
    gow3_free_camera_basis(s);
    s->active = 1;
    return 1;
}
/* Turns about the eye and moves along the view (up along world Z); 0 if the pose went bad. */
static inline int gow3_free_camera_step(Gow3FreeCamera *s, const Gow3FreeCameraInput *in, float seconds) {
    if (!s->active || !isfinite(seconds) || seconds < 0 || seconds > .25f) return 0;
    const float values[] = {in->right, in->up, in->forward, in->yaw_rate, in->pitch_rate, in->yaw, in->pitch, in->speed};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (!isfinite(values[i])) return 0;
    const float limit = 1.55334303427f;
    s->heading = remainderf(s->heading - in->yaw - in->yaw_rate * seconds, 6.28318530718f);
    s->pitch = fmaxf(-limit, fminf(limit, s->pitch + in->pitch + in->pitch_rate * seconds));
    gow3_free_camera_basis(s);
    float *m = s->matrix, delta[3], length = 0;
    for (int i = 0; i < 3; ++i) {
        delta[i] = m[i] * in->right + (i == 2 ? in->up : 0) + m[4 + i] * in->forward;
        length += delta[i] * delta[i];
    }
    const float scale = length > 1 ? 1.f / sqrtf(length) : 1.f;
    for (int i = 0; i < 3; ++i) m[12 + i] += delta[i] * scale * in->speed * seconds;
    return gow3_free_camera_valid(m);
}
#endif
