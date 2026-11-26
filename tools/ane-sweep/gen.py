import json
import sys
from pathlib import Path

import coremltools as ct
import numpy as np
import torch
from torch import nn


class Net(nn.Module):
    def __init__(self, ch, width, blocks, unshuffle, shuffle):
        super().__init__()
        self.pre = nn.PixelUnshuffle(2) if unshuffle else nn.Identity()
        cin = ch * 4 if unshuffle else ch
        self.head = nn.Conv2d(cin, width, 3, padding=1)
        self.blocks = nn.ModuleList(
            nn.Sequential(
                nn.Conv2d(width, width, 3, padding=1),
                nn.SiLU(),
                nn.Conv2d(width, width, 3, padding=1),
            )
            for _ in range(blocks)
        )
        self.tail = nn.Conv2d(width, ch * shuffle * shuffle, 3, padding=1)
        self.post = nn.PixelShuffle(shuffle)

    def forward(self, x):
        x = self.head(self.pre(x))
        for block in self.blocks:
            x = x + block(x)
        return torch.clamp(self.post(self.tail(x)), 0, 1)


def variants():
    for c in (64, 48, 32, 24, 16):
        for b in (6, 4, 2):
            yield f"rgb_c{c}_b{b}_960x544", 3, c, b, 960, 544, False
    for w, h in ((848, 480), (768, 432), (640, 352)):
        yield f"rgb_c64_b6_{w}x{h}", 3, 64, 6, w, h, False
    for c in (64, 32):
        for b in (6, 4):
            yield f"rgbhalf_c{c}_b{b}_960x544", 3, c, b, 960, 544, True
    for c in (32, 16):
        yield f"y_c{c}_b4_960x544", 1, c, 4, 960, 544, False
    yield "yhalf_c32_b4_960x544", 1, 32, 4, 960, 544, True


def build(out, name, ch, width, blocks, w, h, half):
    torch.manual_seed(0)
    net = Net(ch, width, blocks, half, 4 if half else 2).eval()
    shape = (1, ch, h, w)
    traced = torch.jit.trace(net, torch.rand(*shape))
    model = ct.convert(
        traced,
        convert_to="mlprogram",
        minimum_deployment_target=ct.target.iOS17,
        compute_precision=ct.precision.FLOAT16,
        inputs=[ct.TensorType(name="input", shape=shape, dtype=np.float16)],
        outputs=[ct.TensorType(name="output", dtype=np.float16)],
    )
    model.save(str(out / f"{name}.mlpackage"))
    root = f"{name}.mlpackage"
    return {
        "name": name,
        "input": "input",
        "output": "output",
        "input_shape": list(shape),
        "output_shape": [1, ch, h * 2, w * 2],
        "files": [
            f"{root}/Manifest.json",
            f"{root}/Data/com.apple.CoreML/model.mlmodel",
            f"{root}/Data/com.apple.CoreML/weights/weight.bin",
        ],
    }


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    models = [build(out, *v) for v in variants()]
    (out / "manifest.json").write_text(json.dumps({"models": models}, indent=2))


main()
