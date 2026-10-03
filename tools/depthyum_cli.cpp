// Command-line harness for the Depthyum engine: depth maps from PPM images, so the model and the
// temporal stabilisation can be checked without After Effects.
//
//   depthyum_cli depth in.ppm out.pgm   aimodel=model.onnx ort=onnxruntime.dll [aires=518] [refine=1] [gpu=1]
//   depthyum_cli seq   out_prefix in0.ppm in1.ppm ... aimodel=... ort=... [radius=3] [tol=0.08]
//
// seq writes out_prefixNNN.pgm for every input, each fused with its neighbours the way the plug-in does.

#include "depthyum/DepthAI.h"
#include "depthyum/Temporal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace depthyum;

namespace {

struct Image { int w = 0, h = 0; std::vector<float> rgb; };

bool readPpm(const char* path, Image& im) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[3] = {0};
    int w = 0, h = 0, maxv = 0;
    if (std::fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxv) != 4 || std::strcmp(magic, "P6") != 0 || maxv != 255) { std::fclose(f); return false; }
    std::fgetc(f);
    std::vector<unsigned char> buf(static_cast<size_t>(w) * h * 3);
    const bool ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!ok) return false;
    im.w = w; im.h = h;
    im.rgb.resize(buf.size());
    for (size_t i = 0; i < buf.size(); ++i) im.rgb[i] = buf[i] / 255.0f;
    return true;
}

bool writePgm(const char* path, const std::vector<float>& d, int w, int h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (float v : d) std::fputc(static_cast<int>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f)), f);
    std::fclose(f);
    return true;
}

RgbSource sourceOf(const Image& im) {
    RgbSource s;
    s.w = im.w; s.h = im.h;
    const Image* p = &im;
    s.get = [p](int x, int y, float* rgb) {
        const float* q = &p->rgb[(static_cast<size_t>(y) * p->w + x) * 3];
        rgb[0] = q[0]; rgb[1] = q[1]; rgb[2] = q[2];
    };
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: see the header of tools/depthyum_cli.cpp\n"); return 2; }
    const std::string cmd = argv[1];
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    for (int i = 2; i < argc; ++i) {
        const char* eq = std::strchr(argv[i], '=');
        if (eq) opt[std::string(argv[i], static_cast<size_t>(eq - argv[i]))] = eq + 1; else pos.push_back(argv[i]);
    }
    auto num = [&](const char* k, double d) { auto it = opt.find(k); return it == opt.end() ? d : std::atof(it->second.c_str()); };

    DepthAIConfig cfg;
    cfg.runtimeLib = opt.count("ort") ? opt["ort"] : "onnxruntime.dll";
    cfg.modelPath = opt.count("aimodel") ? opt["aimodel"] : "lensyum_depth.onnx";
    cfg.useGpu = num("gpu", 1) != 0;
    std::string err;
    if (!depthAIInit(cfg, err)) { std::fprintf(stderr, "depth init failed: %s\n", err.c_str()); return 1; }
    const int side = static_cast<int>(num("aires", 518));

    if (cmd == "depth" && pos.size() == 2) {
        Image im;
        if (!readPpm(pos[0].c_str(), im)) { std::fprintf(stderr, "cannot read %s\n", pos[0].c_str()); return 1; }
        const auto t0 = std::chrono::steady_clock::now();
        RawPtr raw;
        if (!depthAIRaw(sourceOf(im), side, raw, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        TemporalParams p;
        std::vector<float> fused, out;
        fuseTemporal({raw}, 0, p, fused);
        resampleDepth(fused, raw->w, raw->h, out, im.w, im.h);
        if (num("refine", 1) != 0) refineDepth(im.rgb.data(), im.w, im.h, out, std::max(2, std::max(im.w, im.h) / 240), 2e-3f);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%dx%d, network %dx%d, %.0f ms\n", im.w, im.h, raw->w, raw->h, ms);
        return writePgm(pos[1].c_str(), out, im.w, im.h) ? 0 : 1;
    }

    if (cmd == "seq" && pos.size() >= 2) {
        const std::string prefix = pos[0];
        std::vector<Image> imgs(pos.size() - 1);
        std::vector<RawPtr> raws(imgs.size());
        for (size_t i = 0; i < imgs.size(); ++i) {
            if (!readPpm(pos[i + 1].c_str(), imgs[i])) { std::fprintf(stderr, "cannot read %s\n", pos[i + 1].c_str()); return 1; }
            if (!depthAIRaw(sourceOf(imgs[i]), side, raws[i], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        }
        TemporalParams p;
        p.tolerance = static_cast<float>(num("tol", 0.08));
        const int R = static_cast<int>(num("radius", 3));
        for (int i = 0; i < static_cast<int>(imgs.size()); ++i) {
            std::vector<RawPtr> window;
            for (int k = -R; k <= R; ++k) window.push_back(i + k >= 0 && i + k < static_cast<int>(imgs.size()) ? raws[i + k] : nullptr);
            std::vector<float> fused, out;
            int used = 0;
            fuseTemporal(window, R, p, fused, &used);
            resampleDepth(fused, raws[i]->w, raws[i]->h, out, imgs[i].w, imgs[i].h);
            if (num("refine", 1) != 0) refineDepth(imgs[i].rgb.data(), imgs[i].w, imgs[i].h, out, std::max(2, std::max(imgs[i].w, imgs[i].h) / 240), 2e-3f);
            char name[512];
            std::snprintf(name, sizeof(name), "%s%03d.pgm", prefix.c_str(), i);
            writePgm(name, out, imgs[i].w, imgs[i].h);
            std::printf("%s: %d frame(s) used\n", name, used);
        }
        return 0;
    }
    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
