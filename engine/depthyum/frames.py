"""Lettura dei frame di un intervallo di un video tramite ffmpeg (stream PPM su pipe).

Il PPM ha l'header con le dimensioni reali di ogni frame, quindi la rotazione
applicata da ffmpeg (video verticali da telefono) e il ridimensionamento non
richiedono un probe separato.
"""
import os
import subprocess
import tempfile

import numpy as np


def ffmpeg_exe():
    override = os.environ.get("DEPTHYUM_FFMPEG")
    if override:
        return override
    import imageio_ffmpeg

    return imageio_ffmpeg.get_ffmpeg_exe()


def _token(stream):
    tok = b""
    while True:
        c = stream.read(1)
        if not c:
            return tok or None
        if c.isspace():
            if tok:
                return tok
            continue
        tok += c


def _read_ppm(stream):
    magic = _token(stream)
    if magic is None:
        return None
    if magic != b"P6":
        raise RuntimeError("Stream ffmpeg non valido (atteso PPM)")
    w = int(_token(stream))
    h = int(_token(stream))
    int(_token(stream))  # maxval
    n = w * h * 3
    buf = stream.read(n)
    if len(buf) < n:
        return None
    return np.frombuffer(buf, np.uint8).reshape(h, w, 3)


def build_command(path, start, duration, fps, max_dim):
    vf = []
    if max_dim and max_dim > 0:
        vf.append(
            f"scale=w='min(iw,{max_dim})':h='min(ih,{max_dim})'"
            ":force_original_aspect_ratio=decrease:force_divisible_by=2"
        )
    vf.append(f"fps={fps:.9f}")
    return [
        ffmpeg_exe(), "-v", "error", "-nostdin",
        "-ss", f"{max(start, 0.0):.6f}", "-i", path,
        "-t", f"{duration:.6f}", "-an", "-sn",
        "-vf", ",".join(vf),
        "-f", "image2pipe", "-vcodec", "ppm", "-",
    ]


def read_frames(path, start, duration, fps, max_dim=1920):
    """Generatore di frame RGB uint8 (H, W, 3), a frame rate costante `fps`."""
    cmd = build_command(path, start, duration, fps, max_dim)
    err = tempfile.TemporaryFile()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err)
    count = 0
    try:
        while True:
            frame = _read_ppm(proc.stdout)
            if frame is None:
                break
            count += 1
            yield frame
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()
        if proc.stdout:
            proc.stdout.close()
        if count == 0:
            err.seek(0)
            msg = err.read().decode("utf-8", "replace").strip()
            err.close()
            raise RuntimeError("ffmpeg non ha prodotto frame. " + msg[-400:])
        err.close()
