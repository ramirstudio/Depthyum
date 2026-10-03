import numpy as np

from depthyum import temporal


def test_cuts_found_at_hard_change():
    thumbs = np.zeros((10, 4, 4), np.float32)
    thumbs[5:] = 0.8
    cuts = temporal.detect_cuts(thumbs, 0.12)
    assert list(np.flatnonzero(cuts)) == [0, 5]
    assert temporal.shots(cuts) == [(0, 5), (5, 10)]


def test_no_cut_on_slow_drift():
    thumbs = np.linspace(0, 0.3, 30, dtype=np.float32)[:, None, None] * np.ones((1, 4, 4), np.float32)
    assert temporal.detect_cuts(thumbs, 0.12).sum() == 1


def test_range_modes():
    rng = np.random.default_rng(0)
    lo = 1.0 + rng.normal(0, 0.2, 40)
    hi = 9.0 + rng.normal(0, 0.5, 40)
    cuts = np.zeros(40, bool)
    cuts[0] = True
    f_lo, _ = temporal.stabilize_range(lo, hi, cuts, "frame")
    s_lo, _ = temporal.stabilize_range(lo, hi, cuts, "smooth", sigma=6)
    h_lo, h_hi = temporal.stabilize_range(lo, hi, cuts, "shot")
    assert np.allclose(f_lo, lo)
    assert np.std(np.diff(s_lo)) < np.std(np.diff(lo))
    assert np.ptp(h_lo) == 0 and np.ptp(h_hi) == 0


def test_range_does_not_cross_cuts():
    lo = np.r_[np.zeros(10), np.ones(10) * 5]
    hi = lo + 10
    cuts = np.zeros(20, bool)
    cuts[[0, 10]] = True
    s_lo, _ = temporal.stabilize_range(lo, hi, cuts, "smooth", sigma=3)
    assert np.allclose(s_lo[:10], 0) and np.allclose(s_lo[10:], 5)


def test_normalize_bounds():
    out = temporal.normalize(np.array([-5.0, 0.0, 5.0, 20.0]), 0.0, 10.0)
    assert out.min() == 0.0 and out.max() == 1.0 and out[2] == 0.5


def test_blend_damps_flicker_but_follows_motion():
    prev = np.full((4, 4), 0.50, np.float32)
    flicker = np.full((4, 4), 0.52, np.float32)
    motion = np.full((4, 4), 0.90, np.float32)
    assert abs(temporal.blend(prev, flicker, 0.8).mean() - 0.50) < abs(0.52 - 0.50)
    assert np.allclose(temporal.blend(prev, motion, 0.8), motion, atol=1e-3)
    assert temporal.blend(None, flicker, 0.8) is flicker
