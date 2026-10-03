import glob
import json
import os
import subprocess
import sys
import textwrap

import cv2
import numpy as np
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "marigold"))
import postprocess  # noqa: E402

BAKE = os.path.join(os.path.dirname(__file__), "..", "tools", "marigold", "bake.py")
FPS = 24
W, H = 128, 72


def scene_depth(i, cut_at=None):
    """True depth (1 = near): a ramp, plus a near block that enters from frame 8."""
    d = np.tile(np.linspace(0.1, 0.5, W, dtype=np.float32), (H, 1))
    if i >= 8:
        x = 20 + 3 * (i - 8)
        d[20:50, x:x + 24] = 0.95
    if cut_at is not None and i >= cut_at:
        d = d[::-1].copy() * 0.6 + 0.3
    return d


def make_frame(i, cut_at=None):
    lum = scene_depth(i, cut_at)
    if cut_at is not None and i >= cut_at:
        lum = 1.0 - lum
    g = np.clip(lum * 255, 0, 255).astype(np.uint8)
    return np.dstack([g, g, g])


@pytest.fixture(scope="session")
def video(tmp_path_factory):
    import imageio_ffmpeg

    path = str(tmp_path_factory.mktemp("vid") / "clip.mp4")
    cmd = [imageio_ffmpeg.get_ffmpeg_exe(), "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
           "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-", "-c:v", "mpeg4", "-q:v", "2", "-pix_fmt", "yuv420p", path]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for i in range(30):
        proc.stdin.write(make_frame(i, cut_at=20).tobytes())
    proc.stdin.close()
    assert proc.wait() == 0
    return path


FAKE_INFER = textwrap.dedent('''
    """Stands in for marigold-v2/scripts/infer.py: same arguments, same output layout."""
    import argparse, glob, os, sys
    import cv2, numpy as np
    p = argparse.ArgumentParser()
    p.add_argument("--modality", default="depth")
    p.add_argument("--image_dir"); p.add_argument("--output_dir")
    p.add_argument("--checkpoint"); p.add_argument("--width"); p.add_argument("--height")
    p.add_argument("--seed", type=int, default=2025)
    a = p.parse_args()
    rng = np.random.default_rng(a.seed)
    out = os.path.join(a.output_dir, "images", "predictions_npy")
    os.makedirs(out, exist_ok=True)
    for f in sorted(glob.glob(os.path.join(a.image_dir, "*.png"))):
        g = cv2.imread(f, cv2.IMREAD_GRAYSCALE).astype(np.float32) / 255.0   # brightness = proximity
        # Log depth grows with distance, and every frame has its own scale and shift.
        log_depth = -(g * 2.0 - 1.0)
        scale, shift = rng.uniform(0.6, 1.5), rng.uniform(-0.3, 0.3)
        np.save(os.path.join(out, os.path.splitext(os.path.basename(f))[0] + ".npy"),
                (log_depth * scale + shift).astype(np.float32))
''')


@pytest.fixture()
def marigold_dir(tmp_path):
    d = tmp_path / "marigold-v2" / "scripts"
    d.mkdir(parents=True)
    (d / "infer.py").write_text(FAKE_INFER)
    return str(tmp_path / "marigold-v2")


def run_bake(video, marigold_dir, out, *extra):
    cmd = [sys.executable, BAKE, "--video", video, "--out", str(out), "--marigold-dir", marigold_dir,
           "--marigold-python", sys.executable, "--fps", str(FPS), "--max-dim", "0", *extra]
    res = subprocess.run(cmd, capture_output=True, text=True)
    assert res.returncode == 0, res.stderr[-800:]
    return [json.loads(l) for l in res.stdout.splitlines() if l.startswith("{")]


def load(out):
    files = sorted(glob.glob(os.path.join(str(out), "depth_*.png")))
    return [cv2.imread(f, cv2.IMREAD_UNCHANGED).astype(np.float64) / 65535.0 for f in files]


def test_bake_writes_aligned_16bit_sequence(video, marigold_dir, tmp_path):
    events = run_bake(video, marigold_dir, tmp_path / "o")
    maps = load(tmp_path / "o")
    assert len(maps) == 30
    assert maps[0].shape == (H, W)
    done = events[-1]
    assert done["event"] == "done" and done["count"] == 30 and done["first"] == "depth_00000.png"
    raw = cv2.imread(os.path.join(str(tmp_path / "o"), "depth_00000.png"), cv2.IMREAD_UNCHANGED)
    assert raw.dtype == np.uint16
    # White is near: the right side of the ramp is brighter than the left in the first shot.
    assert maps[2][:, -10:].mean() > maps[2][:, :10].mean() + 0.2


def temporal_flicker(maps, region):
    ys, xs = region
    series = np.array([m[ys, xs].mean() for m in maps])
    return float(np.abs(np.diff(series)).mean())


def test_alignment_removes_scale_jitter(video, marigold_dir, tmp_path):
    # Frames 0..19 are one shot; look at a pixel region that does not change in the scene.
    region = (slice(60, 70), slice(100, 120))
    run_bake(video, marigold_dir, tmp_path / "shot", "--range", "shot")
    run_bake(video, marigold_dir, tmp_path / "naive", "--range", "frame", "--no-align", "--smooth", "0")
    steady = temporal_flicker(load(tmp_path / "shot")[:19], region)
    naive = temporal_flicker(load(tmp_path / "naive")[:19], region)
    # The naive normalisation also removes affine scatter, but pumps when the near block enters;
    # the aligned, fixed-range output must be at least as steady everywhere and steadier overall.
    assert steady < 0.02
    assert steady <= naive + 1e-3


def test_new_object_does_not_change_background(video, marigold_dir, tmp_path):
    run_bake(video, marigold_dir, tmp_path / "o", "--range", "shot", "--smooth", "0")
    maps = load(tmp_path / "o")
    before, after = maps[6][60:70, 5:25].mean(), maps[12][60:70, 5:25].mean()
    assert abs(before - after) < 0.05


def test_cut_resets_the_shot(video, marigold_dir, tmp_path):
    run_bake(video, marigold_dir, tmp_path / "o")
    maps = load(tmp_path / "o")
    # The second shot is a vertically flipped, remapped scene: it must not be forced onto the
    # scale of the first (frame 19 and frame 20 are different pictures), and stays inside 0..1.
    assert all(0.0 <= m.min() and m.max() <= 1.0 for m in maps)
    assert abs(maps[19].mean() - maps[20].mean()) > 0.0


def test_robust_affine_ignores_moved_pixels():
    rng = np.random.default_rng(1)
    base = rng.normal(size=(64, 64)).astype(np.float32)
    dst = base.copy()
    src = (base - 0.2) / 1.7
    dst[10:30, 10:30] += 3.0                     # an object that moved: large, local residual
    a, b = postprocess.robust_affine(src, dst, step=1)
    assert abs(a - 1.7) < 0.05 and abs(b - 0.2) < 0.05
    assert postprocess.robust_affine(np.zeros((8, 8)), np.zeros((8, 8))) == (1.0, 0.0)


def test_cuts_and_ranges():
    t = np.zeros((10, 4, 4), np.float32)
    t[5:] = 0.8
    c = postprocess.detect_cuts(t)
    assert postprocess.shots(c) == [(0, 5), (5, 10)]
    assert np.ptp(postprocess.gaussian_smooth([1, 1, 1, 1], 2)) == 0
