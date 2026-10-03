#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace depthyum {

// Monocular depth estimation with an ONNX model of the Depth Anything family (input: RGB image
// normalised with ImageNet statistics, sides multiple of 14; output: relative inverse depth).
// The ONNX Runtime library is loaded at run time from runtimeLib, so the plug-in has no link-time
// dependency and never collides with a runtime the host application may already have loaded.
struct DepthAIConfig {
    std::string runtimeLib; // path to onnxruntime shared library
    std::string modelPath;  // path to the .onnx model
    bool useGpu = true;     // DirectML on Windows when available
};

// A picture the network can read: display-referred (sRGB) values 0..1, row 0 = top.
// get() is called from several threads at once.
struct RgbSource {
    int w = 0, h = 0;
    std::function<void(int x, int y, float* rgb)> get;
};

constexpr int kThumbW = 32;
constexpr int kThumbH = 18;

// What the network wrote for one frame, kept as it came out so that neighbouring frames can be
// combined later. Immutable once built, shared between the cache and the renderer.
struct RawDepth {
    int w = 0, h = 0;
    std::vector<float> data;  // relative inverse depth, larger = nearer
    float lo = 0, hi = 1;     // 1st and 99th percentile of data
    std::vector<float> thumb; // kThumbW * kThumbH luminance of the network input, for cut detection
    uint64_t key = 0;
};
using RawPtr = std::shared_ptr<const RawDepth>;

// Thread-safe. Returns false and fills err when the runtime or the model cannot be loaded.
bool depthAIInit(const DepthAIConfig& cfg, std::string& err);

// Runs the network on src (or finds the result in the cache). inferLongSide is the network input
// size along the long side, rounded to multiples of 14. The cache is keyed on the pixels, so the
// same frame requested again, from this render or from a neighbouring one, costs nothing.
bool depthAIRaw(const RgbSource& src, int inferLongSide, RawPtr& out, std::string& err);

// Depth that already exists as a picture, for example a sequence baked offline with a heavier model
// such as Marigold V2. get() returns the luminance 0..1 at a pixel (called from several threads).
struct GraySource {
    int w = 0, h = 0;
    std::function<float(int x, int y)> get;
};

// Wraps a depth picture as a RawDepth, resampled so its long side is at most maxSide, with larger
// values meaning nearer (whiteIsNear false flips it). Cached like the network output.
bool depthRawFromGray(const GraySource& src, int maxSide, bool whiteIsNear, RawPtr& out, std::string& err);

// Replaces ONNX inference with a function (tests, command-line checks). An empty function goes back
// to ONNX. Clears the cache either way.
using DepthBackend = std::function<bool(const float* chw, int w, int h, std::vector<float>& out, int& ow, int& oh, std::string& err)>;
void depthAISetBackend(DepthBackend backend);
void depthAIClearCache();
size_t depthAICacheEntries();

// Bilinear resampling of a single-channel map.
void resampleDepth(const std::vector<float>& src, int sw, int sh, std::vector<float>& dst, int dw, int dh);

// Edge-aware refinement of a depth map with the image as guide (guided filter).
void refineDepth(const float* rgb, int w, int h, std::vector<float>& depth, int radius, float eps);

// Shapes a normalised depth map (0 = far, 1 = near).
//   farPoint/nearPoint: levels, values at or below farPoint become 0 and at or above nearPoint 1.
//   gamma: > 1 pushes the middle towards far, < 1 towards near. shift: moves all depths.
//   smoothRadius: box blur in map pixels (two passes).
struct DepthAdjust {
    float farPoint = 0.0f, nearPoint = 1.0f, gamma = 1.0f, shift = 0.0f;
    bool invert = false;
    float smoothRadius = 0.0f;
};
void adjustDepth(std::vector<float>& depth, int w, int h, const DepthAdjust& a);

} // namespace depthyum
