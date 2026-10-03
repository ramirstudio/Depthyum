// Depthyum for After Effects: SmartFX wrapper around the depthyum_core depth engine.
// 8, 16 and 32 bpc, multi-frame rendering safe (the only shared state is the depth cache in
// depthyum_core, which has its own lock).
//
// Each frame renders from a window of neighbouring frames: PreRender asks for the layer at
// 2*Stability+1 times, SmartRender turns each into a network output (cached by pixel content),
// fuseTemporal combines them. A frame therefore renders the same whatever order it is asked for.

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "AE_EffectUI.h"
#include "AE_EffectSuites.h"
#include "Param_Utils.h"
#include "SPBasic.h"

#include "DepthyumParams.h"
#include "depthyum/DepthAI.h"
#include "depthyum/Output.h"
#include "depthyum/Parallel.h"
#include "depthyum/Temporal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#ifdef AE_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace depthyum;

namespace {

// ------------------------------------------------------------------------------------
// Parameters
// ------------------------------------------------------------------------------------

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data) {
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Output", 6, 1, "Depth Map|Colormap|Overlay|Source|Depth Scan|Scan Color", ID_VIEW);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Depth Source", ID_SRC_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Source", 2, 1, "AI Depth (built-in)|Depth Layer", ID_DEPTH_SOURCE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Depth Layer", PF_LayerDefault_NONE, ID_DEPTH_LAYER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Layer Polarity", 2, 1, "White Is Near|White Is Far", ID_LAYER_POLARITY);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Layer Range", 2, 1, "Use As Is|Stabilize Range", ID_LAYER_RANGE);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_SRC_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Depth", ID_DEPTH_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Encoding", 2, 1, "Disparity (white = near)|Z Distance (white = far)", ID_ENCODING);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Near Distance (m)", 0.05, 1000, 0.1, 10, 0.5, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_NEAR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Far Distance (m)", 0.1, 100000, 1, 200, 50, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_FAR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Invert", FALSE, 0, ID_INVERT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Far Cut", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_FAR_CUT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Near Cut", 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_NEAR_CUT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Contrast", 0.2, 5, 0.2, 5, 1, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_CONTRAST);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Shift", -50, 50, -50, 50, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_SHIFT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Smooth (px)", 0, 100, 0, 60, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_SMOOTH);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_DEPTH_TOPIC_END);

    // Depth Scan: a wave over depth whose phase moves with time. Used by the Depth Scan and
    // Scan Color outputs.
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Scan", ID_SCAN_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Speed (cycles/s)", -8, 8, -2, 2, 0.5, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SCAN_SPEED);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Phase", 0, ID_SCAN_PHASE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Frequency", 0.1, 16, 0.25, 8, 1, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SCAN_FREQ);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Shape", 3, 1, "Sine|Triangle|Sawtooth", ID_SCAN_SHAPE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Sharpness", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_SCAN_SHARP);
    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Dark Color", 30, 50, 255, ID_SCAN_COLOR_A);
    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Lit Color", 255, 40, 30, ID_SCAN_COLOR_B);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Gain", 0, 10, 0, 4, 1.5, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SCAN_GAIN);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Glow", 0, 5, 0, 2, 0.6, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, 0, ID_SCAN_GLOW);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Glow Radius (px)", 1, 400, 2, 100, 20, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, ID_SCAN_GLOW_RADIUS);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_SCAN_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Temporal", ID_TIME_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Stability (frames)", 0, kMaxRadius, 0, kMaxRadius, 3, ID_STABILITY);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Motion Tolerance", 1, 100, 1, 100, 25, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_TOLERANCE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Detect Cuts", TRUE, 0, ID_CUTS);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_TIME_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("AI Depth", ID_AI_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Detail", 4, 2, "Low|Medium|High|Ultra", ID_DETAIL);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Edge Refine", TRUE, 0, ID_REFINE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Use GPU", TRUE, 0, ID_GPU);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Input Color", 3, 1, "Auto|Linear|sRGB", ID_COLOR_MODE);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_AI_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Keep Source Alpha", FALSE, 0, ID_KEEP_ALPHA);

    out_data->num_params = P_COUNT;
    return PF_Err_NONE;
}

// Checks a parameter out, copies the value and checks it back in.
class ParamReader {
public:
    explicit ParamReader(PF_InData* in) : in_(in) {}
    bool ok() const { return err_ == PF_Err_NONE; }
    PF_Err err() const { return err_; }

    double num(int index) {
        PF_ParamDef p;
        if (!checkout(index, p)) return 0;
        double v = 0;
        switch (p.param_type) {
        case PF_Param_FLOAT_SLIDER: v = p.u.fs_d.value; break;
        case PF_Param_SLIDER: v = p.u.sd.value; break;
        case PF_Param_POPUP: v = p.u.pd.value; break;
        case PF_Param_CHECKBOX: v = p.u.bd.value; break;
        case PF_Param_ANGLE: v = FIX_2_FLOAT(p.u.ad.value); break; // degrees, revolutions included
        default: break;
        }
        checkin(p);
        return v;
    }
    // Colour parameter as display-referred 0..1 RGB.
    void color(int index, float* rgb) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        PF_ParamDef p;
        if (!checkout(index, p)) return;
        rgb[0] = p.u.cd.value.red / static_cast<float>(PF_MAX_CHAN8);
        rgb[1] = p.u.cd.value.green / static_cast<float>(PF_MAX_CHAN8);
        rgb[2] = p.u.cd.value.blue / static_cast<float>(PF_MAX_CHAN8);
        checkin(p);
    }

private:
    bool checkout(int index, PF_ParamDef& p) {
        AEFX_CLR_STRUCT(p);
        if (err_ != PF_Err_NONE) return false;
        err_ = PF_CHECKOUT_PARAM(in_, index, in_->current_time, in_->time_step, in_->time_scale, &p);
        return err_ == PF_Err_NONE;
    }
    void checkin(PF_ParamDef& p) {
        const PF_Err e = PF_CHECKIN_PARAM(in_, &p);
        if (err_ == PF_Err_NONE) err_ = e;
    }
    PF_InData* in_;
    PF_Err err_ = PF_Err_NONE;
};

// ------------------------------------------------------------------------------------
// Pixel access
// ------------------------------------------------------------------------------------

inline float srgbDecode(float v) {
    if (v <= 0.04045f) return v / 12.92f;
    return std::pow((v + 0.055f) / 1.055f, 2.4f);
}
inline float srgbEncode(float v) {
    if (v <= 0.0031308f) return v * 12.92f;
    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

struct WorldView {
    PF_EffectWorld* w = nullptr;
    PF_PixelFormat fmt = PF_PixelFormat_ARGB32;
    int width() const { return w ? w->width : 0; }
    int height() const { return w ? w->height : 0; }
    char* row(int y) const { return reinterpret_cast<char*>(w->data) + static_cast<size_t>(y) * w->rowbytes; }

    // Reads a pixel as premultiplied float RGBA. AE worlds are premultiplied already.
    void read(int x, int y, float* rgba) const {
        const char* r = row(y);
        switch (fmt) {
        case PF_PixelFormat_ARGB128: {
            const PF_PixelFloat& p = reinterpret_cast<const PF_PixelFloat*>(r)[x];
            rgba[0] = p.red; rgba[1] = p.green; rgba[2] = p.blue; rgba[3] = p.alpha;
            break;
        }
        case PF_PixelFormat_ARGB64: {
            const PF_Pixel16& p = reinterpret_cast<const PF_Pixel16*>(r)[x];
            const float k = 1.0f / PF_MAX_CHAN16;
            rgba[0] = p.red * k; rgba[1] = p.green * k; rgba[2] = p.blue * k; rgba[3] = p.alpha * k;
            break;
        }
        default: {
            const PF_Pixel8& p = reinterpret_cast<const PF_Pixel8*>(r)[x];
            const float k = 1.0f / PF_MAX_CHAN8;
            rgba[0] = p.red * k; rgba[1] = p.green * k; rgba[2] = p.blue * k; rgba[3] = p.alpha * k;
            break;
        }
        }
    }
    void write(int x, int y, const float* rgba) const {
        char* r = row(y);
        switch (fmt) {
        case PF_PixelFormat_ARGB128: {
            PF_PixelFloat& p = reinterpret_cast<PF_PixelFloat*>(r)[x];
            p.red = rgba[0]; p.green = rgba[1]; p.blue = rgba[2]; p.alpha = rgba[3];
            break;
        }
        case PF_PixelFormat_ARGB64: {
            PF_Pixel16& p = reinterpret_cast<PF_Pixel16*>(r)[x];
            auto q = [](float v) { return static_cast<A_u_short>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * PF_MAX_CHAN16)); };
            p.red = q(rgba[0]); p.green = q(rgba[1]); p.blue = q(rgba[2]); p.alpha = q(rgba[3]);
            break;
        }
        default: {
            PF_Pixel8& p = reinterpret_cast<PF_Pixel8*>(r)[x];
            auto q = [](float v) { return static_cast<A_u_char>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * PF_MAX_CHAN8)); };
            p.red = q(rgba[0]); p.green = q(rgba[1]); p.blue = q(rgba[2]); p.alpha = q(rgba[3]);
            break;
        }
        }
    }
};

PF_Err pixelFormat(PF_InData* in_data, PF_EffectWorld* world, PF_PixelFormat& fmt) {
    SPBasicSuite* sp = in_data->pica_basicP;
    const void* suite = nullptr;
    if (!sp || sp->AcquireSuite(kPFWorldSuite, kPFWorldSuiteVersion2, &suite) != kSPNoError || !suite)
        return PF_Err_BAD_CALLBACK_PARAM;
    const PF_WorldSuite2* ws = static_cast<const PF_WorldSuite2*>(suite);
    const PF_Err err = ws->PF_GetPixelFormat(world, &fmt);
    sp->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
    return err;
}

// What the network sees: unpremultiplied, display-referred RGB of the layer area of a world.
RgbSource makeSource(const WorldView& v, int x0, int y0, int cw, int ch, bool linear) {
    RgbSource s;
    s.w = cw;
    s.h = ch;
    s.get = [v, x0, y0, linear](int x, int y, float* rgb) {
        float p[4];
        v.read(x + x0, y + y0, p);
        const float ia = p[3] > 0 ? 1.0f / p[3] : 0.0f;
        for (int c = 0; c < 3; ++c) {
            float t = std::min(std::max(p[c] * ia, 0.0f), 1.0f);
            rgb[c] = linear ? srgbEncode(t) : t;
        }
    };
    return s;
}

// A world that is entirely transparent is a frame the layer does not have (before its start
// or after its end).
bool hasContent(const WorldView& v) {
    const int w = v.width(), h = v.height();
    if (w < 2 || h < 2) return false;
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i) {
            float p[4];
            v.read(i * (w - 1) / 7, j * (h - 1) / 7, p);
            if (p[3] > 0.002f) return true;
        }
    return false;
}

// A depth picture: luminance of the world, unpremultiplied, taken as data (no colour curve).
GraySource makeGray(const WorldView& v) {
    GraySource g;
    g.w = v.width();
    g.h = v.height();
    g.get = [v](int x, int y) {
        float p[4];
        v.read(x, y, p);
        float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        if (p[3] > 0) l /= p[3];
        return std::min(std::max(l, 0.0f), 1.0f);
    };
    return g;
}

// ------------------------------------------------------------------------------------
// Smart render
// ------------------------------------------------------------------------------------

struct PreRenderData {
    PF_LRect inRect;
    PF_LRect outRect;
    double layerW, layerH;
    int radius;
};

void deletePreRenderData(void* p) { delete static_cast<PreRenderData*>(p); }

// Folder the .aex lives in, with a trailing separator. The AI depth files sit next to it.
std::string pluginFolder() {
#ifdef AE_OS_WIN
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&pluginFolder), &self))
        return std::string();
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH * 2);
    std::wstring path(buf, n);
    const size_t cut = path.find_last_of(L"\\/");
    path = cut == std::wstring::npos ? std::wstring() : path.substr(0, cut + 1);
    const int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len : 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &out[0], len, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
#else
    return std::string();
#endif
}

void logLine(const std::string& text, bool truncate = false) {
#ifdef AE_OS_WIN
    char tmp[MAX_PATH + 1] = {0};
    if (GetTempPathA(MAX_PATH, tmp) > 0) {
        if (FILE* f = std::fopen((std::string(tmp) + "depthyum_log.txt").c_str(), truncate ? "w" : "a")) {
            std::fprintf(f, "%s\n", text.c_str());
            std::fclose(f);
        }
    }
#else
    (void)text;
    (void)truncate;
#endif
}

inline double ratio(const PF_RationalScale& r) { return r.den ? double(r.num) / double(r.den) : 1.0; }

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra) {
    PF_Err err = PF_Err_NONE;
    ParamReader pr(in_data);
    int radius = static_cast<int>(pr.num(P_STABILITY));
    const int view = static_cast<int>(pr.num(P_VIEW));
    const bool layerMode = pr.num(P_DEPTH_SOURCE) >= 2;
    if (!pr.ok()) return pr.err();
    radius = std::min(std::max(radius, 0), kMaxRadius);
    // Neighbours only matter for views that use the depth, and only when time moves.
    if (in_data->time_step <= 0 || view == 4) radius = 0;

    PF_RenderRequest req = extra->input->output_request;
    // The network needs the whole layer, whatever part After Effects asks to see.
    PF_RenderRequest fullReq = req;
    fullReq.rect.left = fullReq.rect.top = -30000;
    fullReq.rect.right = fullReq.rect.bottom = 30000;
    fullReq.preserve_rgb_of_zero_alpha = TRUE;

    PF_CheckoutResult centerRes;
    ERR(extra->cb->checkout_layer(in_data->effect_ref, P_INPUT, CHECKOUT_CENTER, &fullReq,
                                  in_data->current_time, in_data->time_step, in_data->time_scale, &centerRes));
    if (layerMode) {
        // The depth comes from the Depth Layer: its neighbouring frames matter, the input's do not.
        PF_CheckoutResult depthRes;
        ERR(extra->cb->checkout_layer(in_data->effect_ref, P_DEPTH_LAYER, CHECKOUT_DEPTH_CENTER, &fullReq,
                                      in_data->current_time, in_data->time_step, in_data->time_scale, &depthRes));
        for (int k = -radius; k <= radius && !err; ++k) {
            if (k == 0) continue;
            PF_CheckoutResult res;
            ERR(extra->cb->checkout_layer(in_data->effect_ref, P_DEPTH_LAYER, CHECKOUT_DEPTH_NEIGHBOUR + k + kMaxRadius, &fullReq,
                                          in_data->current_time + k * in_data->time_step, in_data->time_step,
                                          in_data->time_scale, &res));
        }
    } else {
        for (int k = -radius; k <= radius && !err; ++k) {
            if (k == 0) continue;
            PF_CheckoutResult res;
            ERR(extra->cb->checkout_layer(in_data->effect_ref, P_INPUT, CHECKOUT_NEIGHBOUR + k + kMaxRadius, &fullReq,
                                          in_data->current_time + k * in_data->time_step, in_data->time_step,
                                          in_data->time_scale, &res));
        }
    }
    if (err) return err;

    // Output: what was asked for, within what the input can provide (the layer does not grow).
    PF_LRect out = req.rect;
    out.left = std::max(out.left, centerRes.max_result_rect.left);
    out.top = std::max(out.top, centerRes.max_result_rect.top);
    out.right = std::min(out.right, centerRes.max_result_rect.right);
    out.bottom = std::min(out.bottom, centerRes.max_result_rect.bottom);
    if (out.right < out.left) out.right = out.left;
    if (out.bottom < out.top) out.bottom = out.top;

    extra->output->result_rect = out;
    extra->output->max_result_rect = centerRes.max_result_rect;
    extra->output->solid = FALSE;

    PreRenderData* prd = new (std::nothrow) PreRenderData;
    if (!prd) return PF_Err_OUT_OF_MEMORY;
    prd->inRect = centerRes.result_rect;
    prd->outRect = out;
    prd->layerW = centerRes.ref_width > 0 ? centerRes.ref_width : in_data->width;
    prd->layerH = centerRes.ref_height > 0 ? centerRes.ref_height : in_data->height;
    prd->radius = radius;
    extra->output->pre_render_data = prd;
    extra->output->delete_pre_render_data_func = deletePreRenderData;
    return err;
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    const PreRenderData* prd = static_cast<const PreRenderData*>(extra->input->pre_render_data);
    if (!prd) return PF_Err_BAD_CALLBACK_PARAM;

    PF_EffectWorld *inW = nullptr, *outW = nullptr, *depthCenterW = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, CHECKOUT_CENTER, &inW));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outW));

    if (!err && inW && outW) {
        WorldView in, out;
        in.w = inW;
        out.w = outW;
        ERR(pixelFormat(in_data, inW, in.fmt));
        ERR(pixelFormat(in_data, outW, out.fmt));

        ParamReader pr(in_data);
        const int view = static_cast<int>(pr.num(P_VIEW)) - 1; // 0 depth, 1 colormap, 2 overlay, 3 source
        const bool zEncoding = pr.num(P_ENCODING) >= 2;
        const double nearM = pr.num(P_NEAR), farM = pr.num(P_FAR);
        DepthAdjust adj;
        adj.invert = pr.num(P_INVERT) != 0;
        adj.farPoint = static_cast<float>(pr.num(P_FAR_CUT) / 100.0);
        adj.nearPoint = static_cast<float>(pr.num(P_NEAR_CUT) / 100.0);
        adj.gamma = static_cast<float>(pr.num(P_CONTRAST));
        adj.shift = static_cast<float>(pr.num(P_SHIFT) / 100.0);
        const double smoothPx = pr.num(P_SMOOTH);
        // Scan: the phase is the Phase angle plus Speed times the layer time, in cycles.
        const double layerSeconds = static_cast<double>(in_data->current_time) / std::max<double>(static_cast<double>(in_data->time_scale), 1.0);
        const double scanPhase = pr.num(P_SCAN_PHASE) / 360.0 + pr.num(P_SCAN_SPEED) * layerSeconds;
        const float scanFreq = static_cast<float>(pr.num(P_SCAN_FREQ));
        const int scanShape = std::min(std::max(static_cast<int>(pr.num(P_SCAN_SHAPE)) - 1, 0), 2);
        const float scanSharp = static_cast<float>(pr.num(P_SCAN_SHARP) / 100.0);
        float scanColA[3], scanColB[3];
        pr.color(P_SCAN_COLOR_A, scanColA);
        pr.color(P_SCAN_COLOR_B, scanColB);
        const float scanGain = static_cast<float>(pr.num(P_SCAN_GAIN));
        const float scanGlow = static_cast<float>(pr.num(P_SCAN_GLOW));
        const double glowRadiusPx = pr.num(P_SCAN_GLOW_RADIUS);
        TemporalParams tp;
        tp.tolerance = static_cast<float>(0.01 + 0.29 * pr.num(P_TOLERANCE) / 100.0);
        tp.detectCuts = pr.num(P_CUTS) != 0;
        const int detail = static_cast<int>(pr.num(P_DETAIL));
        const bool refine = pr.num(P_REFINE) != 0;
        const bool useGpu = pr.num(P_GPU) != 0;
        const int colorMode = static_cast<int>(pr.num(P_COLOR_MODE));
        const bool keepAlpha = pr.num(P_KEEP_ALPHA) != 0;
        const bool layerMode = pr.num(P_DEPTH_SOURCE) >= 2;
        const bool layerWhiteNear = pr.num(P_LAYER_POLARITY) < 2;
        tp.fixedRange = layerMode && pr.num(P_LAYER_RANGE) < 2;
        ERR(pr.err());

        const int W = in.width(), H = in.height();
        const double dsx = ratio(in_data->downsample_x);
        const int glowRadius = std::max(1, static_cast<int>(std::lround(glowRadiusPx * dsx)));
        // Layer area inside the buffer (the buffer may carry more than the layer).
        const int lx0 = std::max(0, static_cast<int>(-prd->inRect.left));
        const int ly0 = std::max(0, static_cast<int>(-prd->inRect.top));
        const int lx1 = std::min(W, static_cast<int>(std::lround(prd->layerW * dsx - prd->inRect.left)));
        const int ly1 = std::min(H, static_cast<int>(std::lround(prd->layerH * ratio(in_data->downsample_y) - prd->inRect.top)));
        const int cw = lx1 - lx0, ch = ly1 - ly0;
        const bool linear = colorMode == 2 || (colorMode == 1 && in.fmt == PF_PixelFormat_ARGB128);

        std::vector<float> depth;      // normalised disparity at layer resolution, 0 far .. 1 near
        std::vector<float> rgb;        // display-referred picture, for refinement and the overlay
        bool aiFailed = false;

        if (!err && view != 3 && cw > 1 && ch > 1) {
            std::string aiErr;
            DepthAIConfig cfg;
            cfg.runtimeLib = pluginFolder() + "depthyum_ort.dll";
            cfg.modelPath = pluginFolder() + "depthyum_depth.onnx";
            cfg.useGpu = useGpu;
            if (layerMode) {
                // Depth from a picture (a sequence baked offline): no network, no runtime files.
                const int R = prd->radius;
                std::vector<RawPtr> window(static_cast<size_t>(2 * R + 1));
                extra->cb->checkout_layer_pixels(in_data->effect_ref, CHECKOUT_DEPTH_CENTER, &depthCenterW);
                WorldView dv;
                dv.w = depthCenterW;
                if (!depthCenterW || pixelFormat(in_data, depthCenterW, dv.fmt) != PF_Err_NONE || !hasContent(dv)) {
                    aiErr = "Depth Layer is not set, or has no picture at this time";
                    aiFailed = true;
                } else if (!depthRawFromGray(makeGray(dv), 1536, layerWhiteNear, window[R], aiErr)) {
                    aiFailed = true;
                }
                for (int k = -R; k <= R && !aiFailed; ++k) {
                    if (k == 0) continue;
                    const int id = CHECKOUT_DEPTH_NEIGHBOUR + k + kMaxRadius;
                    PF_EffectWorld* nW = nullptr;
                    extra->cb->checkout_layer_pixels(in_data->effect_ref, id, &nW);
                    if (nW) {
                        WorldView nv;
                        nv.w = nW;
                        if (pixelFormat(in_data, nW, nv.fmt) == PF_Err_NONE && nv.width() == dv.width() && nv.height() == dv.height() && hasContent(nv)) {
                            std::string nErr;
                            RawPtr r;
                            if (depthRawFromGray(makeGray(nv), 1536, layerWhiteNear, r, nErr)) window[R + k] = r;
                            else logLine("neighbour depth frame skipped: " + nErr);
                        }
                        extra->cb->checkin_layer_pixels(in_data->effect_ref, id);
                    }
                }
                if (!aiFailed) {
                    std::vector<float> fused;
                    if (fuseTemporal(window, R, tp, fused)) {
                        resampleDepth(fused, window[R]->w, window[R]->h, depth, cw, ch);
                    } else {
                        aiErr = "temporal fusion failed";
                        aiFailed = true;
                    }
                }
            } else if (depthAIInit(cfg, aiErr)) {
                const int sides[4] = {392, 518, 770, 1022};
                const int side = sides[std::min(std::max(detail, 1), 4) - 1];
                const int R = prd->radius;
                std::vector<RawPtr> window(static_cast<size_t>(2 * R + 1));

                const RgbSource centerSrc = makeSource(in, lx0, ly0, cw, ch, linear);
                if (!depthAIRaw(centerSrc, side, window[R], aiErr)) aiFailed = true;

                for (int k = -R; k <= R && !aiFailed; ++k) {
                    if (k == 0) continue;
                    const int id = CHECKOUT_NEIGHBOUR + k + kMaxRadius;
                    PF_EffectWorld* nW = nullptr;
                    extra->cb->checkout_layer_pixels(in_data->effect_ref, id, &nW);
                    if (nW) {
                        WorldView nv;
                        nv.w = nW;
                        if (pixelFormat(in_data, nW, nv.fmt) == PF_Err_NONE && nv.width() == W && nv.height() == H && hasContent(nv)) {
                            std::string nErr;
                            RawPtr r;
                            if (depthAIRaw(makeSource(nv, lx0, ly0, cw, ch, linear), side, r, nErr)) window[R + k] = r;
                            else logLine("neighbour frame skipped: " + nErr);
                        }
                        extra->cb->checkin_layer_pixels(in_data->effect_ref, id);
                    }
                }

                if (!aiFailed) {
                    std::vector<float> fused;
                    if (fuseTemporal(window, R, tp, fused)) {
                        resampleDepth(fused, window[R]->w, window[R]->h, depth, cw, ch);
                    } else {
                        aiErr = "temporal fusion failed";
                        aiFailed = true;
                    }
                }
            } else {
                aiFailed = true;
            }

            if (aiFailed) {
                // The reason goes to %TEMP%\depthyum_log.txt so a failure is diagnosable.
                logLine("Depth failed: " + (aiErr.empty() ? std::string("layer too small") : aiErr) +
                            "\nruntime: " + cfg.runtimeLib + "\nmodel: " + cfg.modelPath,
                        true);
            } else {
                rgb.resize(static_cast<size_t>(cw) * ch * 3);
                const RgbSource s = makeSource(in, lx0, ly0, cw, ch, linear);
                parallelFor(ch, [&](int y) {
                    for (int x = 0; x < cw; ++x) s.get(x, y, &rgb[(static_cast<size_t>(y) * cw + x) * 3]);
                });
                if (refine) refineDepth(rgb.data(), cw, ch, depth, std::max(2, std::max(cw, ch) / 240), 2e-3f);
                adj.smoothRadius = static_cast<float>(smoothPx * cw / std::max(prd->layerW, 1.0));
                adjustDepth(depth, cw, ch, adj);
            }
        }

        // Depth Scan / Scan Color: a wave over depth, shifted by the phase at this time. The wave is
        // the grey output; the colour output tints the picture's own luminance with it and adds a glow.
        std::vector<float> scanMask;
        std::vector<float> scanRgb;
        if (!err && !aiFailed && !depth.empty() && (view == 4 || view == 5)) {
            scanMask.resize(depth.size());
            parallelFor(ch, [&](int y) {
                for (int x = 0; x < cw; ++x) {
                    const size_t i = static_cast<size_t>(y) * cw + x;
                    scanMask[i] = scanWave(depth[i], scanFreq, scanPhase, scanShape, scanSharp);
                }
            });
            if (view == 5) {
                scanRgb.resize(static_cast<size_t>(cw) * ch * 3);
                parallelFor(ch, [&](int y) {
                    for (int x = 0; x < cw; ++x) {
                        const size_t i = static_cast<size_t>(y) * cw + x;
                        const float* p = &rgb[i * 3];
                        const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                        const float m = scanMask[i];
                        for (int k = 0; k < 3; ++k)
                            scanRgb[i * 3 + k] = lum * scanGain * (scanColA[k] * (1.0f - m) + scanColB[k] * m);
                    }
                });
                if (scanGlow > 0.0f) {
                    std::vector<float> glow(scanRgb);
                    boxBlurImage(glow, cw, ch, 3, glowRadius);
                    for (size_t i = 0; i < scanRgb.size(); ++i) scanRgb[i] += scanGlow * glow[i];
                }
            }
        }

        if (!err) {
            const A_long ox = prd->outRect.left - prd->inRect.left;
            const A_long oy = prd->outRect.top - prd->inRect.top;
            parallelFor(out.height(), [&](int y) {
                const int sy = static_cast<int>(y + oy);
                for (int x = 0; x < out.width(); ++x) {
                    const int sx = static_cast<int>(x + ox);
                    float o[4] = {0, 0, 0, 0};
                    if (sx >= 0 && sy >= 0 && sx < W && sy < H) {
                        float src[4];
                        in.read(sx, sy, src);
                        const int lx = sx - lx0, ly = sy - ly0;
                        const bool inLayer = lx >= 0 && ly >= 0 && lx < cw && ly < ch;
                        const float a = keepAlpha ? src[3] : (inLayer ? 1.0f : 0.0f);
                        if (view == 3 && !aiFailed) {
                            for (int c = 0; c < 4; ++c) o[c] = src[c];
                        } else if (aiFailed) {
                            // Missing runtime or model: a flat red frame, so it cannot be taken for a result.
                            o[0] = a; o[3] = a;
                        } else if (inLayer && !depth.empty() && (view < 4 || !scanMask.empty())) {
                            const size_t i = static_cast<size_t>(ly) * cw + lx;
                            const float d = depth[i];
                            float c[3];
                            if (view == 0) {
                                // Depth is data: written as is, at every bit depth.
                                const float v = zEncoding ? zDistanceNormalised(d, nearM, farM) : d;
                                c[0] = c[1] = c[2] = v;
                            } else if (view == 4) {
                                c[0] = c[1] = c[2] = scanMask[i]; // data, like the depth map
                            } else if (view == 5) {
                                for (int k = 0; k < 3; ++k) c[k] = scanRgb[i * 3 + k];
                                if (linear)
                                    for (int k = 0; k < 3; ++k) c[k] = srgbDecode(std::max(c[k], 0.0f));
                            } else {
                                depthColor(d, c);
                                if (view == 2)
                                    for (int k = 0; k < 3; ++k) c[k] = 0.5f * rgb[i * 3 + k] + 0.5f * c[k];
                                if (linear)
                                    for (int k = 0; k < 3; ++k) c[k] = srgbDecode(c[k]);
                            }
                            o[0] = c[0] * a; o[1] = c[1] * a; o[2] = c[2] * a; o[3] = a;
                        }
                    }
                    out.write(x, y, o);
                }
            });
        }
    }

    ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, CHECKOUT_CENTER));
    if (depthCenterW) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, CHECKOUT_DEPTH_CENTER));
    return err ? err : err2;
}

PF_Err About(PF_InData* in_data, PF_OutData* out_data) {
    PF_SPRINTF(out_data->return_msg, "%s %d.%d\rAI depth maps with temporal stability: a Z-depth for every frame.",
               DEPTHYUM_NAME, DEPTHYUM_MAJOR, DEPTHYUM_MINOR);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data) {
    out_data->my_version = PF_VERSION(DEPTHYUM_MAJOR, DEPTHYUM_MINOR, DEPTHYUM_BUG, PF_Stage_RELEASE, DEPTHYUM_BUILD);
    // Must match AE_Effect_Global_OutFlags / _2 in DepthyumPiPL.r (and the 'global out flags' values in DepthyumPiPL.rc).
    // WIDE_TIME_INPUT: the render reads the layer at neighbouring times, so After Effects must not
    // reuse a cached frame when only a neighbour changed. NON_PARAM_VARY: the scan moves with time
    // even when no parameter and no source pixel changes (a still image).
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_WIDE_TIME_INPUT | PF_OutFlag_NON_PARAM_VARY;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

} // namespace

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                     const char* inHostVersion) {
    PF_Err result = PF_Err_INVALID_CALLBACK;
    result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, DEPTHYUM_NAME, DEPTHYUM_MATCH_NAME, DEPTHYUM_CATEGORY,
                                     AE_RESERVED_INFO, "EffectMain", DEPTHYUM_URL);
    return result;
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra) {
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT: err = About(in_data, out_data); break;
        case PF_Cmd_GLOBAL_SETUP: err = GlobalSetup(in_data, out_data); break;
        case PF_Cmd_PARAMS_SETUP: err = ParamsSetup(in_data, out_data); break;
        case PF_Cmd_SMART_PRE_RENDER: err = PreRender(in_data, out_data, static_cast<PF_PreRenderExtra*>(extra)); break;
        case PF_Cmd_SMART_RENDER: err = SmartRender(in_data, out_data, static_cast<PF_SmartRenderExtra*>(extra)); break;
        default: break;
        }
    } catch (const std::bad_alloc&) {
        logLine("exception: out of memory");
        err = PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        logLine("exception: unknown");
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
