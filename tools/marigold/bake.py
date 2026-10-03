#!/usr/bin/env python3
"""Bakes a depth sequence for Depthyum with Marigold V2.

Marigold V2 (https://github.com/huawei-bayerlab/marigold-v2) is a Diffusion Transformer that needs
Linux, Python 3.10 and a CUDA GPU with about 17 GB at 1024 x 1024, so it cannot run inside the
After Effects plug-in. This script runs it on the frames of a video, removes the frame-to-frame
scatter of its affine-invariant output and writes a 16-bit grey PNG sequence (white = near) that
Depthyum reads through Depth Source > Depth Layer.

On Windows run it inside WSL2 with CUDA; the output folder can be a /mnt/c/... path.

  python bake.py --video clip.mp4  # --video also takes a single photograph: it bakes one frame --out depth_clip --marigold-dir ~/marigold-v2 \
                 --marigold-python ~/miniconda3/envs/marigold-v2/bin/python --fps 24
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import postprocess  # noqa: E402


def say(event, **fields):
    fields["event"] = event
    print(json.dumps(fields, ensure_ascii=False), flush=True)


def ffmpeg_exe():
    override = os.environ.get("DEPTHYUM_FFMPEG")
    if override:
        return override
    import imageio_ffmpeg

    return imageio_ffmpeg.get_ffmpeg_exe()


def extract_frames(video, dest, start, duration, fps, max_dim):
    vf = []
    if max_dim > 0:
        vf.append(
            f"scale=w='min(iw,{max_dim})':h='min(ih,{max_dim})'"
            ":force_original_aspect_ratio=decrease:force_divisible_by=16"
        )
    vf.append(f"fps={fps:.9f}")
    cmd = [ffmpeg_exe(), "-v", "error", "-nostdin", "-y", "-ss", f"{max(start, 0.0):.6f}", "-i", video]
    if duration and duration > 0:
        cmd += ["-t", f"{duration:.6f}"]
    cmd += ["-an", "-sn", "-vf", ",".join(vf), os.path.join(dest, "frame_%05d.png")]
    subprocess.run(cmd, check=True)
    return sorted(glob.glob(os.path.join(dest, "frame_*.png")))


def thumbnails(frames):
    out = []
    for f in frames:
        g = cv2.imread(f, cv2.IMREAD_GRAYSCALE)
        out.append(cv2.resize(g, (32, 18), interpolation=cv2.INTER_AREA).astype(np.float32) / 255.0)
    return np.stack(out)


def run_marigold(args, frames_dir, work_dir):
    script = os.path.join(args.marigold_dir, "scripts", "infer.py")
    if not os.path.isfile(script):
        raise FileNotFoundError(f"scripts/infer.py not found in {args.marigold_dir}")
    cmd = [args.marigold_python, script, "--modality", args.modality,
           "--image_dir", frames_dir, "--output_dir", work_dir, "--seed", str(args.seed)]
    if args.checkpoint:
        cmd += ["--checkpoint", args.checkpoint]
    if args.infer_size:
        cmd += ["--width", str(args.infer_size[0]), "--height", str(args.infer_size[1])]
    say("status", message="Marigold V2: loading the model (the first run quantises it, a few minutes)")
    subprocess.run(cmd, check=True, cwd=args.marigold_dir)


def find_predictions(work_dir, frames):
    found = {}
    for path in glob.glob(os.path.join(work_dir, "**", "predictions_npy", "**", "*.npy"), recursive=True):
        found[os.path.splitext(os.path.basename(path))[0]] = path
    paths = []
    for f in frames:
        stem = os.path.splitext(os.path.basename(f))[0]
        if stem not in found:
            raise FileNotFoundError(f"no prediction for {stem} under {work_dir}")
        paths.append(found[stem])
    return paths


def write_png16(path, img):
    if not cv2.imwrite(path, img, [cv2.IMWRITE_PNG_COMPRESSION, 1]):
        raise RuntimeError(f"cannot write {path}")


def size_wh(s):
    w, h = s.lower().split("x")
    return int(w), int(h)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--video", required=True)
    p.add_argument("--out", required=True, help="folder for depth_00000.png, depth_00001.png, ...")
    p.add_argument("--marigold-dir", required=True, help="clone of huawei-bayerlab/marigold-v2 with assets downloaded")
    p.add_argument("--marigold-python", default=sys.executable, help="python of the marigold-v2 environment")
    p.add_argument("--modality", choices=["depth", "normals", "albedo"], default="depth")
    p.add_argument("--checkpoint", help="checkpoint directory or repo[/subfolder] (default: the modality's default)")
    p.add_argument("--start", type=float, default=0.0, help="seconds of the video to start from")
    p.add_argument("--duration", type=float, default=0.0, help="seconds to process, 0 = to the end")
    p.add_argument("--fps", type=float, default=24.0, help="frame rate of the footage in After Effects (a still photo gives one frame)")
    p.add_argument("--max-dim", type=int, default=1024, help="long side of the frames given to the model, 0 = native")
    p.add_argument("--infer-size", type=size_wh, help="fixed inference size WxH, multiples of 16")
    p.add_argument("--seed", type=int, default=2025)
    p.add_argument("--polarity", choices=["auto", "far-large", "near-large"], default="auto",
                   help="depth grows with distance (Log, Uniform) or with proximity (Disparity)")
    p.add_argument("--range", choices=["shot", "smooth", "frame"], default="shot")
    p.add_argument("--no-align", action="store_true", help="skip the frame-to-frame scale and shift alignment")
    p.add_argument("--smooth", type=float, default=0.5, help="0..0.95 temporal averaging of small changes")
    p.add_argument("--cut-threshold", type=float, default=0.12)
    p.add_argument("--keep-temp", action="store_true")
    args = p.parse_args(argv)

    os.makedirs(args.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="depthyum_marigold_")
    frames_dir = os.path.join(tmp, "frames")
    work_dir = os.path.join(tmp, "marigold")
    os.makedirs(frames_dir)
    try:
        say("status", message="Extracting frames")
        frames = extract_frames(args.video, frames_dir, args.start, args.duration, args.fps, args.max_dim)
        if not frames:
            raise RuntimeError("ffmpeg produced no frames")
        say("status", message=f"{len(frames)} frames")
        run_marigold(args, frames_dir, work_dir)
        preds = find_predictions(work_dir, frames)

        count = len(preds)
        if args.modality == "depth":
            checkpoint = (args.checkpoint or "").lower()
            near_large = args.polarity == "near-large" or (args.polarity == "auto" and "disparity" in checkpoint)
            thumbs = thumbnails(frames)
            gen = postprocess.process_depth(
                lambda i: np.load(preds[i], mmap_mode="r"), count, thumbs, args.fps, near_large=near_large,
                mode=args.range, align=not args.no_align, smooth=min(max(args.smooth, 0.0), 0.95),
                cut_threshold=args.cut_threshold,
                progress=lambda stage, d, n: say("progress", stage=stage, done=d, total=n))
            for i, n in gen:
                write_png16(os.path.join(args.out, f"depth_{i:05d}.png"), np.rint(n * 65535.0).astype(np.uint16))
                if i % 4 == 0 or i == count - 1:
                    say("progress", stage="write", done=i + 1, total=count)
            first = "depth_00000.png"
        else:
            # Normals are unit vectors in [-1, 1] and albedo is linear RGB in [0, 1]; written as they are.
            prefix = args.modality
            for i, path in enumerate(preds):
                arr = np.load(path).astype(np.float32)  # [3, H, W]
                if args.modality == "normals":
                    arr = arr * 0.5 + 0.5
                img = np.clip(arr.transpose(1, 2, 0), 0.0, 1.0)[:, :, ::-1]  # RGB -> BGR for OpenCV
                write_png16(os.path.join(args.out, f"{prefix}_{i:05d}.png"), np.rint(img * 65535.0).astype(np.uint16))
                say("progress", stage="write", done=i + 1, total=count)
            first = f"{prefix}_00000.png"
        say("done", dir=os.path.abspath(args.out), first=first, count=count, fps=args.fps)
    finally:
        if args.keep_temp:
            say("status", message=f"kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
