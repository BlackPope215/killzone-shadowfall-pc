/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GOW3_SCENE_RES_HOOK_H
#define GOW3_SCENE_RES_HOOK_H
#include <stdint.h>
#include <string.h>
#include "signature.h"

/* Killzone Shadow Fall CUSA00190 v01.81: the 3D scene at a smaller size than the display, for the
 * upscaler's quality modes (GOW3_RENDER_RES). The display size (device 0x3a3fa68 +0x3be8/+0x3bec,
 * set from the init rectangle at 0x8e6256) stays 1920x1080: the UI, menus, movies and loading
 * screen are drawn with it, which is what the old resolution patch shrank along with the scene.
 *
 * The scene renderer (0x3b490e8, ctor 0x509e40) creates every scene target from the size given to
 * 0x50a670, and each caller copies the display size there with "mov r32,[reg+0x3be8/0x3bec]":
 * boot (0x8e6470), the two target re-creations (0x50f930, 0x50fc43) and level load (0xdc5120).
 * Each load becomes "mov r32,[rip+cell]": the scene size cell below, which the in-game menu
 * changes live (quality modes switch without a restart).
 *
 * Gow3SceneSize lives in a hook page near the image: the requested size (the menu writes both
 * halves at once, one 64-bit store) and the applied one (the size the targets were made with). */
typedef struct { uint32_t width, height, applied_width, applied_height; } Gow3SceneSize;
enum { GOW3_SCENE_SIZE_SITES = 4 };
typedef struct { const char *name; Gow3Signature site; int width_first; } Gow3SceneSizeSite;
static const Gow3SceneSizeSite gow3_scene_size_sites[GOW3_SCENE_SIZE_SITES] = {
    /* mov eax,[rbx+0x3be8]; mov ecx,[rbx+0x3bec] */
    {"boot", {"8b 83 e8 3b 00 00 8b 8b ec 3b 00 00 c7 85 48 ff ff ff ff ff ff ff", 0}, 0},
    /* mov ecx,[rax+0x3be8]; mov eax,[rax+0x3bec] */
    {"re-creation", {"8b 88 e8 3b 00 00 8b 80 ec 3b 00 00 c7 45 a8 ff ff ff ff", 0}, 1},
    {"re-creation", {"8b 88 e8 3b 00 00 8b 80 ec 3b 00 00 c7 45 c0 ff ff ff ff", 0}, 1},
    {"level load", {"8b 88 e8 3b 00 00 8b 80 ec 3b 00 00 48 8d b5 68 ff ff ff", 0}, 1},
};

/* The two loads replaced by loads of the applied size: eax/ecx get the width/height the site's own
 * order gives them. `out` receives the 12 bytes, `site` is where they run (the rip they are
 * relative to). Returns 0 when the cell is out of rel32 range. */
static inline int gow3_scene_size_write(unsigned char *out, uintptr_t site, int width_first, const Gow3SceneSize *cell) {
    unsigned char *at = out;
    const uintptr_t width = (uintptr_t)&cell->applied_width, height = (uintptr_t)&cell->applied_height;
    const int64_t to_width = (int64_t)width - (int64_t)(site + 6);
    const int64_t to_height = (int64_t)height - (int64_t)(site + 12);
    if (to_width < INT32_MIN || to_width > INT32_MAX || to_height < INT32_MIN || to_height > INT32_MAX) return 0;
    const int32_t w = (int32_t)to_width, h = (int32_t)to_height;
    at[0] = 0x8b; at[1] = width_first ? 0x0d : 0x05; memcpy(at + 2, &w, 4); /* mov ecx|eax,[rip+width] */
    at[6] = 0x8b; at[7] = width_first ? 0x05 : 0x0d; memcpy(at + 8, &h, 4); /* mov eax|ecx,[rip+height] */
    return 1;
}

/* Image offsets 0x50f90c (per-frame render 0x50f8b0) and 0x50fc1f (0x50fbf0): the targets are
 * made again when the half-width request (+0x11) differs from the current mode (+0x10):
 *   mov al,[rbx+0x11]; cmp al,[rbx+0x10]; jnz <re-create, +0x17>
 * The stub also sends the frame there when the requested scene size differs from the applied
 * one, after taking the requested size as applied; the size loads above then read it. rcx is
 * written again on both paths before it is read. */
enum { GOW3_SCENE_RECREATE_HOOK_LENGTH = 6, GOW3_SCENE_RECREATE_OFFSET = 0x17 };
static const Gow3Signature gow3_scene_recreate_sites[2] = {
    {"8a 43 11 3a 43 10 75 0f 48 8b 4b 58 8b 93 94 04 00 00 3b 51 18 74 50 88 43 10 48 8d 05 ?? ?? ?? ?? 48 8b 00 8b 88 e8 3b 00 00 8b 80 ec 3b 00 00 c7 45 a8", 0},
    {"8a 43 11 3a 43 10 75 0f 48 8b 4b 58 8b 93 94 04 00 00 3b 51 18 74 50 88 43 10 48 8d 05 ?? ?? ?? ?? 48 8b 00 8b 88 e8 3b 00 00 8b 80 ec 3b 00 00 c7 45 c0", 0},
};

static inline unsigned gow3_scene_recreate_build_stub(unsigned char *out, uintptr_t out_address, uintptr_t site,
                                                      const Gow3SceneSize *cell) {
    unsigned n = 0;
    const uint64_t address = (uint64_t)(uintptr_t)cell;
    out[n++] = 0x48; out[n++] = 0xb9; memcpy(out + n, &address, 8); n += 8; /* mov rcx,cell */
    const unsigned char check[] = {
        0x48, 0x8b, 0x01,       /* mov rax,[rcx]      requested */
        0x48, 0x3b, 0x41, 0x08, /* cmp rax,[rcx+8]    applied */
        0x75, 0x0b,             /* jne recreate */
        0x8a, 0x43, 0x11,       /* mov al,[rbx+0x11] */
        0x3a, 0x43, 0x10};      /* cmp al,[rbx+0x10] */
    memcpy(out + n, check, sizeof check); n += sizeof check;
    const int64_t to_resume = (int64_t)(site + GOW3_SCENE_RECREATE_HOOK_LENGTH) - (int64_t)(out_address + n + 5);
    out[n++] = 0xe9; const unsigned resume = n; n += 4; /* jmp site+6 (the jnz, flags of the cmp) */
    const unsigned char recreate[] = {
        0x48, 0x89, 0x41, 0x08, /* mov [rcx+8],rax    applied = requested */
        0x8a, 0x43, 0x11};      /* mov al,[rbx+0x11]  (the block stores it in +0x10) */
    memcpy(out + n, recreate, sizeof recreate); n += sizeof recreate;
    const int64_t to_block = (int64_t)(site + GOW3_SCENE_RECREATE_OFFSET) - (int64_t)(out_address + n + 5);
    if (to_resume < INT32_MIN || to_resume > INT32_MAX || to_block < INT32_MIN || to_block > INT32_MAX) return 0;
    const int32_t r = (int32_t)to_resume, b = (int32_t)to_block;
    memcpy(out + resume, &r, 4);
    out[n++] = 0xe9; memcpy(out + n, &b, 4); n += 4; /* jmp site+0x17 */
    return n;
}

/* Image offset 0x50fe57 in the scene's per-frame view copy (0x50fcb0): each view (0x730 bytes,
 * render rectangle x,y,w,h at +0x1f0) has just been copied twice, to r14+r13 and r12+r13. The
 * game's own half-width mode (multiplayer at 60 Hz) halves w right after, in those copies only:
 * the original rectangle is the UI's reference (0xd94eef). The stub does the same for a full-display
 * rectangle {0,0,1920,1080}, which becomes {0,0,w,h} of the applied size, then replays the 10
 * bytes it replaced. */
enum { GOW3_SCENE_RECT_HOOK_LENGTH = 10 };
static const Gow3Signature gow3_scene_rect_site = {"48 8b 75 c0 80 7e 10 00 74 70 4b 8b 8c 2f f0 01 00 00", 0};

/* Returns the stub's length, or 0 when a branch back is out of rel32 range. rax and the flags are
 * free here (both paths overwrite them before reading); rcx is saved around the stub. */
static inline unsigned gow3_scene_rect_build_stub(unsigned char *out, uintptr_t out_address, uintptr_t site,
                                                  const Gow3SceneSize *cell) {
    unsigned n = 0;
    const uint64_t applied = (uint64_t)(uintptr_t)&cell->applied_width;
    out[n++] = 0x51;                                                        /* push rcx */
    out[n++] = 0x48; out[n++] = 0xb9; memcpy(out + n, &applied, 8); n += 8; /* mov rcx,&applied */
    out[n++] = 0x48; out[n++] = 0x8b; out[n++] = 0x09;                      /* mov rcx,[rcx]  w | h<<32 */
    for (int copy = 0; copy < 2; ++copy) {
        /* lea rax,[r14|r12 + r13 + 0x1f0] */
        const unsigned char lea[8] = {0x4b, 0x8d, 0x84, copy ? 0x2c : 0x2e, 0xf0, 0x01, 0x00, 0x00};
        memcpy(out + n, lea, 8); n += 8;
        const unsigned char checks[] = {
            0x83, 0x38, 0x00,                         /* cmp dword [rax],0 */
            0x75, 0x1c,                               /* jne skip */
            0x83, 0x78, 0x04, 0x00,                   /* cmp dword [rax+4],0 */
            0x75, 0x16,                               /* jne skip */
            0x81, 0x78, 0x08, 0x80, 0x07, 0x00, 0x00, /* cmp dword [rax+8],1920 */
            0x75, 0x0d,                               /* jne skip */
            0x81, 0x78, 0x0c, 0x38, 0x04, 0x00, 0x00, /* cmp dword [rax+0xc],1080 */
            0x75, 0x04,                               /* jne skip (the 4-byte write) */
            0x48, 0x89, 0x48, 0x08};                  /* mov [rax+8],rcx  w,h */
        memcpy(out + n, checks, sizeof checks); n += sizeof checks;
        /* skip: */
    }
    out[n++] = 0x59; /* pop rcx */
    const unsigned char replay[8] = {0x48, 0x8b, 0x75, 0xc0, 0x80, 0x7e, 0x10, 0x00}; /* mov rsi,[rbp-0x40]; cmp byte [rsi+0x10],0 */
    memcpy(out + n, replay, 8); n += 8;
    const int64_t to_half = (int64_t)(site + 10 + 0x70) - (int64_t)(out_address + n + 6); /* the original jz target */
    const int64_t to_resume = (int64_t)(site + GOW3_SCENE_RECT_HOOK_LENGTH) - (int64_t)(out_address + n + 6 + 5);
    if (to_half < INT32_MIN || to_half > INT32_MAX || to_resume < INT32_MIN || to_resume > INT32_MAX) return 0;
    const int32_t z = (int32_t)to_half, r = (int32_t)to_resume;
    out[n++] = 0x0f; out[n++] = 0x84; memcpy(out + n, &z, 4); n += 4; /* jz 0x50fed1 */
    out[n++] = 0xe9; memcpy(out + n, &r, 4); n += 4;                  /* jmp 0x50fe61 */
    return n;
}
#endif
