# Depthyum architecture

Depthyum produces a depth map for a layer. The engine is a C++17 library with no dependencies (`core/`), used by an After Effects SmartFX wrapper (`ae/`) and by a test CLI (`tools/depthyum_cli.cpp`). The network runs through ONNX Runtime, on the GPU through DirectML when available.

## AI depth

`DepthAI.cpp` loads ONNX Runtime at run time (`LoadLibraryExW`, `OrtGetApiBase`, DirectML provider with CPU fallback), so there is no link-time dependency and no clash with the runtime of other applications. This part is the one Lensyum uses. The model is Depth Anything V2 Small: RGB input normalised with ImageNet statistics at sides that are multiples of 14, relative disparity output.

`depthAIRaw` builds the network input by sampling an `RgbSource` (a callback, so the wrapper reads straight from the After Effects buffer in 8, 16 or 32 bit and no full-frame copy is made), runs the network and returns a `RawDepth`: the output exactly as the network wrote it, its 1st and 99th percentile, and a 32 × 18 luminance thumbnail of the input. Results are kept in a cache keyed on the network input pixels (every 5th value hashed) with a 384 MB budget, so a frame asked for again, by the same render or by a neighbouring one, is not estimated twice.

## Temporal fusion

`Temporal.cpp`, `fuseTemporal`. The input is a window of `RawDepth` in time order with the frame to render at `center`; a null entry is a frame that does not exist. The result depends only on the arguments.

1. The window is cut where a frame is missing, has another size, or where two consecutive thumbnails differ by more than a threshold (mean absolute luminance change, 0.12 by default). Offsets are walked outward from the centre and stop at the first break.
2. The normalisation range is the Gaussian-weighted mean (sigma = half the window radius) of each used frame's 1st and 99th percentile. All used frames are normalised with this one range.
3. The maps are averaged per pixel with weight `g(k) * exp(-((n_k - n_0) / tau)^2)`, where `g` is the Gaussian of the offset and `n_0` is the centre frame's value at that pixel. Motion Tolerance sets `tau`.

The range is shared because the model's scale and offset change from frame to frame: normalising each frame with its own percentiles removes that scatter but makes the map pump whenever the content changes, while a range averaged over the window follows real changes smoothly. The assumption is that the raw outputs of neighbouring frames are on a comparable scale. When they are not, Stability 0 gives plain per-frame normalisation.

## After Effects wrapper

`DepthyumAE.cpp` implements SmartFX (`PreRender`, `SmartRender`) at 8, 16 and 32 bit, with the multi-frame rendering flags. The effect is flagged `PF_OutFlag_WIDE_TIME_INPUT` because it reads other times. `PreRender` checks the layer out at the current time and at k = -R..R time steps, always the whole layer, since the network needs it whatever part After Effects asks to see. `SmartRender` turns the centre frame and each neighbour into a `RawDepth`, checking each neighbour back in as soon as it is read so only one extra frame is held at a time; neighbours that are fully transparent (outside the layer) are left out of the window. The fused map is resampled to the layer area, refined against the image with a guided filter, shaped (`adjustDepth`), encoded as disparity or Z distance and written as grey, colormap or overlay.

Parameters have indices in panel order (`DepthyumParams.h`) and stable disk IDs, always appended at the end. The PiPL is pre-generated in `DepthyumPiPL.rc` as in Lensyum and must be kept consistent with the flags in `GlobalSetup`. The plug-in looks for `depthyum_ort.dll` and `depthyum_depth.onnx` next to the `.aex`; if either is missing the frame comes out solid red and the reason is written to `%TEMP%\depthyum_log.txt`.

## Build

CMake builds `depthyum_core`, `depthyum_cli`, `depthyum_tests` and, with `AE_SDK_DIR`, the `.aex` (MSVC/Windows only). `ORT_INCLUDE_DIR` enables AI depth; without it the network functions return an error unless a test backend is installed with `depthAISetBackend`.

## Known limits

Windows only. The first render of a frame estimates up to 2R + 1 frames, so scrubbing to a new place is slower than stepping forward. The temporal filter has no motion compensation. The network's depth is relative, so Near and Far Distance only give the Z encoding its 1/z shape; they are not measurements.
