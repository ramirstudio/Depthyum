import glob
import json
import os

import cv2
import numpy as np

from depthyum.pipeline import Job, run

from conftest import FPS, H, W, square_x


def _job(video, out, **kw):
    base = dict(input=video, out=str(out), start=0.0, duration=2.0, fps=FPS, model="fake",
                size=140, max_dim=0, smooth=0.0)
    base.update(kw)
    return Job(**base)


def _pngs(out):
    return sorted(glob.glob(os.path.join(str(out), "depth_*.png")))


def test_full_clip_frame_count_and_format(video, tmp_path, capsys):
    n = run(_job(video, tmp_path))
    files = _pngs(tmp_path)
    assert n == len(files) == 48
    img = cv2.imread(files[0], cv2.IMREAD_UNCHANGED)
    assert img.dtype == np.uint16 and img.shape == (H, W)
    assert not os.path.exists(os.path.join(str(tmp_path), ".raw_depth.npy"))
    last = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert last["event"] == "done" and last["count"] == 48 and last["cuts"] == 1


def test_trim_by_start_and_duration(video, tmp_path):
    n = run(_job(video, tmp_path, start=1.0, duration=0.5))
    assert n == 12


def test_depth_tracks_source_frame_by_frame(video, tmp_path):
    run(_job(video, tmp_path, range_mode="frame"))
    files = _pngs(tmp_path)

    def centroid_x(img):
        a = img.astype(np.float64)
        a = np.clip(a - np.median(a, axis=1, keepdims=True), 0, None)
        return (a.sum(0) * np.arange(img.shape[1])).sum() / a.sum()

    for i in (0, 11, 22):
        d = cv2.imread(files[i], cv2.IMREAD_UNCHANGED)
        assert abs(centroid_x(d) - (square_x(i) + 15)) < 6, i


def test_invert_flips_values(video, tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    run(_job(video, a, duration=0.25, range_mode="shot"))
    run(_job(video, b, duration=0.25, range_mode="shot", invert=True))
    ia = cv2.imread(_pngs(a)[0], cv2.IMREAD_UNCHANGED).astype(np.int32)
    ib = cv2.imread(_pngs(b)[0], cv2.IMREAD_UNCHANGED).astype(np.int32)
    assert np.abs((65535 - ia) - ib).max() <= 1


def test_max_dim_caps_output(video, tmp_path):
    run(_job(video, tmp_path, max_dim=80, duration=0.25))
    img = cv2.imread(_pngs(tmp_path)[0], cv2.IMREAD_UNCHANGED)
    assert max(img.shape) == 80 and img.shape == (48, 80)
