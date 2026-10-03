#include "depthyum/DepthAI.h"
#include "depthyum/Math.h"
#include "depthyum/Parallel.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <utility>

#ifdef DEPTHYUM_WITH_ORT
#include "onnxruntime_c_api.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace depthyum {

// ---------------------------------------------------------------------------------------
// Guided filter (He, Sun, Tang): the depth follows the edges of the picture.
// ---------------------------------------------------------------------------------------
namespace {

void boxFilter(const std::vector<float>& in, std::vector<float>& out, int w, int h, int r) {
    std::vector<float> tmp(in.size());
    parallelFor(h, [&](int y) {
        const float* row = &in[static_cast<size_t>(y) * w];
        float* t = &tmp[static_cast<size_t>(y) * w];
        double acc = 0;
        for (int x = -r; x <= r; ++x) acc += row[clampv(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            t[x] = static_cast<float>(acc / (2 * r + 1));
            acc += row[std::min(x + r + 1, w - 1)] - row[std::max(x - r, 0)];
        }
    });
    out.resize(in.size());
    parallelFor(w, [&](int x) {
        double acc = 0;
        for (int y = -r; y <= r; ++y) acc += tmp[static_cast<size_t>(clampv(y, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            out[static_cast<size_t>(y) * w + x] = static_cast<float>(acc / (2 * r + 1));
            acc += tmp[static_cast<size_t>(std::min(y + r + 1, h - 1)) * w + x] - tmp[static_cast<size_t>(std::max(y - r, 0)) * w + x];
        }
    });
}

} // namespace

void refineDepth(const float* rgb, int w, int h, std::vector<float>& depth, int radius, float eps) {
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> I(n), Ip(n), II(n);
    for (size_t i = 0; i < n; ++i) {
        I[i] = 0.2126f * rgb[i * 3] + 0.7152f * rgb[i * 3 + 1] + 0.0722f * rgb[i * 3 + 2];
        Ip[i] = I[i] * depth[i];
        II[i] = I[i] * I[i];
    }
    std::vector<float> mI, mp, mIp, mII;
    boxFilter(I, mI, w, h, radius);
    boxFilter(depth, mp, w, h, radius);
    boxFilter(Ip, mIp, w, h, radius);
    boxFilter(II, mII, w, h, radius);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; ++i) {
        const float cov = mIp[i] - mI[i] * mp[i];
        const float var = mII[i] - mI[i] * mI[i];
        a[i] = cov / (var + eps);
        b[i] = mp[i] - a[i] * mI[i];
    }
    std::vector<float> ma, mb;
    boxFilter(a, ma, w, h, radius);
    boxFilter(b, mb, w, h, radius);
    for (size_t i = 0; i < n; ++i) depth[i] = clampv(ma[i] * I[i] + mb[i], 0.0f, 1.0f);
}

void resampleDepth(const std::vector<float>& src, int sw, int sh, std::vector<float>& dst, int dw, int dh) {
    dst.resize(static_cast<size_t>(dw) * dh);
    if (sw < 1 || sh < 1 || src.size() < static_cast<size_t>(sw) * sh) return;
    parallelFor(dh, [&](int y) {
        const double sy = clampv((y + 0.5) * sh / dh - 0.5, 0.0, static_cast<double>(sh - 1));
        const int y0 = std::min(static_cast<int>(sy), std::max(sh - 2, 0));
        const int y1 = std::min(y0 + 1, sh - 1);
        const float fy = static_cast<float>(sy - y0);
        for (int x = 0; x < dw; ++x) {
            const double sx = clampv((x + 0.5) * sw / dw - 0.5, 0.0, static_cast<double>(sw - 1));
            const int x0 = std::min(static_cast<int>(sx), std::max(sw - 2, 0));
            const int x1 = std::min(x0 + 1, sw - 1);
            const float fx = static_cast<float>(sx - x0);
            auto at = [&](int xx, int yy) { return src[static_cast<size_t>(yy) * sw + xx]; };
            const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
            const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
            dst[static_cast<size_t>(y) * dw + x] = a + (b - a) * fy;
        }
    });
}

// ---------------------------------------------------------------------------------------
// Shared state: backend, ONNX session and the cache of raw network outputs.
// ---------------------------------------------------------------------------------------
namespace {

constexpr size_t kCacheBudgetBytes = 384u * 1024u * 1024u;

struct State {
    std::mutex mtx;
    DepthBackend backend;
    bool ready = false;
    std::string loadedModel;
    std::list<RawPtr> cache; // most recently used first
    size_t cacheBytes = 0;
#ifdef DEPTHYUM_WITH_ORT
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    std::string inputName, outputName;
#endif
};

State& state() {
    static State s;
    return s;
}

size_t entryBytes(const RawDepth& r) { return r.data.size() * sizeof(float) + r.thumb.size() * sizeof(float) + sizeof(RawDepth); }

void clearCacheLocked(State& S) {
    S.cache.clear();
    S.cacheBytes = 0;
}

RawPtr findLocked(State& S, uint64_t key) {
    for (auto it = S.cache.begin(); it != S.cache.end(); ++it) {
        if ((*it)->key == key) {
            RawPtr hit = *it;
            S.cache.splice(S.cache.begin(), S.cache, it);
            return hit;
        }
    }
    return nullptr;
}

void insertLocked(State& S, const RawPtr& r) {
    S.cache.push_front(r);
    S.cacheBytes += entryBytes(*r);
    while (S.cache.size() > 1 && S.cacheBytes > kCacheBudgetBytes) {
        S.cacheBytes -= entryBytes(*S.cache.back());
        S.cache.pop_back();
    }
}

} // namespace

void depthAISetBackend(DepthBackend backend) {
    State& S = state();
    std::lock_guard<std::mutex> lock(S.mtx);
    S.backend = std::move(backend);
    clearCacheLocked(S);
}

void depthAIClearCache() {
    State& S = state();
    std::lock_guard<std::mutex> lock(S.mtx);
    clearCacheLocked(S);
}

size_t depthAICacheEntries() {
    State& S = state();
    std::lock_guard<std::mutex> lock(S.mtx);
    return S.cache.size();
}

#ifndef DEPTHYUM_WITH_ORT

bool depthAIInit(const DepthAIConfig&, std::string& err) {
    State& S = state();
    std::lock_guard<std::mutex> lock(S.mtx);
    if (S.backend) return true;
    err = "Depthyum was built without ONNX Runtime support";
    return false;
}

#else

namespace {

bool check(const OrtApi* api, OrtStatus* st, std::string& err) {
    if (!st) return true;
    err = api->GetErrorMessage(st);
    api->ReleaseStatus(st);
    return false;
}

// Runs a call whose failure is not fatal and frees its status.
void soft(const OrtApi* api, OrtStatus* st) {
    if (st) api->ReleaseStatus(st);
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
#endif

} // namespace

bool depthAIInit(const DepthAIConfig& cfg, std::string& err) {
    State& S = state();
    std::lock_guard<std::mutex> lock(S.mtx);
    if (S.backend) return true;
    if (S.ready && S.loadedModel == cfg.modelPath) return true;

    if (!S.api) {
        typedef const OrtApiBase*(ORT_API_CALL * GetApiBaseFn)();
        GetApiBaseFn getBase = nullptr;
#ifdef _WIN32
        // Altered search path: DirectML.dll and other dependencies are looked up next to the
        // runtime, not in the host application's folder.
        HMODULE lib = LoadLibraryExW(widen(cfg.runtimeLib).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lib) { err = "cannot load " + cfg.runtimeLib + " (Windows error " + std::to_string(GetLastError()) + ", 126 = a dependency such as DirectML.dll is missing, 193 = wrong architecture, 2 = file not found)"; return false; }
        getBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(lib, "OrtGetApiBase"));
#else
        void* lib = dlopen(cfg.runtimeLib.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) { err = "cannot load " + cfg.runtimeLib; return false; }
        getBase = reinterpret_cast<GetApiBaseFn>(dlsym(lib, "OrtGetApiBase"));
#endif
        if (!getBase) { err = "OrtGetApiBase not found in " + cfg.runtimeLib; return false; }
        S.api = getBase()->GetApi(ORT_API_VERSION);
        if (!S.api) { err = "ONNX Runtime too old for this build"; return false; }
        if (!check(S.api, S.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "depthyum", &S.env), err)) return false;
    }
    const OrtApi* api = S.api;
    if (S.session) { api->ReleaseSession(S.session); S.session = nullptr; }
    S.ready = false;

    OrtSessionOptions* so = nullptr;
    if (!check(api, api->CreateSessionOptions(&so), err)) return false;
    soft(api, api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
#ifdef _WIN32
    if (cfg.useGpu) {
        // DirectML runs on any DirectX 12 GPU; it needs sequential execution without memory patterns.
        soft(api, api->DisableMemPattern(so));
        soft(api, api->SetSessionExecutionMode(so, ORT_SEQUENTIAL));
        typedef OrtStatus*(ORT_API_CALL * AppendDmlFn)(OrtSessionOptions*, int);
        HMODULE lib = GetModuleHandleW(widen(cfg.runtimeLib.substr(cfg.runtimeLib.find_last_of("\\/") + 1)).c_str());
        AppendDmlFn appendDml = lib ? reinterpret_cast<AppendDmlFn>(GetProcAddress(lib, "OrtSessionOptionsAppendExecutionProvider_DML")) : nullptr;
        if (appendDml) {
            OrtStatus* st = appendDml(so, 0);
            if (st) api->ReleaseStatus(st); // fall back to the CPU provider
        }
    }
    OrtStatus* st = api->CreateSession(S.env, widen(cfg.modelPath).c_str(), so, &S.session);
#else
    (void)cfg.useGpu;
    OrtStatus* st = api->CreateSession(S.env, cfg.modelPath.c_str(), so, &S.session);
#endif
    api->ReleaseSessionOptions(so);
    if (!check(api, st, err)) { S.session = nullptr; return false; }

    OrtAllocator* alloc = nullptr;
    soft(api, api->GetAllocatorWithDefaultOptions(&alloc));
    char* name = nullptr;
    if (check(api, api->SessionGetInputName(S.session, 0, alloc, &name), err)) { S.inputName = name; soft(api, api->AllocatorFree(alloc, name)); }
    if (check(api, api->SessionGetOutputName(S.session, 0, alloc, &name), err)) { S.outputName = name; soft(api, api->AllocatorFree(alloc, name)); }
    clearCacheLocked(S);
    S.loadedModel = cfg.modelPath;
    S.ready = true;
    return true;
}

#endif

namespace {

// One forward pass. Called with the state lock held.
bool inferLocked(State& S, std::vector<float>& input, int tw, int th, std::vector<float>& net, int& nw, int& nh, std::string& err) {
    if (S.backend) return S.backend(input.data(), tw, th, net, nw, nh, err);
#ifndef DEPTHYUM_WITH_ORT
    err = "Depthyum was built without ONNX Runtime support";
    return false;
#else
    if (!S.ready) { err = "depth model not loaded"; return false; }
    const OrtApi* api = S.api;
    OrtMemoryInfo* mem = nullptr;
    if (!check(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem), err)) return false;
    const int64_t shape[4] = {1, 3, th, tw};
    OrtValue* in = nullptr;
    OrtStatus* st = api->CreateTensorWithDataAsOrtValue(mem, input.data(), input.size() * sizeof(float), shape, 4,
                                                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in);
    api->ReleaseMemoryInfo(mem);
    if (!check(api, st, err)) return false;
    const char* inNames[1] = {S.inputName.c_str()};
    const char* outNames[1] = {S.outputName.c_str()};
    OrtValue* outV = nullptr;
    st = api->Run(S.session, nullptr, inNames, &in, 1, outNames, 1, &outV);
    api->ReleaseValue(in);
    if (!check(api, st, err)) return false;
    OrtTensorTypeAndShapeInfo* info = nullptr;
    soft(api, api->GetTensorTypeAndShape(outV, &info));
    size_t nd = 0;
    soft(api, api->GetDimensionsCount(info, &nd));
    std::vector<int64_t> d(nd);
    soft(api, api->GetDimensions(info, d.data(), nd));
    api->ReleaseTensorTypeAndShapeInfo(info);
    if (nd < 2) { api->ReleaseValue(outV); err = "unexpected model output"; return false; }
    nh = static_cast<int>(d[nd - 2]);
    nw = static_cast<int>(d[nd - 1]);
    float* p = nullptr;
    soft(api, api->GetTensorMutableData(outV, reinterpret_cast<void**>(&p)));
    if (!p) { api->ReleaseValue(outV); err = "empty model output"; return false; }
    net.assign(p, p + static_cast<size_t>(nw) * nh);
    api->ReleaseValue(outV);
    return true;
#endif
}

} // namespace

bool depthAIRaw(const RgbSource& src, int inferLongSide, RawPtr& out, std::string& err) {
    State& S = state();
    const int w = src.w, h = src.h;
    if (w < 2 || h < 2 || !src.get) { err = "image too small"; return false; }

    // Network input size: long side as requested, both sides multiples of 14.
    const double sc = std::max(inferLongSide, 56) / static_cast<double>(std::max(w, h));
    const int tw = std::max(14, static_cast<int>(std::lround(w * sc / 14.0)) * 14);
    const int th = std::max(14, static_cast<int>(std::lround(h * sc / 14.0)) * 14);

    std::vector<float> input(static_cast<size_t>(3) * tw * th);
    const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
    parallelFor(th, [&](int y) {
        const double sy = clampv((y + 0.5) * h / th - 0.5, 0.0, h - 1.001);
        const int y0 = static_cast<int>(sy);
        const float fy = static_cast<float>(sy - y0);
        for (int x = 0; x < tw; ++x) {
            const double sx = clampv((x + 0.5) * w / tw - 0.5, 0.0, w - 1.001);
            const int x0 = static_cast<int>(sx);
            const float fx = static_cast<float>(sx - x0);
            float p00[3], p10[3], p01[3], p11[3];
            src.get(x0, y0, p00);
            src.get(x0 + 1, y0, p10);
            src.get(x0, y0 + 1, p01);
            src.get(x0 + 1, y0 + 1, p11);
            for (int c = 0; c < 3; ++c) {
                const float a = p00[c] + (p10[c] - p00[c]) * fx;
                const float b = p01[c] + (p11[c] - p01[c]) * fx;
                const float v = clampv(a + (b - a) * fy, 0.0f, 1.0f);
                input[(static_cast<size_t>(c) * th + y) * tw + x] = (v - mean[c]) / stdv[c];
            }
        }
    });

    // Cache key: the network input itself (every 5th value), so a frame is recognised by its
    // pixels, not by when it was asked for.
    Hasher hs;
    hs.add(tw);
    hs.add(th);
    for (size_t i = 0; i < input.size(); i += 5) hs.add(input[i]);
    const uint64_t key = hs.h;

    std::lock_guard<std::mutex> lock(S.mtx);
    if (RawPtr hit = findLocked(S, key)) { out = hit; return true; }

    std::vector<float> net;
    int nw = 0, nh = 0;
    if (!inferLocked(S, input, tw, th, net, nw, nh, err)) return false;
    if (nw < 2 || nh < 2 || net.size() < static_cast<size_t>(nw) * nh) { err = "unexpected model output size"; return false; }

    auto raw = std::make_shared<RawDepth>();
    raw->w = nw;
    raw->h = nh;
    raw->key = key;
    raw->data.assign(net.begin(), net.begin() + static_cast<size_t>(nw) * nh);

    // Robust range: 1st and 99th percentile, so single outliers do not flatten the map.
    {
        std::vector<float> tmp(raw->data);
        const size_t n = tmp.size();
        const size_t iLo = n / 100, iHi = n - 1 - n / 100;
        std::nth_element(tmp.begin(), tmp.begin() + iLo, tmp.end());
        raw->lo = tmp[iLo];
        std::nth_element(tmp.begin() + iLo, tmp.begin() + iHi, tmp.end());
        raw->hi = tmp[iHi];
    }

    // Thumbnail of the input luminance for cut detection.
    {
        std::vector<double> acc(static_cast<size_t>(kThumbW) * kThumbH, 0.0);
        std::vector<int> cnt(acc.size(), 0);
        for (int y = 0; y < th; ++y) {
            const int by = std::min(kThumbH - 1, y * kThumbH / th);
            for (int x = 0; x < tw; ++x) {
                const int bx = std::min(kThumbW - 1, x * kThumbW / tw);
                const size_t i = static_cast<size_t>(y) * tw + x, plane = static_cast<size_t>(tw) * th;
                const float r = input[i] * stdv[0] + mean[0];
                const float g = input[plane + i] * stdv[1] + mean[1];
                const float b = input[2 * plane + i] * stdv[2] + mean[2];
                acc[static_cast<size_t>(by) * kThumbW + bx] += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                cnt[static_cast<size_t>(by) * kThumbW + bx]++;
            }
        }
        raw->thumb.resize(acc.size());
        for (size_t i = 0; i < acc.size(); ++i) raw->thumb[i] = cnt[i] ? static_cast<float>(acc[i] / cnt[i]) : 0.0f;
    }

    insertLocked(S, raw);
    out = raw;
    return true;
}

bool depthRawFromGray(const GraySource& src, int maxSide, bool whiteIsNear, RawPtr& out, std::string& err) {
    State& S = state();
    const int w = src.w, h = src.h;
    if (w < 2 || h < 2 || !src.get) { err = "depth layer too small"; return false; }

    // Resample to at most maxSide on the long side (box average when shrinking, so thin
    // structures do not alias).
    const double sc = std::min(1.0, std::max(maxSide, 16) / static_cast<double>(std::max(w, h)));
    const int nw = std::max(2, static_cast<int>(std::lround(w * sc)));
    const int nh = std::max(2, static_cast<int>(std::lround(h * sc)));
    auto raw = std::make_shared<RawDepth>();
    raw->w = nw;
    raw->h = nh;
    raw->data.resize(static_cast<size_t>(nw) * nh);
    parallelFor(nh, [&](int y) {
        const int y0 = std::min(h - 1, static_cast<int>(static_cast<double>(y) * h / nh));
        const int y1 = std::max(y0 + 1, std::min(h, static_cast<int>(static_cast<double>(y + 1) * h / nh)));
        for (int x = 0; x < nw; ++x) {
            const int x0 = std::min(w - 1, static_cast<int>(static_cast<double>(x) * w / nw));
            const int x1 = std::max(x0 + 1, std::min(w, static_cast<int>(static_cast<double>(x + 1) * w / nw)));
            double acc = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) acc += src.get(xx, yy);
            const float v = static_cast<float>(acc / ((y1 - y0) * (x1 - x0)));
            raw->data[static_cast<size_t>(y) * nw + x] = whiteIsNear ? v : 1.0f - v;
        }
    });

    // Key: the picture itself plus a tag that keeps it apart from network outputs.
    Hasher hs;
    hs.add(nw);
    hs.add(nh);
    hs.add(static_cast<int>(whiteIsNear));
    hs.add(0x6772617955ULL);
    for (size_t i = 0; i < raw->data.size(); i += 3) hs.add(raw->data[i]);
    raw->key = hs.h;

    {
        std::lock_guard<std::mutex> lock(S.mtx);
        if (RawPtr hit = findLocked(S, raw->key)) { out = hit; return true; }
    }

    {
        std::vector<float> tmp(raw->data);
        const size_t n = tmp.size();
        const size_t iLo = n / 100, iHi = n - 1 - n / 100;
        std::nth_element(tmp.begin(), tmp.begin() + iLo, tmp.end());
        raw->lo = tmp[iLo];
        std::nth_element(tmp.begin() + iLo, tmp.begin() + iHi, tmp.end());
        raw->hi = tmp[iHi];
    }
    // Thumbnail of the depth itself: a cut changes the depth picture as well.
    raw->thumb.assign(static_cast<size_t>(kThumbW) * kThumbH, 0.0f);
    {
        std::vector<int> cnt(raw->thumb.size(), 0);
        for (int y = 0; y < nh; ++y) {
            const int by = std::min(kThumbH - 1, y * kThumbH / nh);
            for (int x = 0; x < nw; ++x) {
                const int bx = std::min(kThumbW - 1, x * kThumbW / nw);
                raw->thumb[static_cast<size_t>(by) * kThumbW + bx] += raw->data[static_cast<size_t>(y) * nw + x];
                cnt[static_cast<size_t>(by) * kThumbW + bx]++;
            }
        }
        for (size_t i = 0; i < raw->thumb.size(); ++i)
            if (cnt[i]) raw->thumb[i] /= cnt[i];
    }
    {
        std::lock_guard<std::mutex> lock(S.mtx);
        insertLocked(S, raw);
    }
    out = raw;
    return true;
}

void adjustDepth(std::vector<float>& d, int w, int h, const DepthAdjust& a) {
    if (w < 1 || h < 1 || d.size() < static_cast<size_t>(w) * h) return;
    const float span = std::max(a.nearPoint - a.farPoint, 0.02f);
    const float g = std::max(a.gamma, 0.05f);
    for (float& v : d) {
        if (a.invert) v = 1.0f - v;
        v = std::min(std::max((v - a.farPoint) / span, 0.0f), 1.0f);
        if (g != 1.0f) v = std::pow(v, g);
        v = std::min(std::max(v + a.shift, 0.0f), 1.0f);
    }
    const int r = static_cast<int>(std::lround(a.smoothRadius));
    if (r > 0) {
        std::vector<float> tmp(d.size());
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = 0; y < h; ++y) { // horizontal running mean, edges clamped
                const float* in = d.data() + static_cast<size_t>(y) * w;
                float* out = tmp.data() + static_cast<size_t>(y) * w;
                double acc = 0;
                for (int k = -r; k <= r; ++k) acc += in[std::min(std::max(k, 0), w - 1)];
                for (int x = 0; x < w; ++x) {
                    out[x] = static_cast<float>(acc / (2 * r + 1));
                    acc += in[std::min(x + r + 1, w - 1)] - in[std::max(x - r, 0)];
                }
            }
            for (int x = 0; x < w; ++x) { // vertical
                double acc = 0;
                for (int k = -r; k <= r; ++k) acc += tmp[static_cast<size_t>(std::min(std::max(k, 0), h - 1)) * w + x];
                for (int y = 0; y < h; ++y) {
                    d[static_cast<size_t>(y) * w + x] = static_cast<float>(acc / (2 * r + 1));
                    acc += tmp[static_cast<size_t>(std::min(y + r + 1, h - 1)) * w + x] - tmp[static_cast<size_t>(std::max(y - r, 0)) * w + x];
                }
            }
        }
    }
}

} // namespace depthyum
