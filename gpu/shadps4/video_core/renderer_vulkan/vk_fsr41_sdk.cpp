// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_fsr41_sdk.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include "../../../third_party/ffx-sdk/api/include/ffx_api.h"
#include "../../../third_party/ffx-sdk/api/include/ffx_api_types.h"
#include "../../../third_party/ffx-sdk/api/include/vk/ffx_api_vk.h"
#include "../../../third_party/ffx-sdk/upscalers/include/ffx_upscale.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan {

namespace {

constexpr wchar_t LoaderName[] = L"amd_fidelityfx_loader_vk.dll";
constexpr wchar_t UpscalerName[] = L"amd_fidelityfx_upscaler_vk.dll";

#ifdef _WIN32
std::filesystem::path DllDirectory() {
    if (const wchar_t* env = _wgetenv(L"GOW3_FFX_DIR"); env && env[0]) {
        return env;
    }
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}
#endif

void Message(uint32_t type, const wchar_t* message) {
    std::printf("Upscaler: AMD FSR SDK %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning",
                message ? message : L"");
    std::fflush(stdout);
}

} // namespace

struct Fsr41Sdk::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    std::string problem;
    bool fatal = false;
#ifdef _WIN32
    HMODULE module = nullptr;
#endif
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    ffxContext context = nullptr;
    u32 out_width = 0, out_height = 0;
    bool hdr = true;
    std::string version;

    Impl(const Instance& instance_, Scheduler& scheduler_)
        : instance{instance_}, scheduler{scheduler_} {}

    ~Impl() {
        if (context) {
            scheduler.Finish();
            destroy(&context, nullptr);
            context = nullptr;
        }
        // The module stays loaded: the driver may keep threads of its own in it.
    }

    void Fail(std::string reason, bool permanent) {
        if (problem != reason) {
            std::printf("Upscaler: AMD FSR SDK unavailable: %s\n", reason.c_str());
            std::fflush(stdout);
        }
        problem = std::move(reason);
        fatal |= permanent;
    }

    bool Load() {
#ifdef _WIN32
        if (module) {
            return true;
        }
        const auto directory = DllDirectory();
        if (!std::filesystem::exists(directory / LoaderName) ||
            !std::filesystem::exists(directory / UpscalerName)) {
            Fail("amd_fidelityfx_loader_vk.dll / amd_fidelityfx_upscaler_vk.dll not found in " +
                     directory.string(),
                 true);
            return false;
        }
        // The loader finds the upscaler DLL beside itself.
        module = LoadLibraryExW((directory / LoaderName).c_str(), nullptr,
                                LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) {
            Fail("the loader DLL did not load (error " + std::to_string(GetLastError()) + ")", true);
            return false;
        }
        create = reinterpret_cast<PfnFfxCreateContext>(
            reinterpret_cast<void*>(GetProcAddress(module, "ffxCreateContext")));
        destroy = reinterpret_cast<PfnFfxDestroyContext>(
            reinterpret_cast<void*>(GetProcAddress(module, "ffxDestroyContext")));
        query = reinterpret_cast<PfnFfxQuery>(
            reinterpret_cast<void*>(GetProcAddress(module, "ffxQuery")));
        dispatch = reinterpret_cast<PfnFfxDispatch>(
            reinterpret_cast<void*>(GetProcAddress(module, "ffxDispatch")));
        if (!create || !destroy || !query || !dispatch) {
            Fail("the loader DLL lacks the ffx API entry points", true);
            return false;
        }
        return true;
#else
        Fail("Windows only", true);
        return false;
#endif
    }

    bool CreateContext(u32 ow, u32 oh, bool hdr_input) {
        if (context) {
            scheduler.WaitSubmitted();
            scheduler.Finish();
            destroy(&context, nullptr);
            context = nullptr;
        }
        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.deviceContext.vkDevice = instance.GetDevice();
        backend.deviceContext.vkPhysicalDevice = instance.GetPhysicalDevice();
        backend.deviceContext.vkDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
        // Native AA runs on the HDR scene color; the scaled presets on the tonemapped frame.
        desc.flags = hdr_input ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                               : 0u;
        // Any preset's render size fits: the context is not recreated for a size change.
        desc.maxRenderSize = {ow, oh};
        desc.maxUpscaleSize = {ow, oh};
        desc.fpMessage = Message;
        const ffxReturnCode_t created = create(&context, &desc.header, nullptr);
        if (created != FFX_API_RETURN_OK) {
            context = nullptr;
            // An old driver has no provider for this device: permanent for the session.
            Fail("context creation failed (" + std::to_string(created) +
                     "; FSR 4.1 needs AMD Software 26.10.1 or later)",
                 true);
            return false;
        }
        ffxQueryGetProviderVersion provider{};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (query(&context, &provider.header) == FFX_API_RETURN_OK && provider.versionName) {
            version = provider.versionName;
        } else {
            version = "unknown version";
        }
        out_width = ow;
        out_height = oh;
        hdr = hdr_input;
        std::printf("Upscaler: AMD FSR SDK 2.4, %s, output %ux%u (%s)\n", version.c_str(), ow, oh,
                    hdr_input ? "HDR scene color" : "tonemapped frame");
        std::fflush(stdout);
        return true;
    }

    static FfxApiResource Resource(const Fsr4Upscaler::Image& image, vk::Format fallback,
                                   uint32_t usage) {
        const vk::Format format = image.format != vk::Format::eUndefined ? image.format : fallback;
        FfxApiResource r{};
        r.resource = reinterpret_cast<void*>(VkImage(image.image));
        r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        r.description.format = ffxApiGetSurfaceFormatVK(VkFormat(format));
        r.description.width = image.width;
        r.description.height = image.height;
        r.description.depth = 1;
        r.description.mipCount = 1;
        r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
        r.description.usage = usage | (image.depth || ffxApiIsDepthFormat(VkFormat(format))
                                           ? FFX_API_RESOURCE_USAGE_DEPTHTARGET
                                           : 0u);
        // Every image is in layout General: the SDK moves it to what it needs and back.
        r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        return r;
    }

    bool Record(const Fsr4Upscaler::Frame& f) {
        if (fatal || !Load()) {
            return false;
        }
        if (!context || f.output.width != out_width || f.output.height != out_height ||
            f.hdr != hdr) {
            if (!CreateContext(f.output.width, f.output.height, f.hdr)) {
                return false;
            }
        }
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = reinterpret_cast<void*>(VkCommandBuffer(f.cmdbuf));
        d.color = Resource(f.color, vk::Format::eR16G16B16A16Sfloat, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.depth = Resource(f.depth, vk::Format::eD32Sfloat, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.motionVectors =
            Resource(f.motion, vk::Format::eR16G16Sfloat, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.output = Resource(f.output, vk::Format::eR16G16B16A16Sfloat, FFX_API_RESOURCE_USAGE_UAV);
        d.jitterOffset = {f.jitter[0], f.jitter[1]};
        // Vectors are in render pixels (previous - current), as for the other upscalers.
        d.motionVectorScale = {1.0f, 1.0f};
        d.renderSize = {f.render_width, f.render_height};
        d.upscaleSize = {f.output.width, f.output.height};
        d.enableSharpening = f.sharpen && f.sharpness > 0.0f;
        d.sharpness = f.sharpness;
        d.frameTimeDelta = f.frame_ms;
        d.preExposure = 1.0f;
        d.reset = f.reset;
        d.cameraNear = f.near_plane;
        d.cameraFar = f.far_plane;
        d.cameraFovAngleVertical = f.vertical_fov;
        d.viewSpaceToMetersFactor = 1.0f;
        if (const ffxReturnCode_t result = dispatch(&context, &d.header);
            result != FFX_API_RETURN_OK) {
            Fail("dispatch failed (" + std::to_string(result) + ")", false);
            return false;
        }
        problem.clear();
        return true;
    }
};

Fsr41Sdk::Fsr41Sdk(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {}

Fsr41Sdk::~Fsr41Sdk() = default;

bool Fsr41Sdk::Record(const Fsr4Upscaler::Frame& frame) {
    return impl->Record(frame);
}

const std::string& Fsr41Sdk::Problem() const noexcept {
    return impl->problem;
}

bool Fsr41Sdk::Fatal() const noexcept {
    return impl->fatal;
}

} // namespace Vulkan
