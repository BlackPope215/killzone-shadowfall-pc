// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    // gow3: 0 SDR; 1 the game's own HDR10, passed through; 2 the SDR picture expanded to HDR10
    // by sdr_to_hdr10() below (HDR output on an SDR game).
    uint hdr;
    bool srgb_input;
    float hdr_paper_white; // hdr 2: nits of the SDR picture's white
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

// gow3 (from SpartanPort 0d21602, after AjncSystem's Shadow of the Colossus port): the SDR picture
// to HDR10 (Rec.2020 primaries, SMPTE ST 2084 PQ). `lin` is the picture in linear light, white at 1,
// shown at hdr_paper_white nits; highlights get up to 1.5 times that, smoothly from 60% of white,
// one factor per pixel so the hue stays. The BT.2087 matrix takes Rec.709 to Rec.2020.
const float highlight_gain = 1.5;
vec3 sdr_to_hdr10(vec3 lin) {
    lin = max(lin, 0.0);
    const float peak = max(lin.r, max(lin.g, lin.b));
    const float t = smoothstep(0.6, 1.0, peak);
    lin *= 1.0 + (highlight_gain - 1.0) * t * t;
    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164,
                             0.3293, 0.9195, 0.0880,
                             0.0433, 0.0114, 0.8956);
    const vec3 y = clamp(to2020 * lin * pp.hdr_paper_white / 10000.0, 0.0, 1.0);
    const vec3 p = pow(y, vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), vec3(78.84375));
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr == 1u) {
        color = color_linear;
    } else if (pp.hdr == 2u) {
        // Encoded as for an SDR display (with the gamma setting), then decoded with the 2.2 power
        // of a PC monitor, so the picture matches the SDR one. Alpha 1 for layered presents.
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        const vec3 encoded = gamma(color_linear.rgb);
        color = vec4(sdr_to_hdr10(pow(clamp(encoded, 0.0, 1.0), vec3(2.2))), 1.0);
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
