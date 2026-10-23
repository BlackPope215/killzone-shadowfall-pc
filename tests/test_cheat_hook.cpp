// Killzone cheats (src/cheat_hook.h) and the free camera (src/free_camera_hook.h) on synthetic state.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/cheat_hook.h"
#include "../src/free_camera_hook.h"

// The pattern's bytes with every "??" as `fill`.
static std::vector<unsigned char> Bytes(const char* pattern, unsigned char fill) {
    int parsed[GOW3_SIGNATURE_MAX];
    const unsigned n = gow3_signature_parse(pattern, parsed);
    assert(n);
    std::vector<unsigned char> out(n);
    for (unsigned i = 0; i < n; ++i) out[i] = parsed[i] < 0 ? fill : (unsigned char)parsed[i];
    return out;
}

static int Read(uintptr_t address, void* out, size_t size) {
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return 1;
}
static int Write(uintptr_t address, const void* in, size_t size) {
    std::memcpy(reinterpret_cast<void*>(address), in, size);
    return 1;
}

int main() {
    // Each DebugSettings reader is found once and leads to the same pointer slot.
    for (const auto& cheat : gow3_debug_cheats) {
        std::vector<unsigned char> image(0x1000, 0x90);
        auto code = Bytes(cheat.site.pattern, 0);
        const uint64_t at = 0x400;
        std::memcpy(image.data() + at, code.data(), code.size());
        const uint64_t lea = at + cheat.site.at;
        assert(image[lea] == 0x48 && image[lea + 1] == 0x8d && image[lea + 2] == 0x05);
        const int32_t disp = 0x200 - (int32_t)(lea + 7);
        std::memcpy(image.data() + lea + 3, &disp, 4);
        assert(gow3_signature_find(image.data(), image.size(), &cheat.site) == lea);
        assert(gow3_debug_settings_slot(image.data(), lea) == 0x200 + 0x7b0);
        assert(cheat.size == 1 || cheat.size == 4);
    }
    assert(GOW3_DEBUG_CHEAT_COUNT == 5 && gow3_debug_cheat_ids[0] == GOW3_CHEAT_GOD &&
           gow3_debug_cheat_ids[4] == GOW3_CHEAT_CHAPTERS);

    // Tactical Echo: only the local player's active scan is kept going.
    static unsigned char base[0xc00000];
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    std::vector<unsigned char> vtable(0x200), entity(0x1000), ability(0x300);
    const uintptr_t is_player = b + 0xa7ab20;
    std::memcpy(vtable.data() + 0x140, &is_player, 8);
    const uintptr_t vt = reinterpret_cast<uintptr_t>(vtable.data());
    std::memcpy(entity.data(), &vt, 8);
    entity[0x282] = 1;
    const uintptr_t owner = reinterpret_cast<uintptr_t>(entity.data());
    std::memcpy(ability.data() + 0x1e0, &owner, 8);
    int state = 2, pulses = 0;
    std::memcpy(ability.data() + 0x2b8, &state, 4);
    std::memcpy(ability.data() + 0x2c0, &pulses, 4);
    const uintptr_t a = reinterpret_cast<uintptr_t>(ability.data());
    assert(gow3_echo_continue(a, b, Read, Write));
    std::memcpy(&pulses, ability.data() + 0x2c0, 4);
    assert(pulses == 2 && ability[0x2c6] == 1);
    entity[0x282] = 0;  // not the player (no controller)
    ability[0x2c6] = 0;
    assert(!gow3_echo_continue(a, b, Read, Write) && ability[0x2c6] == 0);
    entity[0x282] = 1;
    state = 0;  // idle: left to the game
    std::memcpy(ability.data() + 0x2b8, &state, 4);
    assert(!gow3_echo_continue(a, b, Read, Write));

    // The camera bridge: visit's address and the way back.
    unsigned char stub[GOW3_FREE_CAMERA_STUB];
    const uintptr_t at = 0x10000000, visit = 0x123456789abcdef0ull, resume = at + 0x1000;
    assert(gow3_free_camera_build_stub(stub, at, visit, resume) == GOW3_FREE_CAMERA_STUB);
    uintptr_t target;
    int32_t back;
    std::memcpy(&target, stub + 15, 8);
    std::memcpy(&back, stub + 26, 4);
    assert(target == visit && at + 30 + back == resume && stub[25] == 0xe9);
    assert(!gow3_free_camera_build_stub(stub, at, visit, at + 0x100000000ull));

    // Free camera pose: starts at the game's view, moves along it, refuses a broken transform.
    float native[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};  // looks along +Y
    Gow3FreeCamera camera{};
    assert(gow3_free_camera_start(&camera, native));
    Gow3FreeCameraInput in{};
    in.forward = 1;
    in.speed = 4;
    assert(gow3_free_camera_step(&camera, &in, 0.1f));
    assert(std::fabs(camera.matrix[13] - 6.4f) < 1e-4f && std::fabs(camera.matrix[12] - 5) < 1e-4f);
    in = {};
    in.up = 1;
    in.speed = 10;
    assert(gow3_free_camera_step(&camera, &in, 0.1f) && std::fabs(camera.matrix[14] - 8) < 1e-4f);
    in = {};
    in.pitch = 10;  // clamped short of the pole
    assert(gow3_free_camera_step(&camera, &in, 0) && camera.pitch < 1.56f);
    assert(!gow3_free_camera_step(&camera, &in, 1.0f));  // a stall is not a jump
    native[0] = 2;
    Gow3FreeCamera broken{};
    assert(!gow3_free_camera_start(&broken, native) && !broken.active);
    std::puts("cheat-hook-test: OK (debug settings sites, Tactical Echo, camera bridge and pose)");
}
