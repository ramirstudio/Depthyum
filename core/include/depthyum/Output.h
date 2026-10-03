#pragma once

#include "depthyum/Math.h"

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

} // namespace depthyum
