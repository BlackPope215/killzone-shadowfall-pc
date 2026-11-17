# AMD (RDNA 3) fixes for the Killzone Shadow Fall port

Tested on an AMD Radeon RX 7900 XT (Windows, AMD driver), output 2560x1440 by the port (game at
1080p), 120-144 FPS limit. Every change is its own commit with the reasoning in its message; most
have an environment switch that restores the old behaviour.

## Rendering

- **Purple stair-stepped lines along spotlight cones** (e.g. the lounge with ceiling lights):
  the decal-box pass (ps `a80c1b1b`, vs `3b129b04`) samples the target it draws into. On AMD the
  feedback loop returned stale or undefined texels. Added to the feedback-loop snapshot list
  (`vk_feedback_copy.h`). Found with the new per-pass / per-draw image statistics
  (`GOW3_DIAG_IMAGES=2/3`).
- Read-only depth attachments no longer mark the scene proxy as written, so the native depth image
  stays current (`vk_scene_resolution`; `GOW3_DEPTH_READONLY_PROXY=0` restores).
- Shader recompiler: negated float compares (`V_CMP_N*`) are unordered (true on NaN, as on GCN);
  `S_WQM_B64` grows masks to whole quads (pixel and compute shaders); Wave64 absent-lane reads
  folded on 64-lane hosts.

- **Bullet-impact decals (stream-out), on all GPUs:** the game's one stream-out pipeline
  (copy shader `80558465`) clips the geometry around a hit on the GPU, and the game's CPU reads
  the triangles back to build the decal (`image+0x3aeda0`, found with a guard page on the buffer,
  `GOW3_STREAMOUT_DIAG=1`). It sets every buffer offset itself and never uses the filled size.
  The port skipped these draws (no impact decals at all), and with the capture on
  (`GOW3_STREAMOUT=1`) the CPU still read old memory, because the default Relaxed readback mode
  never returns GPU-written buffers: thin slivers, magenta on the ground, white on crates. The
  precise readback mode for the whole memory (`GOW3_READBACKS=2`) stops the game during loading.
  Each capture is now copied back to guest memory once the GPU is done, through an 8 MiB ring of
  its own and only from the offset the game set (`vk_stream_out.cpp`). Stream-out is on by default
  (`GOW3_STREAMOUT=0` off, `GOW3_STREAMOUT_WRITEBACK=0` without the copy back).

## Shader-compile stutter

The port drops a pending draw (its pipeline still compiling) only for "live", full-resolution
targets. Four problems kept it waiting instead:

1. **The full-resolution width came from the UI**, composed at the output size (2560 for a 1920
   scene): every scene target looked like 75 %, so nothing was ever dropped and each new shader
   stalled ~470 ms. The scene width now comes from passes with a depth buffer only, which makes it
   independent of the display (FHD, QHD, 4K, ultrawide alike); before any scene pass nothing is
   dropped.
2. The width was the maximum of the last 10 s: a wider pass during a loading screen blocked drops
   just when the next area's shaders compiled. Now a 32-64 flip window.
3. **After a level load** the scene targets had to be redrawn 12 frames in a row again: the first
   frames waited for every new shader (176 waits, 4.6 s, a 3.4 s freeze). Targets that were live
   for 300+ frames before the gap count as live at once (`GOW3_PENDING_RELIVE=0` restores).
4. **A waited-for compile job sat in the queue** behind ~300 others (1.2 s, 452 ms of it queued).
   `AsyncJob::Wait` now runs a job no worker has started on the waiting thread
   (`GOW3_WAIT_RUNS_JOB=0` restores).

Result: waits over 100 ms went from ~4 per area (470 ms each) to 0; a level start went from
4.6 s of waits to ~0.2 s; worst waits in new areas ~6-30 ms.

## FSR 4: AMD's FSR SDK 2.4 (FSR Upscaling 4.1.2)

FSR 4 now runs through AMD's official FSR SDK 2.4 for Vulkan when its signed DLLs
(`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`, SDK tag v2.4.0,
`Kits/FidelityFX/signedbin`) are in the game's `bin` folder. The SDK picks the newest upscaler
the GPU and driver support: on the RX 7900 XT it chose **FSR Upscaling 4.1.2** (INT8 model).
Same inputs as before; ~134 FPS average vs ~117-121 with the v07 model in comparable scenes. Its
output shows the same lines on the last rows and columns as v07's, so the edge fix runs for both. Without the DLLs, or when the SDK refuses the device, the v07 model is
used as before (`GOW3_FSR4_SDK=0` forces it). Headers (MIT) in `gpu/third_party/ffx-sdk`.

## Stability

- **GPU hangs (device lost) fixed:** the recompiler emits every stage for 64 lanes, but after
  the Shadow of the Colossus subgroup change only the stages the driver lists in
  `requiredSubgroupSizeStages` were pinned to 64. AMD's Windows driver lists `0xf0` (no vertex),
  so vertex shaders ran in 32-lane waves with code built for 64: a wave loop over the exec mask
  never ended, the GPU hung for ~2.5 s and was reset (device lost every 2-7 minutes, any
  upscaler, empty fault report). On the 64-lane path every stage is pinned again, as in the
  original port (AMD honours it for vertex shaders); `GOW3_PIN_ALL_STAGES=0` pins only the
  listed stages. Confirmed: no hang in a long session that hung within minutes before.
- Compute is still never sent down a 32-lane path while 64 can be pinned for fragment and
  compute (`GOW3_SUBGROUP_STAGES=0` restores the original checks).
- A graphics or compute pipeline the driver refuses skips its draws / dispatches instead of
  ending the game.
- Device lost: the driver's fault report (`VK_EXT_device_fault`) is now logged also when the
  swapchain acquire or present sees the loss first (before, only a failed queue submit read it).
- A minimized or hidden window draws at most 30 FPS (`GOW3_BACKGROUND_FPS`, 0 = off) instead of
  rendering at the full limit for nothing.

## From other PS4 ports

SpartanPort (texture cache, compute queues, D32 copies, FSR/RCAS present fixes, mutex flags,
memory), the Uncharted port (64-bit motion-vector addresses added in 32-bit halves with a carry,
SubmitGfx ordering, SRW locks, FindImage copying the merged `ImageInfo`), the Shadow of the
Colossus port (subgroup sizes, absent lanes, refused pipelines) and The Order 1886 port
(`SlotVector` whose entries never move). Each commit names its source.

## Diagnostics added (off by default)

- `GOW3_STREAMOUT_DIAG=1` with the capture on: the capture ranges, the draws that read them, and
  every 10 s the CPU code that read them (guard page: instruction, code bytes, registers) and
  their contents.
- `GOW3_DIAG_IMAGES=2`: per pass of a captured frame, NaN / infinite / negative / magenta pixel
  counts of each target. `=3` with `GOW3_DIAG_ADDR`: one target after every draw.
- Long compile waits log the scene width and whether skipping is off; the 5 s "Shaders:" line
  counts compile jobs run by the waiting thread.
