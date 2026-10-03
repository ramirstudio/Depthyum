import os
import subprocess
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

FPS = 24
W, H = 160, 96


def square_x(i):
    return 10 + 2 * (i % 24)


def make_frame(i):
    f = np.full((H, W, 3), 30 if i < 24 else 150, np.uint8)
    x = square_x(i)
    f[30:60, x:x + 30] = 255 if i < 24 else 20
    return f


@pytest.fixture(scope="session")
def video(tmp_path_factory):
    import imageio_ffmpeg

    path = str(tmp_path_factory.mktemp("vid") / "clip.mp4")
    cmd = [
        imageio_ffmpeg.get_ffmpeg_exe(), "-v", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
        "-c:v", "mpeg4", "-q:v", "2", "-pix_fmt", "yuv420p", path,
    ]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for i in range(48):
        p.stdin.write(make_frame(i).tobytes())
    p.stdin.close()
    assert p.wait() == 0
    return path
