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
    float hdr_peak;        // hdr 2: nits the highlights are fitted under
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

// SMPTE ST 2084 (PQ): nits to the PQ value in [0, 1], and back.
const float pq_m1 = 0.1593017578125, pq_m2 = 78.84375, pq_c1 = 0.8359375, pq_c2 = 18.8515625, pq_c3 = 18.6875;
vec3 pq_encode(vec3 nits) {
    const vec3 p = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(pq_m1));
    return pow((pq_c1 + pq_c2 * p) / (1.0 + pq_c3 * p), vec3(pq_m2));
}
float pq_encode(float nits) {
    return pq_encode(vec3(nits)).x;
}
float pq_decode(float e) {
    const float p = pow(clamp(e, 0.0, 1.0), 1.0 / pq_m2);
    return 10000.0 * pow(max(p - pq_c1, 0.0) / (pq_c2 - pq_c3 * p), 1.0 / pq_m1);
}

// gow3 (from SpartanPort 0d21602 and ab6798a, after AjncSystem's Shadow of the Colossus port): the
// SDR picture to HDR10 (Rec.2020 primaries, PQ). `lin` is the picture in linear light, white at 1,
// shown at hdr_paper_white nits; highlights get up to 1.5 times that, smoothly from 60% of white,
// one factor per pixel so the hue stays. They are then fitted under hdr_peak with the EETF of
// ITU-R BT.2390 (BT.2408 Annex 5) on the brightest channel in PQ, again as one factor: the source
// maximum is paper white x 1.5, black stays 0, the curve is the identity up to the knee
// KS = 1.5 maxLum - 0.5 and a Hermite spline above it that ends at the peak. When the source
// maximum fits under the peak the picture is untouched. Last, BT.2087 takes Rec.709 to Rec.2020.
const float highlight_gain = 1.5;
vec3 sdr_to_hdr10(vec3 lin) {
    lin = max(lin, 0.0);
    const float peak = max(lin.r, max(lin.g, lin.b));
    const float t = smoothstep(0.6, 1.0, peak);
    lin *= 1.0 + (highlight_gain - 1.0) * t * t;
    vec3 nits = lin * pp.hdr_paper_white;
    const float source_pq = pq_encode(pp.hdr_paper_white * highlight_gain);
    const float max_lum = pq_encode(pp.hdr_peak) / source_pq;
    const float ks = 1.5 * max_lum - 0.5;
    const float m = max(nits.r, max(nits.g, nits.b));
    const float e1 = min(pq_encode(m) / source_pq, 1.0);
    if (max_lum < 1.0 && e1 > ks && m > 0.0) {
        const float s = (e1 - ks) / (1.0 - ks), s2 = s * s, s3 = s2 * s;
        const float e2 = (2.0 * s3 - 3.0 * s2 + 1.0) * ks + (s3 - 2.0 * s2 + s) * (1.0 - ks) +
                         (-2.0 * s3 + 3.0 * s2) * max_lum;
        nits *= pq_decode(e2 * source_pq) / m;
    }
    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164,
                             0.3293, 0.9195, 0.0880,
                             0.0433, 0.0114, 0.8956);
    return pq_encode(to2020 * nits);
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
