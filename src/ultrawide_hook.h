/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GOW3_ULTRAWIDE_HOOK_H
#define GOW3_ULTRAWIDE_HOOK_H
#include <stdint.h>
#include <string.h>
#include "signature.h"

/* Killzone Shadow Fall CUSA00190 v01.81: Hor+ field of view for screens wider than 16:9.
 *
 * Every camera keeps a vertical field of view (camera +0x6c, degrees) and an aspect ratio (+0x70).
 * The game's cameras derive the vertical view from a horizontal one with the aspect of the player
 * view (0xd929d0: ViewportLayout multiplier [0x3c3ffe0]+0x1f78 times 16/9), in ten places
 * (0x7230c0, 0x722040, 0xd92a00, 0xa583d0, 0xa58bb0, ...). Those stay at 16:9, so the vertical
 * view is the 16:9 one. The frustum of a camera is made in one place, Camera::GetFrustum
 * (0x6356c0, vtable slot +0xa0 of both camera classes): its near-plane half width is
 * near*tan(fov/2)*aspect (0x2bd5a0), the projection and the culling of the view both come from
 * it. Its aspect load becomes aspect*k, k = (width/height)/(16/9): the horizontal view widens and
 * nothing at the sides is culled. 0x2bd5a0 itself is left alone: lights, shadows and probes make
 * their frusta with it too.
 *
 * Image offset 0x6356ea: vmovss xmm3,[rbx+0x70] (5 bytes) -> jmp to a stub that loads it,
 * multiplies by k (a float after the stub) and jumps back to 0x6356ef. */
enum { GOW3_ULTRAWIDE_HOOK_LENGTH = 5 };
static const Gow3Signature gow3_ultrawide_frustum_site = {
    "c5 fa 10 5b 70 c5 fa 10 53 6c c5 f8 28 43 10 c5 f8 29 85 70 ff ff ff", 0};

/* Returns the stub's length (k included), or 0 when the jump back is out of rel32 range. */
static inline unsigned gow3_ultrawide_build_stub(unsigned char *out, uintptr_t out_address, uintptr_t site, float k) {
    unsigned n = 0;
    const unsigned char load[] = {0xc5, 0xfa, 0x10, 0x5b, 0x70}; /* vmovss xmm3,[rbx+0x70] */
    memcpy(out + n, load, sizeof load); n += sizeof load;
    out[n++] = 0xc5; out[n++] = 0xe2; out[n++] = 0x59; out[n++] = 0x1d; /* vmulss xmm3,xmm3,[rip+k] */
    const unsigned k_disp = n; n += 4;
    const int64_t to_resume = (int64_t)(site + GOW3_ULTRAWIDE_HOOK_LENGTH) - (int64_t)(out_address + n + 5);
    if (to_resume < INT32_MIN || to_resume > INT32_MAX) return 0;
    const int32_t r = (int32_t)to_resume;
    out[n++] = 0xe9; memcpy(out + n, &r, 4); n += 4; /* jmp 0x6356ef */
    const int32_t k_rel = (int32_t)(n - (k_disp + 4));
    memcpy(out + k_disp, &k_rel, 4);
    memcpy(out + n, &k, 4); n += 4;
    return n;
}
#endif
