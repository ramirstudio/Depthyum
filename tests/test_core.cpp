// Tests of the engine without After Effects and without a model: the network is replaced by a
// function so the cache, the cut detection and the temporal fusion can be checked on any platform.

#include "depthyum/DepthAI.h"
#include "depthyum/Output.h"
#include "depthyum/Temporal.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace depthyum;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failed; } } while (0)
#define CHECK_NEAR(a, b, tol) do { const double A_ = (a), B_ = (b); if (std::fabs(A_ - B_) > (tol)) { std::printf("FAIL %s:%d  %s = %g, expected %g\n", __FILE__, __LINE__, #a, A_, B_); ++g_failed; } } while (0)

namespace {

// A scene of vertical bands plus a square: brightness encodes depth.
struct Scene {
    int w = 64, h = 36;
    float base = 0.3f;       // overall brightness, a cut changes it
    int squareX = 10;        // moving object
    float noise = 0.0f;
    uint32_t seed = 1;

    RgbSource source() const {
        Scene s = *this;
        RgbSource src;
        src.w = w; src.h = h;
        src.get = [s](int x, int y, float* rgb) {
            float v = s.base + 0.3f * (x / float(s.w));
            if (x >= s.squareX && x < s.squareX + 12 && y >= 10 && y < 26) v = 0.95f;
            if (s.noise > 0) {
                uint32_t k = s.seed * 2654435761u ^ (x * 40503u + y * 9973u);
                k ^= k >> 13; k *= 1274126177u; k ^= k >> 16;
                v += s.noise * ((k & 0xffff) / 65535.0f - 0.5f);
            }
            rgb[0] = rgb[1] = rgb[2] = std::min(std::max(v, 0.0f), 1.0f);
        };
        return src;
    }
};

std::atomic<int> g_runs{0};
float g_jitter = 0.0f;

// Fake network: depth follows the red channel (undoing the ImageNet normalisation), plus a
// per-call random gain/offset standing in for the model's frame-to-frame scatter.
bool fakeNet(const float* chw, int tw, int th, std::vector<float>& out, int& ow, int& oh, std::string&) {
    ++g_runs;
    ow = tw; oh = th;
    out.resize(static_cast<size_t>(tw) * th);
    static std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const float gain = 1.0f + g_jitter * u(rng), off = g_jitter * u(rng);
    for (size_t i = 0; i < out.size(); ++i) out[i] = (chw[i] * 0.229f + 0.485f) * 10.0f * gain + off;
    return true;
}

RawPtr raw(const Scene& s, int longSide = 140) {
    RawPtr r; std::string err;
    CHECK(depthAIRaw(s.source(), longSide, r, err));
    return r;
}

double meanAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double acc = 0;
    for (size_t i = 0; i < a.size(); ++i) acc += std::fabs(a[i] - b[i]);
    return acc / a.size();
}

void testCache() {
    depthAISetBackend(fakeNet);
    g_runs = 0; g_jitter = 0;
    Scene a;
    RawPtr r1 = raw(a), r2 = raw(a);
    CHECK(r1 && r2);
    CHECK(r1.get() == r2.get());           // same pixels, same entry
    CHECK(g_runs == 1);
    CHECK(r1->w % 14 == 0 && r1->h % 14 == 0);
    CHECK(r1->thumb.size() == size_t(kThumbW * kThumbH));
    CHECK(r1->hi > r1->lo);
    Scene b = a; b.squareX = 30;
    RawPtr r3 = raw(b);
    CHECK(r3.get() != r1.get());
    CHECK(g_runs == 2);
    CHECK(depthAICacheEntries() == 2);
    Scene big = a; big.w = 200; big.h = 112;
    RawPtr r4 = raw(big, 280);
    CHECK(r4 && r4->w == 280 && r4->h == 154);
}

void testCuts() {
    depthAISetBackend(fakeNet);
    Scene a, b = a; b.base = 0.75f;
    Scene c = a; c.squareX = 14;           // same shot, object moved a little
    RawPtr ra = raw(a), rb = raw(b), rc = raw(c);
    CHECK(isCut(*ra, *rb, 0.12f));
    CHECK(!isCut(*ra, *rc, 0.12f));
}

void testFuseReducesNoise() {
    depthAISetBackend(fakeNet);
    g_jitter = 0.0f;
    // Per-pixel noise that changes every frame: the fused map must be closer to the clean one.
    Scene clean;
    RawPtr rc = raw(clean);
    std::vector<RawPtr> window;
    for (int k = 0; k < 7; ++k) {
        Scene s = clean; s.noise = 0.12f; s.seed = 100 + k;
        window.push_back(raw(s));
    }
    TemporalParams p; p.tolerance = 0.2f;
    std::vector<float> single, fused, cleanMap;
    int used = 0;
    CHECK(fuseTemporal({rc}, 0, p, cleanMap));
    CHECK(fuseTemporal({window[3]}, 0, p, single));
    CHECK(fuseTemporal(window, 3, p, fused, &used));
    CHECK(used == 7);
    const double eSingle = meanAbsDiff(single, cleanMap), eFused = meanAbsDiff(fused, cleanMap);
    std::printf("noise: single %.4f, fused %.4f\n", eSingle, eFused);
    CHECK(eFused < eSingle * 0.8);
}

void testFuseKeepsMovingEdges() {
    depthAISetBackend(fakeNet);
    std::vector<RawPtr> window;
    for (int k = 0; k < 5; ++k) { Scene s; s.squareX = 10 + 6 * k; window.push_back(raw(s)); }
    TemporalParams p; p.tolerance = 0.05f;
    std::vector<float> fused, centre;
    CHECK(fuseTemporal(window, 2, p, fused));
    CHECK(fuseTemporal({window[2]}, 0, p, centre));
    // Where the square is in the centre frame the fused value must stay at the centre value.
    const RawDepth& c = *window[2];
    double dev = 0; int n = 0;
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
            const float v = centre[size_t(y) * c.w + x];
            if (v > 0.9f) { dev += std::fabs(fused[size_t(y) * c.w + x] - v); ++n; }
        }
    CHECK(n > 0);
    std::printf("moving edge: mean deviation inside the object %.4f\n", dev / std::max(n, 1));
    CHECK(dev / std::max(n, 1) < 0.03);
}

void testFuseStopsAtCutsAndGaps() {
    depthAISetBackend(fakeNet);
    Scene a, b = a; b.base = 0.8f; b.squareX = 40;
    std::vector<RawPtr> window = {raw(a), raw(a), raw(a), raw(b), raw(b)};
    // Make the frames distinct so the cache does not hand back one entry.
    Scene a1 = a; a1.squareX = 11; Scene a2 = a; a2.squareX = 12;
    Scene b1 = b; b1.squareX = 41;
    window = {raw(a), raw(a1), raw(a2), raw(b), raw(b1)};
    TemporalParams p;
    std::vector<float> out; int used = 0;
    CHECK(fuseTemporal(window, 1, p, out, &used));
    CHECK(used == 3);                      // a, a1, a2: the cut before b stops the window
    CHECK(fuseTemporal(window, 4, p, out, &used));
    CHECK(used == 2);                      // b, b1
    p.detectCuts = false;
    CHECK(fuseTemporal(window, 2, p, out, &used));
    CHECK(used == 5);
    p.detectCuts = true;
    std::vector<RawPtr> gap = window; gap[2] = nullptr;
    CHECK(!fuseTemporal(gap, 2, p, out));  // centre missing
    CHECK(fuseTemporal(gap, 1, p, out, &used));
    CHECK(used == 2);                      // a, a1: stops at the gap
    p.tolerance = 0.08f;
}

void testDeterministicOrder() {
    depthAISetBackend(fakeNet);
    g_jitter = 0.0f;
    std::vector<RawPtr> w;
    for (int k = 0; k < 5; ++k) { Scene s; s.noise = 0.1f; s.seed = 5 + k; w.push_back(raw(s)); }
    TemporalParams p;
    std::vector<float> o1, o2;
    CHECK(fuseTemporal(w, 2, p, o1));
    depthAIClearCache();
    std::vector<RawPtr> w2;
    for (int k = 4; k >= 0; --k) { Scene s; s.noise = 0.1f; s.seed = 5 + k; w2.insert(w2.begin(), raw(s)); }
    CHECK(fuseTemporal(w2, 2, p, o2));
    CHECK(meanAbsDiff(o1, o2) < 1e-6);
}

void testOutputs() {
    CHECK_NEAR(zDistanceNormalised(1.0f, 0.5, 50), 0.0, 1e-6);   // nearest -> near plane
    CHECK_NEAR(zDistanceNormalised(0.0f, 0.5, 50), 1.0, 1e-6);   // farthest -> far plane
    float prev = 2;
    for (int i = 0; i <= 10; ++i) {
        const float z = zDistanceNormalised(i / 10.0f, 0.5, 50);
        CHECK(z <= prev);                                         // nearer disparity, smaller z
        prev = z;
    }
    // Half disparity is much closer to the near plane than to the far one (1/z spacing).
    CHECK(zDistanceNormalised(0.5f, 0.5, 50) < 0.05f);
    float c0[3], c1[3];
    depthColor(0.0f, c0); depthColor(1.0f, c1);
    CHECK(c0[2] > c0[0]);                                         // far is blue
    CHECK(c1[0] > c1[2]);                                         // near is red
}

void testGraySource() {
    depthAISetBackend(fakeNet);
    auto make = [](float base, bool flip) {
        GraySource g;
        g.w = 64; g.h = 36;
        g.get = [base, flip](int x, int y) {
            float v = base + 0.5f * (x / 64.0f) + (y > 18 ? 0.0f : 0.05f);
            v = std::min(std::max(v, 0.0f), 1.0f);
            return flip ? 1.0f - v : v;
        };
        return g;
    };
    std::string err;
    RawPtr a, b, c, d;
    CHECK(depthRawFromGray(make(0.2f, false), 32, true, a, err));
    CHECK(a->w == 32 && a->h == 18);
    CHECK(depthRawFromGray(make(0.2f, false), 32, true, b, err));
    CHECK(a.get() == b.get());                       // cached
    CHECK(depthRawFromGray(make(0.2f, true), 32, false, c, err));
    // White-is-far input flipped gives the same depth as the white-is-near one.
    double diff = 0;
    for (size_t i = 0; i < a->data.size(); ++i) diff += std::fabs(a->data[i] - c->data[i]);
    CHECK(diff / a->data.size() < 1e-5);
    CHECK(a->hi > a->lo);
    // Larger values are nearer, and the long side is capped but never enlarged.
    CHECK(a->data.back() > a->data.front());
    CHECK(depthRawFromGray(make(0.2f, false), 500, true, d, err));
    CHECK(d->w == 64 && d->h == 36);
    // fixedRange keeps the values: a flat 0.3 stays 0.3, not stretched to the full range.
    GraySource flat; flat.w = 16; flat.h = 16; flat.get = [](int, int) { return 0.3f; };
    RawPtr f;
    CHECK(depthRawFromGray(flat, 16, true, f, err));
    TemporalParams p; p.fixedRange = true;
    std::vector<float> out;
    CHECK(fuseTemporal({f}, 0, p, out));
    CHECK_NEAR(out[0], 0.3, 1e-5);
    // The same frames with a different gain end up the same only when the range is stabilised.
    GraySource g1 = make(0.1f, false), g2 = make(0.1f, false);
    g2.get = [g1](int x, int y) { return 0.5f * g1.get(x, y); };
    RawPtr r1, r2;
    CHECK(depthRawFromGray(g1, 32, true, r1, err));
    CHECK(depthRawFromGray(g2, 32, true, r2, err));
    std::vector<float> o1, o2;
    TemporalParams free; free.fixedRange = false; free.tolerance = 0.3f;
    fuseTemporal({r1}, 0, free, o1);
    fuseTemporal({r2}, 0, free, o2);
    CHECK(meanAbsDiff(o1, o2) < 0.02);               // renormalised: gain does not matter
    TemporalParams fixed; fixed.fixedRange = true;
    fuseTemporal({r1}, 0, fixed, o1);
    fuseTemporal({r2}, 0, fixed, o2);
    CHECK(meanAbsDiff(o1, o2) > 0.05);               // used as is: gain shows
    depthAISetBackend(nullptr);
}

void testScan() {
    // Loops exactly: one more cycle of phase gives the same wave, at every depth.
    for (int shape = 0; shape < 3; ++shape)
        for (float d = 0.0f; d <= 1.0f; d += 0.1f) {
            CHECK_NEAR(scanWave(d, 1.5f, 0.3, shape, 0.2f), scanWave(d, 1.5f, 1.3, shape, 0.2f), 1e-4);
            CHECK_NEAR(scanWave(d, 1.0f, 0.3, shape, 0.0f), scanWave(d, 1.0f, -2.7, shape, 0.0f), 1e-4);
        }
    // Range, and the peak sits where the phase puts it.
    for (float d = 0.0f; d <= 1.0f; d += 0.05f) {
        const float v = scanWave(d, 2.0f, 0.1, kScanSine, 0.5f);
        CHECK(v >= 0.0f && v <= 1.0f);
    }
    CHECK_NEAR(scanWave(0.5f, 1.0f, 0.0, kScanSine, 0.0f), 1.0, 1e-5);   // u = 0.5
    CHECK_NEAR(scanWave(0.0f, 1.0f, 0.0, kScanSine, 0.0f), 0.0, 1e-5);   // u = 0
    CHECK_NEAR(scanWave(0.5f, 1.0f, 0.0, kScanTriangle, 0.0f), 1.0, 1e-5);
    // Advancing the phase moves the peak from near towards far (smaller d).
    CHECK(scanWave(0.3f, 1.0f, 0.2, kScanSine, 0.0f) > scanWave(0.3f, 1.0f, 0.0, kScanSine, 0.0f));
    // Sharpness narrows the band: below the midpoint gets darker, above gets brighter.
    CHECK(scanWave(0.1f, 1.0f, 0.0, kScanSine, 0.9f) < scanWave(0.1f, 1.0f, 0.0, kScanSine, 0.0f));
    CHECK(scanWave(0.3f, 1.0f, 0.0, kScanSine, 0.9f) > scanWave(0.3f, 1.0f, 0.0, kScanSine, 0.0f));
    // Blur keeps a constant image constant and conserves the mean of an impulse away from the edges.
    std::vector<float> flat(20 * 10 * 3, 0.4f);
    boxBlurImage(flat, 20, 10, 3, 3);
    for (float v : flat) CHECK_NEAR(v, 0.4, 1e-5);
    std::vector<float> dot(41 * 41, 0.0f);
    dot[20 * 41 + 20] = 1.0f;
    boxBlurImage(dot, 41, 41, 1, 3);
    double sum = 0; for (float v : dot) sum += v;
    CHECK_NEAR(sum, 1.0, 1e-4);
    CHECK(dot[20 * 41 + 20] < 0.1f && dot[20 * 41 + 20] > 0.0f);
}

void testResampleAndAdjust() {
    std::vector<float> small = {0, 1, 0, 1}, big;
    resampleDepth(small, 2, 2, big, 8, 8);
    CHECK(big.size() == 64);
    CHECK_NEAR(big[0], 0.0, 1e-6);
    CHECK_NEAR(big[7], 1.0, 1e-6);
    std::vector<float> d = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f};
    DepthAdjust a; a.invert = true;
    adjustDepth(d, 3, 3, a);
    CHECK_NEAR(d[0], 1.0, 1e-6);
    CHECK_NEAR(d[4], 0.0, 1e-6);
    std::vector<float> e(9, 0.5f);
    DepthAdjust cut; cut.farPoint = 0.4f; cut.nearPoint = 0.6f;
    adjustDepth(e, 3, 3, cut);
    CHECK_NEAR(e[0], 0.5, 1e-5);
}

} // namespace

int main() {
    testCache();
    testCuts();
    testFuseReducesNoise();
    testFuseKeepsMovingEdges();
    testFuseStopsAtCutsAndGaps();
    testDeterministicOrder();
    testOutputs();
    testScan();
    testGraySource();
    testResampleAndAdjust();
    depthAISetBackend(nullptr);
    if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
