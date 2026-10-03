#pragma once

#include "depthyum/DepthAI.h"

#include <vector>

namespace depthyum {

struct TemporalParams {
    float tolerance = 0.08f;   // depth difference (0..1) beyond which a neighbour stops counting
    bool detectCuts = true;    // never mix frames across a cut
    float cutThreshold = 0.12f; // mean absolute luminance change between thumbnails that counts as a cut
};

// True when two consecutive frames belong to different shots.
bool isCut(const RawDepth& a, const RawDepth& b, float threshold);

// Combines the network output of a window of frames into one stable, normalised map (0 = far,
// 1 = near) for frames[center]. It depends only on its arguments, so a frame renders the same
// whatever order After Effects asks for frames in.
//
// frames holds the window in time order; a null entry is a frame that does not exist (before the
// start or after the end of the layer). Frames are used outward from the centre until a null, a
// frame of another size or, with detectCuts, a cut. Three things happen:
//   1. the normalisation range (1st and 99th percentile of each frame) is averaged over the used
//      frames with Gaussian weights, so one frame's outlier does not move the whole map;
//   2. every used frame is normalised with that common range;
//   3. the maps are averaged per pixel. A neighbour's weight falls with its distance in time and
//      with how far its value is from the centre frame's at that pixel, so moving edges are not
//      smeared and only flicker is averaged away.
// used (optional) receives the number of frames that took part.
bool fuseTemporal(const std::vector<RawPtr>& frames, int center, const TemporalParams& p,
                  std::vector<float>& out, int* used = nullptr);

} // namespace depthyum
