"""FSR 4 files: AMD's FSR SDK 2.4 Vulkan DLLs (FSR Upscaling 4.1.2, the game's first choice) and the
v07 INT8 model shaders (its fallback): the same set and commit as tools/fetch_fsr4_assets.sh.

The DLLs are AMD's signed binaries from the FidelityFX SDK tag v2.4.0 (MIT); the game loads them
from this folder (GOW3_FFX_DIR, gpu vk_fsr41_sdk.cpp) or from its own bin folder.

Built by FireBurn/Q2RTX from AMD's MIT-licensed FidelityFX SDK source; they may not ship in the
release zip (packaging/windows/package.sh), so the launcher downloads them into fsr4_shaders next
to the port, where the game reads them (GOW3_FSR4_DIR, gpu vk_fsr4.cpp).
"""
from pathlib import Path
import urllib.request

COMMIT = 'ae8d628fae208813172446d1e49ed94150b04658'
BASE = f'https://raw.githubusercontent.com/FireBurn/Q2RTX/{COMMIT}/baseq2/fsr4_shaders/'
MODELS = ('native', 'quality', 'balanced', 'performance', 'ultraperf', 'drs')
TIERS = ('1080', '2160')
SDK_BASE = ('https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/refs/tags/v2.4.0/'
            'Kits/FidelityFX/signedbin/')
SDK_DLLS = ('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll')


def url(name):
    return (SDK_BASE if name in SDK_DLLS else BASE) + name


def files():
    names = list(SDK_DLLS) + ['LICENSE-FSR4-v07.txt', 'rcas.spv', 'spd_auto_exposure.spv']
    for model in MODELS:
        prefix = f'fsr4_model_v07_i8_{model}'
        names += [f'{prefix}_initializers.bin', f'{prefix}_pre_weights.bin', f'{prefix}_shader_manifest.json']
        for tier in TIERS:
            names += [f'{prefix}_{tier}_pre.spv', f'{prefix}_{tier}_post.spv']
            names += [f'{prefix}_{tier}_pass{p}.spv' for p in range(1, 13)]
    return names


def missing(folder: Path):
    return [name for name in files() if not (folder / name).is_file() or (folder / name).stat().st_size == 0]


def download(folder: Path, progress=None):
    """Fetches the missing files; progress(done, total) after each. Raises OSError on failure."""
    folder.mkdir(parents=True, exist_ok=True)
    todo = missing(folder)
    for done, name in enumerate(todo, 1):
        part = folder / (name + '.part')
        request = urllib.request.Request(url(name), headers={'User-Agent': 'kzsf-launcher'})
        with urllib.request.urlopen(request, timeout=60) as response, open(part, 'wb') as out:
            while chunk := response.read(1 << 16):
                out.write(chunk)
        part.replace(folder / name)
        if progress:
            progress(done, len(todo))
    return len(todo)
