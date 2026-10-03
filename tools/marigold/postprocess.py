"""Stabilisation of per-frame depth predictions. Only numpy, no model.

Marigold V2 predicts affine-invariant depth: every frame comes with its own unknown scale and
shift, so the raw sequence pumps even when the scene does not move. The steps here undo that.
"""
import numpy as np


def detect_cuts(thumbs, threshold=0.12):
    """thumbs: (N, h, w) floats 0..1. Returns bool (N,), True at the first frame of every shot."""
    n = len(thumbs)
    cuts = np.zeros(n, dtype=bool)
    if n:
        cuts[0] = True
    for i in range(1, n):
        if float(np.abs(thumbs[i] - thumbs[i - 1]).mean()) > threshold:
            cuts[i] = True
    return cuts


def shots(cuts):
    idx = list(np.flatnonzero(cuts)) + [len(cuts)]
    return [(int(idx[i]), int(idx[i + 1])) for i in range(len(idx) - 1)]


def gaussian_smooth(x, sigma):
    x = np.asarray(x, dtype=np.float64)
    if sigma <= 0 or len(x) < 2:
        return x.copy()
    radius = max(1, int(np.ceil(sigma * 3)))
    k = np.exp(-0.5 * (np.arange(-radius, radius + 1) / sigma) ** 2)
    k /= k.sum()
    padded = np.pad(x, radius, mode="reflect" if len(x) > radius else "edge")
    return np.convolve(padded, k, mode="valid")


def robust_affine(src, dst, step=8, iters=6):
    """Scale and shift (a, b) such that a * src + b matches dst, ignoring the pixels that moved.

    Starts from a median-based fit, then iteratively reweighted least squares on a subsample:
    pixels whose residual is large (an object that entered or moved between the two frames) get a
    weight close to zero.
    """
    s = np.asarray(src, dtype=np.float64)[::step, ::step].ravel()
    d = np.asarray(dst, dtype=np.float64)[::step, ::step].ravel()
    ok = np.isfinite(s) & np.isfinite(d)
    s, d = s[ok], d[ok]
    if len(s) < 16 or np.ptp(s) < 1e-9:
        return 1.0, 0.0
    ms, md = np.median(s), np.median(d)
    mad_s = np.median(np.abs(s - ms)) + 1e-12
    mad_d = np.median(np.abs(d - md))
    a = mad_d / mad_s if mad_d > 0 else 1.0
    b = md - a * ms
    floor = 0.01 * (np.std(d) + 1e-9)
    for _ in range(iters):
        r = d - (a * s + b)
        sigma = max(1.4826 * np.median(np.abs(r - np.median(r))), floor)
        w = np.exp(-((r / (2.5 * sigma)) ** 2))
        sw = w.sum()
        if sw < 1e-9:
            break
        ms_w, md_w = (w * s).sum() / sw, (w * d).sum() / sw
        var = (w * (s - ms_w) ** 2).sum()
        if var < 1e-12:
            break
        a = (w * (s - ms_w) * (d - md_w)).sum() / var
        b = md_w - a * ms_w
    return float(a), float(b)


def blend(prev, cur, strength, tau=0.08):
    """Motion-adaptive exponential average: smooths small changes, follows large ones."""
    if prev is None or strength <= 0:
        return cur
    w = strength * np.exp(-(((cur - prev) / tau) ** 2))
    return cur * (1.0 - w) + prev * w


def _percentiles(arr, clip):
    sub = np.asarray(arr)[::2, ::2]
    lo, hi = np.percentile(sub, [clip, 100.0 - clip])
    return float(lo), float(hi)


def process_depth(loader, count, thumbs, fps, near_large=False, mode="shot", align=True,
                  smooth=0.5, clip=1.0, cut_threshold=0.12, progress=None):
    """Yields (index, map) with map float32 in 0..1, 1 = near, for each of `count` frames.

    loader(i) returns the raw prediction of frame i as a 2D array. Frames are loaded twice (one
    pass to measure, one to write), never all at once.
    The alignment of a frame is made against the previous frame already aligned, so a shot shares the
    scale of its first frame; on very long shots errors can build up and "smooth" is the alternative.
    mode: "shot"   one fixed range per shot, after aligning each frame to the previous one;
          "smooth" per-frame range averaged over about half a second;
          "frame"  each frame normalised on its own (no alignment).
    """
    cuts = detect_cuts(thumbs, cut_threshold)
    sgn = 1.0 if near_large else -1.0
    # Pass 1: alignment parameters and ranges.
    ab = np.tile([1.0, 0.0], (count, 1))
    lo = np.zeros(count)
    hi = np.zeros(count)
    prev_aligned = None
    for i in range(count):
        cur = sgn * np.asarray(loader(i), dtype=np.float32)
        if mode != "frame" and align:
            if cuts[i] or prev_aligned is None:
                prev_aligned = cur
            else:
                a, b = robust_affine(cur, prev_aligned)
                ab[i] = (a, b)
                prev_aligned = a * cur + b
            lo[i], hi[i] = _percentiles(prev_aligned, clip)
        else:
            lo[i], hi[i] = _percentiles(cur, clip)
        if progress:
            progress("measure", i + 1, count)
    # Ranges per frame.
    out_lo, out_hi = lo.copy(), hi.copy()
    for a, b in shots(cuts):
        if mode == "shot":
            out_lo[a:b] = np.percentile(lo[a:b], 10)
            out_hi[a:b] = np.percentile(hi[a:b], 90)
        elif mode == "smooth":
            sigma = 0.5 * fps
            out_lo[a:b] = gaussian_smooth(lo[a:b], sigma)
            out_hi[a:b] = gaussian_smooth(hi[a:b], sigma)
    # Pass 2: normalise and blend.
    prev = None
    for i in range(count):
        cur = sgn * np.asarray(loader(i), dtype=np.float32)
        if mode != "frame" and align:
            cur = ab[i][0] * cur + ab[i][1]
        span = max(out_hi[i] - out_lo[i], 1e-6)
        n = np.clip((cur - out_lo[i]) / span, 0.0, 1.0).astype(np.float32)
        if cuts[i]:
            prev = None
        n = blend(prev, n, smooth)
        prev = n
        yield i, n
