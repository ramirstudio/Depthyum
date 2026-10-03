#include "depthyum/Temporal.h"
#include "depthyum/Math.h"
#include "depthyum/Parallel.h"

#include <algorithm>
#include <cmath>

namespace depthyum {

bool isCut(const RawDepth& a, const RawDepth& b, float threshold) {
    if (a.thumb.size() != b.thumb.size() || a.thumb.empty()) return false;
    double acc = 0;
    for (size_t i = 0; i < a.thumb.size(); ++i) acc += std::fabs(a.thumb[i] - b.thumb[i]);
    return acc / a.thumb.size() > threshold;
}

bool fuseTemporal(const std::vector<RawPtr>& frames, int center, const TemporalParams& p,
                  std::vector<float>& out, int* used) {
    const int n = static_cast<int>(frames.size());
    if (center < 0 || center >= n || !frames[center]) return false;
    const RawDepth& c = *frames[center];
    if (c.w < 1 || c.h < 1 || c.data.size() < static_cast<size_t>(c.w) * c.h) return false;

    // Frames that take part, as (offset from the centre, frame).
    std::vector<std::pair<int, const RawDepth*>> use;
    use.emplace_back(0, &c);
    for (int dir = -1; dir <= 1; dir += 2) {
        const RawDepth* prev = &c;
        for (int i = center + dir; i >= 0 && i < n; i += dir) {
            const RawDepth* f = frames[i].get();
            if (!f || f->w != c.w || f->h != c.h || f->data.size() != c.data.size()) break;
            if (p.detectCuts && isCut(*prev, *f, p.cutThreshold)) break;
            use.emplace_back(i - center, f);
            prev = f;
        }
    }
    if (used) *used = static_cast<int>(use.size());

    const int radius = std::max(center, n - 1 - center);
    const double sigma = std::max(0.5, radius / 2.0);
    std::vector<float> g(use.size());
    double wsum = 0, lo = 0, hi = 0;
    for (size_t i = 0; i < use.size(); ++i) {
        const double k = use[i].first;
        g[i] = static_cast<float>(std::exp(-0.5 * k * k / (sigma * sigma)));
        wsum += g[i];
        lo += g[i] * use[i].second->lo;
        hi += g[i] * use[i].second->hi;
    }
    lo /= wsum;
    hi /= wsum;
    const float inv = 1.0f / static_cast<float>(std::max(hi - lo, 1e-6));
    const float flo = static_cast<float>(lo);
    const float tau = std::max(p.tolerance, 1e-3f);
    const float invTau = 1.0f / tau;

    out.resize(c.data.size());
    const size_t total = c.data.size();
    const int chunks = std::max(1, static_cast<int>((total + 65535) / 65536));
    parallelFor(chunks, [&](int ch) {
        const size_t a = static_cast<size_t>(ch) * 65536;
        const size_t b = std::min(total, a + 65536);
        for (size_t i = a; i < b; ++i) {
            const float n0 = clampv((c.data[i] - flo) * inv, 0.0f, 1.0f);
            float num = n0, den = 1.0f;
            for (size_t f = 1; f < use.size(); ++f) {
                const float nk = clampv((use[f].second->data[i] - flo) * inv, 0.0f, 1.0f);
                const float d = (nk - n0) * invTau;
                const float w = g[f] * std::exp(-d * d);
                num += w * nk;
                den += w;
            }
            out[i] = num / den;
        }
    });
    return true;
}

} // namespace depthyum
