# Depthyum

Depthyum is an After Effects effect that turns any layer into a depth map, frame by frame, with Depth Anything V2 running locally on the GPU (DirectML). The result is a Z-depth that follows the footage and stays stable from frame to frame, ready to drive a lens blur, fog, relief or any effect that takes a depth layer.

It is built the same way as Lensyum: a C++17 engine with no dependencies (`core/`), an After Effects SmartFX wrapper (`ae/`, 8, 16 and 32 bpc, Multi-Frame Rendering) and a command-line harness (`tools/`). `ARCHITECTURE.md` describes the internals.

## Why it is stable

A depth network sees one frame at a time, and its output has its own scale and offset on every frame, so a map normalised frame by frame pumps and flickers. Depthyum renders each frame from a window of its neighbours: it reads the layer at 2 × Stability + 1 times, runs the network on each (the results are cached by pixel content, so every frame is estimated once), averages the normalisation range over the window and averages the maps per pixel. A neighbour counts less the further it is in time and the more its depth differs from the current frame's at that pixel, so moving edges are not smeared and only flicker is averaged away. Frames never mix across a cut. The result of a frame depends only on the footage, not on the order frames are rendered in, so previews, renders and Multi-Frame Rendering agree.

## Controls

Output: Depth Map (grey), Colormap (blue far, red near), Overlay (picture and colormap half and half), Source (to compare), Depth Scan or Scan Color (see below).

Depth Source: AI Depth (built-in) estimates the depth with Depth Anything V2 inside the plug-in. Depth Layer reads a depth picture you already have, for example a sequence baked with Marigold V2 (see below), from the layer set in Depth Layer. Layer Polarity says whether white is near or far. Layer Range is Use As Is, which keeps the values of the picture (right for the baked sequences, already normalised), or Stabilize Range, which renormalises them like the built-in depth. Stability, Motion Tolerance and Detect Cuts apply to the depth layer's neighbouring frames, and Edge Refine, the Depth controls, Scan and the outputs work the same way. With a depth layer the plug-in needs neither the runtime nor the model.

Depth: Encoding is Disparity (white is near, the usual format of AI depth maps) or Z Distance (white is far, with Near and Far Distance in metres giving the 1/z spacing of a real camera). Invert flips the map. Far Cut and Near Cut clip the ends, Contrast and Shift redistribute it, Smooth blurs it in pixels.

Scan: the depth becomes a wave whose phase moves with time, so a band of the picture lights up and travels from near to far, wraps around and starts again. Depth Scan outputs the wave in grey, to use as a matte or a map. Scan Color tints the picture's own luminance with it: Dark Color where the wave is low, Lit Color where it is high, then Gain and a Glow of the chosen radius. Speed is in cycles per second (negative runs from far to near) and Phase is an angle you can keyframe or drive with an expression; the scan is a function of the layer time, so with a Speed that is a whole number of cycles over the layer's length it loops exactly. Frequency is how many waves fit across the depth range, Shape is Sine, Triangle or Sawtooth, and Sharpness narrows the lit band. The scan works on a still image too.

Temporal: Stability is the number of frames on each side that take part (0 turns it off; more is steadier and slower the first time a frame is rendered). Motion Tolerance sets how different a neighbour's depth may be before it stops counting: low keeps moving edges sharp, high smooths harder. Detect Cuts stops the window at a change of shot.

AI Depth: Detail sets the analysis resolution (Low 392, Medium 518, High 770, Ultra 1022 pixels on the long side). Edge Refine snaps the map to the edges of the image. Use GPU falls back to the CPU when switched off. Input Color says how the layer is encoded: Auto treats 8 and 16 bpc as sRGB and 32 bpc as linear.

Keep Source Alpha leaves the alpha of the layer on the result; otherwise the layer area is opaque.

The depth is written as data, without a colour curve, at every bit depth. Work in 16 bpc or 32 bpc to avoid banding when the map drives a blur.

## Marigold V2 for the best depth

[Marigold V2](https://github.com/huawei-bayerlab/marigold-v2) (Apache 2.0) predicts sharper depth than Depth Anything V2 Small, with hair-thin detail, but it is a Diffusion Transformer on top of Qwen-Image-Edit-2509: it needs Linux, Python 3.10 and a CUDA GPU with about 17 GB of memory at 1024 × 1024 (29 GB at 2048 × 2048), and the first load quantises the model for a few minutes. It cannot run inside the plug-in, so it is used as a bake: `tools/marigold/bake.py` runs it on the frames of a clip, removes the frame-to-frame scatter and writes a 16-bit PNG sequence that the Depth Layer source reads.

Marigold's depth is affine-invariant: each frame has its own unknown scale and shift, so a raw sequence pumps. The bake estimates the scale and shift that map every frame onto the previous one, ignoring the pixels that moved or appeared (an iteratively reweighted fit), then uses one fixed range for the whole shot, so a new object entering the frame does not rescale the background. Shots are cut where the picture changes. A light motion-adaptive average takes out what is left. `--range smooth` and `--range frame` are the alternatives; `--no-align` turns the alignment off.

On Windows run it in WSL2 with CUDA. In the WSL shell:

```
git clone https://github.com/huawei-bayerlab/marigold-v2
cd marigold-v2 && bash setup/setup_env.sh && conda activate marigold-v2
python scripts/download_assets.py --skip-datasets
pip install -r /mnt/c/SDK/Depthyum/tools/marigold/requirements.txt

python /mnt/c/SDK/Depthyum/tools/marigold/bake.py --video /mnt/c/clips/shot.mp4 --out /mnt/c/clips/shot_depth \
    --marigold-dir ~/marigold-v2 --fps 24
```

`--fps` is the frame rate of the footage in After Effects; `--start` and `--duration` bake a part of the clip. In After Effects import `shot_depth/depth_00000.png` as an image sequence at the same frame rate, put it in the comp with the same in point as the clip, then on the Depthyum layer set Depth Source to Depth Layer and pick it as Depth Layer. White is near, and Layer Range stays on Use As Is. Hide the depth layer's eye: it still works as a source.

Time and memory are not measured here; a single step of a 20-billion-parameter transformer per frame is slow compared with the built-in estimate, so it is meant for the final render of a shot. `--modality normals` and `--modality albedo` write the other Marigold V2 outputs as 16-bit sequences (`normals_00000.png`, with the vectors stored as n × 0.5 + 0.5, and `albedo_00000.png`); the plug-in does not use them yet, but After Effects effects that take a normal or an albedo map do.

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

The Marigold bake has its own tests (`pip install -r tools/marigold/requirements.txt pytest`, then `pytest tests`), which use a stand-in for Marigold's `infer.py` that writes predictions with a random scale and shift per frame. The engine tests replace the network with a function and check the cache, the cut detection and the temporal fusion on any platform. To try the model:

```
build/depthyum_cli depth photo.ppm depth.pgm aimodel=depthyum_depth.onnx ort=depthyum_ort.dll
build/depthyum_cli seq out_ f000.ppm f001.ppm f002.ppm aimodel=depthyum_depth.onnx ort=depthyum_ort.dll radius=3
```

`seq` writes `out_000.pgm`, `out_001.pgm`, ... each fused with its neighbours like the plug-in does. Options: `aires` (network size), `refine`, `gpu`, `radius`, `tol`.

## Status

The bake tool is tested against a stand-in for `scripts/infer.py` that follows the documented output layout (`images/predictions_npy/<name>.npy`); it has not been run with the real Marigold V2 model, which needs a CUDA GPU.

Windows only. The engine, the cache and the temporal fusion are tested on Linux, including the ONNX Runtime path against a small Depth Anything-shaped model. The After Effects wrapper has not been built against the SDK or run inside After Effects yet. The temporal filter does not compensate camera motion, so on fast pans the neighbours that move are down-weighted rather than aligned.

License: see LICENSE (all rights reserved). Third-party components keep the licenses listed in THIRD_PARTY.md.
