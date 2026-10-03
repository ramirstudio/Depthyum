# Depthyum

Depthyum is an After Effects effect that turns any layer into a depth map, frame by frame, with Depth Anything V2 running locally on the GPU (DirectML). The result is a Z-depth that follows the footage and stays stable from frame to frame, ready to drive a lens blur, fog, relief or any effect that takes a depth layer.

It is built the same way as Lensyum: a C++17 engine with no dependencies (`core/`), an After Effects SmartFX wrapper (`ae/`, 8, 16 and 32 bpc, Multi-Frame Rendering) and a command-line harness (`tools/`). `ARCHITECTURE.md` describes the internals.

## Why it is stable

A depth network sees one frame at a time, and its output has its own scale and offset on every frame, so a map normalised frame by frame pumps and flickers. Depthyum renders each frame from a window of its neighbours: it reads the layer at 2 × Stability + 1 times, runs the network on each (the results are cached by pixel content, so every frame is estimated once), averages the normalisation range over the window and averages the maps per pixel. A neighbour counts less the further it is in time and the more its depth differs from the current frame's at that pixel, so moving edges are not smeared and only flicker is averaged away. Frames never mix across a cut. The result of a frame depends only on the footage, not on the order frames are rendered in, so previews, renders and Multi-Frame Rendering agree.

## Controls

Output: Depth Map (grey), Colormap (blue far, red near), Overlay (picture and colormap half and half), Source (to compare), Depth Scan or Scan Color (see below).

Depth: Encoding is Disparity (white is near, the usual format of AI depth maps) or Z Distance (white is far, with Near and Far Distance in metres giving the 1/z spacing of a real camera). Invert flips the map. Far Cut and Near Cut clip the ends, Contrast and Shift redistribute it, Smooth blurs it in pixels.

Scan: the depth becomes a wave whose phase moves with time, so a band of the picture lights up and travels from near to far, wraps around and starts again. Depth Scan outputs the wave in grey, to use as a matte or a map. Scan Color tints the picture's own luminance with it: Dark Color where the wave is low, Lit Color where it is high, then Gain and a Glow of the chosen radius. Speed is in cycles per second (negative runs from far to near) and Phase is an angle you can keyframe or drive with an expression; the scan is a function of the layer time, so with a Speed that is a whole number of cycles over the layer's length it loops exactly. Frequency is how many waves fit across the depth range, Shape is Sine, Triangle or Sawtooth, and Sharpness narrows the lit band. The scan works on a still image too.

Temporal: Stability is the number of frames on each side that take part (0 turns it off; more is steadier and slower the first time a frame is rendered). Motion Tolerance sets how different a neighbour's depth may be before it stops counting: low keeps moving edges sharp, high smooths harder. Detect Cuts stops the window at a change of shot.

AI Depth: Detail sets the analysis resolution (Low 392, Medium 518, High 770, Ultra 1022 pixels on the long side). Edge Refine snaps the map to the edges of the image. Use GPU falls back to the CPU when switched off. Input Color says how the layer is encoded: Auto treats 8 and 16 bpc as sRGB and 32 bpc as linear.

Keep Source Alpha leaves the alpha of the layer on the result; otherwise the layer area is opaque.

The depth is written as data, without a colour curve, at every bit depth. Work in 16 bpc or 32 bpc to avoid banding when the map drives a blur.

## Install

The `Depthyum-1.0-win64.zip` package contains the plug-in, ONNX Runtime, DirectML and the depth model. Extract it into `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\Depthyum\` with After Effects closed; the same steps are in `INSTALL.txt` inside the zip. Licenses of the bundled components are in `THIRD_PARTY.md`.

## Build on Windows

The setup is the one Lensyum uses: Visual Studio 2022 or later with the "Desktop development with C++" workload, CMake 3.20 or later, the After Effects SDK in `C:\SDK\AfterEffectsSDK`, the NuGet package `Microsoft.ML.OnnxRuntime.DirectML` extracted to `C:\SDK\ort` and `Microsoft.AI.DirectML` extracted to `C:\SDK\dml`.

From "x64 Native Tools Command Prompt for VS", in the project folder:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAE_SDK_DIR="C:/SDK/AfterEffectsSDK"
cmake --build build --target Depthyum
package.bat
```

The result is `build\ae\Depthyum.aex`, and `package.bat` creates `dist\Depthyum-1.0-win64.zip`. `package.bat` takes the depth model from `MODEL`, or reuses the `lensyum_depth.onnx` Lensyum already has in `C:\SDK` or in its install folder.

If the runtime or the model is missing or fails to load, the frame turns solid red and the reason is written to `%TEMP%\depthyum_log.txt`.

## Test the engine without After Effects

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

The tests replace the network with a function and check the cache, the cut detection and the temporal fusion on any platform. To try the model:

```
build/depthyum_cli depth photo.ppm depth.pgm aimodel=depthyum_depth.onnx ort=depthyum_ort.dll
build/depthyum_cli seq out_ f000.ppm f001.ppm f002.ppm aimodel=depthyum_depth.onnx ort=depthyum_ort.dll radius=3
```

`seq` writes `out_000.pgm`, `out_001.pgm`, ... each fused with its neighbours like the plug-in does. Options: `aires` (network size), `refine`, `gpu`, `radius`, `tol`.

## Status

Windows only. The engine, the cache and the temporal fusion are tested on Linux, including the ONNX Runtime path against a small Depth Anything-shaped model. The After Effects wrapper has not been built against the SDK or run inside After Effects yet. The temporal filter does not compensate camera motion, so on fast pans the neighbours that move are down-weighted rather than aligned.

License: see LICENSE (all rights reserved). Third-party components keep the licenses listed in THIRD_PARTY.md.
