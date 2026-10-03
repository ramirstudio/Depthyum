"""Depth Anything V2 tramite transformers, più un modello finto per i test."""
import os

import numpy as np

REPOS = {
    "small": "depth-anything/Depth-Anything-V2-Small-hf",
    "base": "depth-anything/Depth-Anything-V2-Base-hf",
    "large": "depth-anything/Depth-Anything-V2-Large-hf",
}

PATCH = 14
IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)


def target_hw(h, w, size, multiple=PATCH):
    """Lato corto portato a `size`, entrambi i lati arrotondati al multiplo di 14."""
    scale = size / float(min(h, w))
    nh = max(multiple, int(round(h * scale / multiple)) * multiple)
    nw = max(multiple, int(round(w * scale / multiple)) * multiple)
    return nh, nw


def pick_device(requested="auto"):
    import torch

    if requested != "auto":
        return requested
    if torch.cuda.is_available():
        return "cuda"
    if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def default_batch(device):
    return {"cuda": 4, "mps": 2}.get(device, 1)


class FakeModel:
    """Profondità sintetica da luminanza e gradiente verticale. Solo per i test."""

    device = "cpu"
    batch = 2

    def predict(self, frames, size):
        import cv2

        out = []
        for f in frames:
            nh, nw = target_hw(f.shape[0], f.shape[1], size)
            g = cv2.cvtColor(f, cv2.COLOR_RGB2GRAY).astype(np.float32) / 255.0
            g = cv2.resize(g, (nw, nh), interpolation=cv2.INTER_AREA)
            ramp = np.linspace(0.0, 1.0, nh, dtype=np.float32)[:, None]
            out.append(g * 8.0 + ramp * 4.0)
        return np.stack(out)


class DepthAnything:
    def __init__(self, name, device="auto", batch=0, half=None, model=None):
        import torch
        from transformers import AutoModelForDepthEstimation

        self.torch = torch
        self.device = pick_device(device)
        self.batch = batch if batch > 0 else default_batch(self.device)
        use_half = self.device == "cuda" if half is None else half
        self.dtype = torch.float16 if use_half else torch.float32

        if model is None:
            source = name if os.path.isdir(name) else REPOS.get(name, name)
            model = AutoModelForDepthEstimation.from_pretrained(source)
        self.model = model.to(self.device, dtype=self.dtype).eval()

        self.mean = torch.tensor(IMAGENET_MEAN, device=self.device).view(1, 3, 1, 1)
        self.std = torch.tensor(IMAGENET_STD, device=self.device).view(1, 3, 1, 1)

    def predict(self, frames, size):
        """frames: lista di array RGB uint8 della stessa dimensione.
        Ritorna (B, H', W') float32, profondità inversa relativa (alto = vicino)."""
        import torch
        import torch.nn.functional as F

        h, w = frames[0].shape[:2]
        nh, nw = target_hw(h, w, size)
        x = torch.from_numpy(np.stack(frames)).to(self.device)
        x = x.permute(0, 3, 1, 2).float() / 255.0
        x = F.interpolate(x, size=(nh, nw), mode="bicubic", align_corners=False).clamp_(0, 1)
        x = ((x - self.mean) / self.std).to(self.dtype)
        with torch.inference_mode():
            d = self.model(pixel_values=x).predicted_depth
        if d.shape[-2:] != (nh, nw):
            d = F.interpolate(d[:, None].float(), size=(nh, nw), mode="bilinear", align_corners=False)[:, 0]
        return d.float().cpu().numpy()


def load(name, device="auto", batch=0):
    if name == "fake":
        return FakeModel()
    return DepthAnything(name, device=device, batch=batch)
