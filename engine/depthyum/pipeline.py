"""Video -> sequenza PNG a 16 bit di depth map, allineata frame per frame."""
import os
import tempfile
from dataclasses import dataclass

import cv2
import numpy as np

from . import frames as frames_mod
from . import temporal
from .events import emit, log


@dataclass
class Job:
    input: str
    out: str
    start: float
    duration: float
    fps: float
    model: str = "small"
    size: int = 518
    max_dim: int = 1920
    range_mode: str = "smooth"
    smooth: float = 0.5
    invert: bool = False
    gamma: float = 1.0
    clip: float = 1.0
    cut_threshold: float = 0.12
    device: str = "auto"
    batch: int = 0


def _thumb(frame):
    g = cv2.cvtColor(frame, cv2.COLOR_RGB2GRAY)
    return cv2.resize(g, (32, 18), interpolation=cv2.INTER_AREA).astype(np.float32) / 255.0


def run(job, model=None):
    from . import model as model_mod

    os.makedirs(job.out, exist_ok=True)
    total_est = max(1, int(round(job.duration * job.fps)))

    emit("status", message="Caricamento modello")
    net = model or model_mod.load(job.model, device=job.device, batch=job.batch)
    emit("status", message=f"Modello pronto ({net.device})")

    raw_path = os.path.join(job.out, ".raw_depth.npy")
    raw = None
    lo_p, hi_p, thumbs = [], [], []
    size_wh = None
    done = 0

    try:
        pending = []

        def flush():
            nonlocal raw, done
            if not pending:
                return
            depth = net.predict(pending, job.size)
            if raw is None:
                raw = np.lib.format.open_memmap(
                    raw_path, mode="w+", dtype=np.float16,
                    shape=(total_est + 2,) + depth.shape[1:],
                )
            for d in depth:
                if done >= raw.shape[0]:
                    break
                raw[done] = d
                lo_p.append(float(np.percentile(d, job.clip)))
                hi_p.append(float(np.percentile(d, 100.0 - job.clip)))
                done += 1
            pending.clear()
            emit("progress", stage="depth", done=done, total=total_est)

        for frame in frames_mod.read_frames(job.input, job.start, job.duration, job.fps, job.max_dim):
            if size_wh is None:
                size_wh = (frame.shape[1], frame.shape[0])
            elif (frame.shape[1], frame.shape[0]) != size_wh:
                raise RuntimeError("Dimensione dei frame variabile nel video")
            thumbs.append(_thumb(frame))
            pending.append(frame)
            if len(pending) >= net.batch:
                flush()
            if len(thumbs) >= total_est + 2:
                break
        flush()

        if done == 0:
            raise RuntimeError("Nessun frame elaborato")

        cuts = temporal.detect_cuts(np.stack(thumbs[:done]), job.cut_threshold)
        sigma = 0.5 * job.fps
        lo, hi = temporal.stabilize_range(lo_p, hi_p, cuts, job.range_mode, sigma)

        W, H = size_wh
        prev = None
        for i in range(done):
            cur = temporal.normalize(raw[i].astype(np.float32), lo[i], hi[i])
            if cuts[i]:
                prev = None
            cur = temporal.blend(prev, cur, job.smooth)
            prev = cur

            out = cv2.resize(cur, (W, H), interpolation=cv2.INTER_CUBIC)
            out = np.clip(out, 0.0, 1.0)
            if job.gamma != 1.0:
                out = out ** job.gamma
            if job.invert:
                out = 1.0 - out
            img = np.rint(out * 65535.0).astype(np.uint16)
            name = os.path.join(job.out, f"depth_{i:05d}.png")
            if not cv2.imwrite(name, img, [cv2.IMWRITE_PNG_COMPRESSION, 1]):
                raise RuntimeError(f"Scrittura fallita: {name}")
            if i % 4 == 0 or i == done - 1:
                emit("progress", stage="write", done=i + 1, total=done)
    finally:
        if raw is not None:
            del raw
        try:
            os.remove(raw_path)
        except OSError:
            pass

    emit(
        "done", dir=job.out, first="depth_00000.png", count=done,
        fps=job.fps, width=size_wh[0], height=size_wh[1],
        cuts=int(cuts.sum()) - 1,
    )
    log(f"{done} frame, {int(cuts.sum()) - 1} stacchi rilevati")
    return done
