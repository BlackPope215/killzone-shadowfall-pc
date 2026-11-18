// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 (from SpartanPort a29405f): Dear ImGui's fragment shader for the HDR10 swapchain (HDR output,
// Gow3Overlay::Init). The menu's colours are SDR values made for a display with the 2.2 power: they
// are decoded the same way, shown with white at menu_white_nits (the paper white of the game's
// picture, post_process.frag) and encoded to Rec.2020 PQ. ImGui then blends them over the picture.
// The inputs and bindings are ImGui's own (imgui_impl_vulkan.cpp, glsl_shader.frag).
#version 450 core

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform texture2D _Texture;
layout(set = 1, binding = 0) uniform sampler _Sampler;
layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;

// ImGui's backend makes its pipeline without specialization info, so this default is the value
// used: Gow3Overlay::Init writes the paper white into it (Vulkan::SetSpecConstantDefault).
layout(constant_id = 0) const float menu_white_nits = 200.0;

void main() {
    const vec4 c = In.Color * texture(sampler2D(_Texture, _Sampler), In.UV.st);
    const vec3 lin = pow(clamp(c.rgb, 0.0, 1.0), vec3(2.2));
    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164,
                             0.3293, 0.9195, 0.0880,
                             0.0433, 0.0114, 0.8956);
    const vec3 y = clamp(to2020 * lin * (menu_white_nits / 10000.0), 0.0, 1.0);
    const vec3 p = pow(y, vec3(0.1593017578125));
    fColor = vec4(pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), vec3(78.84375)), c.a);
}
