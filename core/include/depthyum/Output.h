#pragma once

#include "depthyum/Math.h"
#include "depthyum/Parallel.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace depthyum {

// Z distance, normalised so that the near plane is 0 and the far plane is 1, from a disparity
// value d (0 = far, 1 = near) that spans 1/far .. 1/near.
inline float zDistanceNormalised(float d, double nearM, double farM) {
    if (farM <= nearM) farM = nearM + 1e-6;
    const double invNear = 1.0 / nearM, invFar = 1.0 / farM;
    const double inv = invFar + clampv(static_cast<double>(d), 0.0, 1.0) * (invNear - invFar);
    const double z = 1.0 / inv;
    return static_cast<float>(clampv((z - nearM) / (farM - nearM), 0.0, 1.0));
}

// Distance-style colour ramp for depth (0 = far, 1 = near): deep blue, cyan, green, yellow, red.
inline void depthColor(float t, float* rgb) {
    static const float stops[5][3] = {{0.05f, 0.10f, 0.45f}, {0.05f, 0.65f, 0.85f}, {0.20f, 0.75f, 0.25f},
                                      {0.95f, 0.85f, 0.15f}, {0.85f, 0.12f, 0.08f}};
    t = clampv(t, 0.0f, 1.0f) * 4.0f;
    const int i = t >= 4.0f ? 3 : static_cast<int>(t);
    const float f = t - i;
    for (int c = 0; c < 3; ++c) rgb[c] = stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f;
}

// ---------------------------------------------------------------------------------------------
// Depth scan: a wave over depth whose phase moves with time, so a band of the picture lights up
// and travels from near to far (or back) and wraps around.
// ---------------------------------------------------------------------------------------------

enum ScanShape { kScanSine = 0, kScanTriangle = 1, kScanSawtooth = 2 };

// d: depth 0..1 (0 far). freq: number of waves across the depth range. phase: in cycles, may be
// negative or large; adding 1 gives the same result, which is what makes the scan loop exactly.
// A growing phase moves the lit band from near to far.
// sharp 0..1 narrows the lit band. Returns 0..1.
inline float scanWave(float d, float freq, double phase, int shape, float sharp) {
    const double x = static_cast<double>(d) * freq + phase;
    const float u = static_cast<float>(x - std::floor(x));
    float w;
    switch (shape) {
    case kScanTriangle: w = 1.0f - std::fabs(2.0f * u - 1.0f); break;
    case kScanSawtooth: w = u; break;
    default: w = 0.5f - 0.5f * std::cos(6.28318530717958647692f * u); break;
    }
    const float k = 1.0f + 15.0f * sharp * sharp;
    return clampv((w - 0.5f) * k + 0.5f, 0.0f, 1.0f);
}

// Focus band: 1 at distance `centre` (depth 0..1, 1 = near), falling to 0 over `soft` beyond half
// the band's `width`. With wrap the distance goes round the depth range, so a band that is
// travelling through depth leaves at one end and re-enters at the other.
inline float bandMask(float d, float centre, float width, float soft, bool wrap) {
    float dist = std::fabs(d - centre);
    if (wrap) dist = std::min(dist, 1.0f - dist);
    const float half = 0.5f * std::max(width, 0.0f);
    if (dist <= half) return 1.0f;
    if (soft <= 1e-6f) return 0.0f;
    const float t = clampv((dist - half) / soft, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// Tone for depth d (0..1) from 8 control points spaced evenly over the depth range. Linear between
// them, or a Catmull-Rom spline through them (smooth) clamped to 0..1. A curve with several peaks
// and valleys picks out several distances at once; a steep stretch spreads a thin slice of depth over
// the whole tonal range, which is what makes the shape of things inside that slice show up.
inline float curveTone(const float* pts, float d, bool smooth) {
    const float x = clampv(d, 0.0f, 1.0f) * 7.0f;
    const int i = std::min(static_cast<int>(x), 6);
    const float t = x - i;
    const float p1 = pts[i], p2 = pts[i + 1];
    if (!smooth) return clampv(p1 + (p2 - p1) * t, 0.0f, 1.0f);
    // Beyond the ends the curve continues in a straight line, so a straight ramp stays straight.
    const float p0 = i > 0 ? pts[i - 1] : 2.0f * pts[0] - pts[1];
    const float p3 = i < 6 ? pts[i + 2] : 2.0f * pts[7] - pts[6];
    const float v = 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
    return clampv(v, 0.0f, 1.0f);
}

// In-place box blur of an interleaved float image (running mean, edges clamped), `passes` times.
inline void boxBlurImage(std::vector<float>& img, int w, int h, int channels, int radius, int passes = 3) {
    if (radius < 1 || w < 1 || h < 1) return;
    std::vector<float> tmp(img.size());
    const int r = radius;
    for (int pass = 0; pass < passes; ++pass) {
        parallelFor(h, [&](int y) {
            for (int c = 0; c < channels; ++c) {
                const float* in = img.data() + static_cast<size_t>(y) * w * channels + c;
                float* out = tmp.data() + static_cast<size_t>(y) * w * channels + c;
                double acc = 0;
                for (int k = -r; k <= r; ++k) acc += in[static_cast<size_t>(clampv(k, 0, w - 1)) * channels];
                for (int x = 0; x < w; ++x) {
                    out[static_cast<size_t>(x) * channels] = static_cast<float>(acc / (2 * r + 1));
                    acc += in[static_cast<size_t>(std::min(x + r + 1, w - 1)) * channels] - in[static_cast<size_t>(std::max(x - r, 0)) * channels];
                }
            }
        });
        parallelFor(w, [&](int x) {
            for (int c = 0; c < channels; ++c) {
                double acc = 0;
                for (int k = -r; k <= r; ++k) acc += tmp[(static_cast<size_t>(clampv(k, 0, h - 1)) * w + x) * channels + c];
                for (int y = 0; y < h; ++y) {
                    img[(static_cast<size_t>(y) * w + x) * channels + c] = static_cast<float>(acc / (2 * r + 1));
                    acc += tmp[(static_cast<size_t>(std::min(y + r + 1, h - 1)) * w + x) * channels + c] -
                           tmp[(static_cast<size_t>(std::max(y - r, 0)) * w + x) * channels + c];
                }
            }
        });
    }
}

} // namespace depthyum
