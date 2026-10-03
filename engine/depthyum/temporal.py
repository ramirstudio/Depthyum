"""Coerenza temporale della depth map. Solo numpy, nessun modello."""
import numpy as np


def detect_cuts(thumbs, threshold=0.12):
    """thumbs: (N, h, w) float 0..1. Ritorna bool (N,), True all'inizio di ogni inquadratura."""
    n = len(thumbs)
    cuts = np.zeros(n, dtype=bool)
    if n:
        cuts[0] = True
    for i in range(1, n):
        if float(np.abs(thumbs[i] - thumbs[i - 1]).mean()) > threshold:
            cuts[i] = True
    return cuts


def shots(cuts):
    """Lista di (inizio, fine esclusa) per ogni inquadratura."""
    idx = list(np.flatnonzero(cuts)) + [len(cuts)]
    return [(idx[i], idx[i + 1]) for i in range(len(idx) - 1)]


def gaussian_smooth(x, sigma):
    if sigma <= 0 or len(x) < 2:
        return np.asarray(x, dtype=np.float64).copy()
    radius = max(1, int(np.ceil(sigma * 3)))
    k = np.exp(-0.5 * (np.arange(-radius, radius + 1) / sigma) ** 2)
    k /= k.sum()
    padded = np.pad(np.asarray(x, dtype=np.float64), radius, mode="reflect" if len(x) > radius else "edge")
    return np.convolve(padded, k, mode="valid")


def stabilize_range(lo, hi, cuts, mode="smooth", sigma=12.0):
    """Estremi di normalizzazione per frame.

    frame:  quelli misurati sul singolo frame (la profondità respira).
    smooth: gaussiana lungo il tempo dentro ogni inquadratura.
    shot:   mediana su tutta l'inquadratura (estremi fissi).
    """
    lo = np.asarray(lo, dtype=np.float64)
    hi = np.asarray(hi, dtype=np.float64)
    if mode == "frame":
        return lo.copy(), hi.copy()
    out_lo = np.empty_like(lo)
    out_hi = np.empty_like(hi)
    for a, b in shots(cuts):
        if mode == "shot":
            out_lo[a:b] = np.median(lo[a:b])
            out_hi[a:b] = np.median(hi[a:b])
        else:
            out_lo[a:b] = gaussian_smooth(lo[a:b], sigma)
            out_hi[a:b] = gaussian_smooth(hi[a:b], sigma)
    return out_lo, out_hi


def normalize(raw, lo, hi):
    return np.clip((raw - lo) / max(hi - lo, 1e-6), 0.0, 1.0).astype(np.float32)


def blend(prev, cur, strength, tau=0.08):
    """Media mobile esponenziale adattiva al movimento.

    Dove il valore cambia poco rispetto al frame precedente (flicker) pesa
    `strength`; dove cambia molto (oggetto in movimento) il peso cade a zero,
    così i bordi non lasciano scie.
    """
    if prev is None or strength <= 0:
        return cur
    w = strength * np.exp(-((cur - prev) / tau) ** 2)
    return cur * (1.0 - w) + prev * w
