#pragma once

// Parameter indices (order in the Effect Controls panel) and stable disk IDs.
// Never renumber disk IDs once projects have been saved with the plug-in; add new ones at the end.

enum {
    P_INPUT = 0,
    P_VIEW,

    P_SRC_TOPIC,
    P_DEPTH_SOURCE,
    P_DEPTH_LAYER,
    P_LAYER_POLARITY,
    P_LAYER_RANGE,
    P_SRC_TOPIC_END,

    P_DEPTH_TOPIC,
    P_ENCODING,
    P_NEAR,
    P_FAR,
    P_INVERT,
    P_FAR_CUT,
    P_NEAR_CUT,
    P_CONTRAST,
    P_SHIFT,
    P_SMOOTH,
    P_DEPTH_TOPIC_END,

    P_SCAN_TOPIC,
    P_SCAN_SPEED,
    P_SCAN_PHASE,
    P_SCAN_FREQ,
    P_SCAN_SHAPE,
    P_SCAN_SHARP,
    P_SCAN_COLOR_A,
    P_SCAN_COLOR_B,
    P_SCAN_GAIN,
    P_SCAN_GLOW,
    P_SCAN_GLOW_RADIUS,
    P_SCAN_TOPIC_END,

    P_TIME_TOPIC,
    P_STABILITY,
    P_TOLERANCE,
    P_CUTS,
    P_TIME_TOPIC_END,

    P_AI_TOPIC,
    P_DETAIL,
    P_REFINE,
    P_GPU,
    P_COLOR_MODE,
    P_AI_TOPIC_END,

    P_KEEP_ALPHA,

    P_COUNT
};

enum {
    ID_VIEW = 100,
    ID_DEPTH_TOPIC,
    ID_ENCODING,
    ID_NEAR,
    ID_FAR,
    ID_INVERT,
    ID_FAR_CUT,
    ID_NEAR_CUT,
    ID_CONTRAST,
    ID_SHIFT,
    ID_SMOOTH,
    ID_DEPTH_TOPIC_END,
    ID_TIME_TOPIC,
    ID_STABILITY,
    ID_TOLERANCE,
    ID_CUTS,
    ID_TIME_TOPIC_END,
    ID_AI_TOPIC,
    ID_DETAIL,
    ID_REFINE,
    ID_GPU,
    ID_COLOR_MODE,
    ID_AI_TOPIC_END,
    ID_KEEP_ALPHA,
    ID_SCAN_TOPIC,
    ID_SCAN_SPEED,
    ID_SCAN_PHASE,
    ID_SCAN_FREQ,
    ID_SCAN_SHAPE,
    ID_SCAN_SHARP,
    ID_SCAN_COLOR_A,
    ID_SCAN_COLOR_B,
    ID_SCAN_GAIN,
    ID_SCAN_GLOW,
    ID_SCAN_GLOW_RADIUS,
    ID_SCAN_TOPIC_END,
    ID_SRC_TOPIC,
    ID_DEPTH_SOURCE,
    ID_DEPTH_LAYER,
    ID_LAYER_POLARITY,
    ID_LAYER_RANGE,
    ID_SRC_TOPIC_END
};

// Largest Stability value: frames on each side of the current one that can take part.
constexpr int kMaxRadius = 12;

// Checkout IDs shared between PreRender and SmartRender. The centre frame has its own ID; the
// neighbour at offset k (from -kMaxRadius to kMaxRadius) uses CHECKOUT_NEIGHBOUR + k + kMaxRadius.
// The depth layer (Depth Source = Depth Layer) has its own centre ID and neighbours from CHECKOUT_DEPTH_NEIGHBOUR.
enum { CHECKOUT_CENTER = 1, CHECKOUT_DEPTH_CENTER = 2, CHECKOUT_NEIGHBOUR = 100, CHECKOUT_DEPTH_NEIGHBOUR = 200 };

#define DEPTHYUM_NAME        "Depthyum"
#define DEPTHYUM_MATCH_NAME  "RAMIR Depthyum"
#define DEPTHYUM_CATEGORY    "Depthyum"
#define DEPTHYUM_URL         "https://github.com/ramirstudio/Depthyum"

#define DEPTHYUM_MAJOR   1
#define DEPTHYUM_MINOR   0
#define DEPTHYUM_BUG     0
#define DEPTHYUM_BUILD   3
