import numpy as np
import pytest

from depthyum.model import DepthAnything, target_hw


def test_target_hw_multiples_of_14():
    for h, w in [(1080, 1920), (1920, 1080), (96, 160), (500, 500)]:
        nh, nw = target_hw(h, w, 518)
        assert nh % 14 == 0 and nw % 14 == 0
        assert min(nh, nw) == 518 or abs(min(nh, nw) - 518) <= 7
    assert target_hw(1080, 1920, 518) == (518, 924)


def tiny_model():
    transformers = pytest.importorskip("transformers")
    from transformers import Dinov2Config, DepthAnythingConfig, DepthAnythingForDepthEstimation

    backbone = Dinov2Config(
        hidden_size=32, num_hidden_layers=4, num_attention_heads=2, intermediate_size=64,
        patch_size=14, image_size=518, out_indices=[1, 2, 3, 4],
        reshape_hidden_states=False, apply_layernorm=True,
    )
    cfg = DepthAnythingConfig(
        backbone_config=backbone, patch_size=14, reassemble_hidden_size=32,
        neck_hidden_sizes=[8, 16, 24, 32], fusion_hidden_size=16, head_hidden_size=8,
    )
    return DepthAnythingForDepthEstimation(cfg)


def test_predict_shapes_with_random_weights():
    net = DepthAnything("tiny", device="cpu", batch=2, model=tiny_model())
    frames = [np.random.randint(0, 255, (72, 128, 3), np.uint8) for _ in range(2)]
    out = net.predict(frames, 112)
    nh, nw = target_hw(72, 128, 112)
    assert out.shape == (2, nh, nw) and out.dtype == np.float32
    assert np.isfinite(out).all()
