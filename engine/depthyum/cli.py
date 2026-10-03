import argparse
import json
import signal
import sys

from . import __version__
from .events import emit, log


def _check(args):
    info = {"version": __version__}
    try:
        import torch

        from .model import REPOS, pick_device

        info["torch"] = torch.__version__
        info["device"] = pick_device("auto")
        if info["device"] == "cuda":
            info["gpu"] = torch.cuda.get_device_name(0)
        cached = {}
        try:
            from huggingface_hub import try_to_load_from_cache

            for key, repo in REPOS.items():
                cached[key] = isinstance(try_to_load_from_cache(repo, "config.json"), str)
        except Exception:
            pass
        info["cached"] = cached
    except Exception as e:  # torch assente o rotto
        info["error"] = f"{type(e).__name__}: {e}"
    try:
        from .frames import ffmpeg_exe

        info["ffmpeg"] = ffmpeg_exe()
    except Exception as e:
        info["error"] = (info.get("error", "") + f" ffmpeg: {e}").strip()
    print(json.dumps(info), flush=True)
    return 0 if "error" not in info else 1


def _run(args):
    from .pipeline import Job, run

    def on_term(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, on_term)
    job = Job(
        input=args.input, out=args.out, start=args.start, duration=args.duration, fps=args.fps,
        model=args.model, size=args.size, max_dim=args.max_dim, range_mode=args.range,
        smooth=args.smooth, invert=args.invert, gamma=args.gamma, clip=args.clip,
        cut_threshold=args.cut_threshold, device=args.device, batch=args.batch,
    )
    try:
        run(job)
    except KeyboardInterrupt:
        emit("cancelled")
        return 130
    except Exception as e:
        log(repr(e))
        emit("error", message=str(e))
        return 1
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="depthyum")
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("check")
    c.set_defaults(func=_check)

    r = sub.add_parser("run")
    r.add_argument("--input", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--start", type=float, default=0.0, help="secondi sorgente da cui iniziare")
    r.add_argument("--duration", type=float, required=True, help="secondi sorgente da elaborare")
    r.add_argument("--fps", type=float, required=True, help="frame rate del footage")
    r.add_argument("--model", default="small", help="small | base | large | cartella locale")
    r.add_argument("--size", type=int, default=518, help="lato corto in ingresso al modello")
    r.add_argument("--max-dim", type=int, default=1920, help="lato lungo massimo dell'output, 0 = nessun limite")
    r.add_argument("--range", choices=["frame", "smooth", "shot"], default="smooth")
    r.add_argument("--smooth", type=float, default=0.5, help="0..0.95, fluidità temporale")
    r.add_argument("--invert", action="store_true")
    r.add_argument("--gamma", type=float, default=1.0)
    r.add_argument("--clip", type=float, default=1.0, help="percentile tagliato a ciascun estremo")
    r.add_argument("--cut-threshold", type=float, default=0.12)
    r.add_argument("--device", default="auto")
    r.add_argument("--batch", type=int, default=0)
    r.set_defaults(func=_run)

    args = p.parse_args(argv)
    return args.func(args)
