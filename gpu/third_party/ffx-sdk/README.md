# AMD FSR SDK 2.4 headers (Vulkan upscaling)

Headers from the AMD FidelityFX / FSR SDK v2.4.0 (MIT, Copyright Advanced Micro Devices, Inc.),
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag v2.4.0:

- `api/include/ffx_api.h`, `api/include/ffx_api_types.h` (Kits/FidelityFX/api/include)
- `api/include/vk/ffx_api_vk.h` (Kits/FidelityFX/api/include/vk)
- `upscalers/include/ffx_upscale.h` (Kits/FidelityFX/upscalers/include)

Used by `gpu/shadps4/video_core/renderer_vulkan/vk_fsr41_sdk.cpp`, which loads AMD's signed
DLLs at run time from the game's `bin` folder (nothing is linked at build time):

- `amd_fidelityfx_loader_vk.dll`
- `amd_fidelityfx_upscaler_vk.dll`

(Kits/FidelityFX/signedbin of the same tag). The SDK picks the newest upscaler the GPU and driver
support: FSR Upscaling 4.1.2 (INT8 model on Radeon RX 7000, FP8 on RX 9000) with AMD Software
26.10.1 or later, else FSR 3.1.6.
