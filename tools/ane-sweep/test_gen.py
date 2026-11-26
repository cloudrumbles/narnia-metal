import json
import subprocess
import sys
from pathlib import Path

import coremltools as ct
import pytest

HERE = Path(__file__).parent
ALLOWED_OPS = {"conv", "silu", "add", "pixel_shuffle", "pixel_unshuffle", "space_to_depth", "clip", "const"}


def expected_variants():
    v = {}
    for c in (64, 48, 32, 24, 16):
        for b in (6, 4, 2):
            v[f"rgb_c{c}_b{b}_960x544"] = (3, 544, 960, 3, 2)
    for w, h in ((848, 480), (768, 432), (640, 352)):
        v[f"rgb_c64_b6_{w}x{h}"] = (3, h, w, 3, 2)
    for c in (64, 32):
        for b in (6, 4):
            v[f"rgbhalf_c{c}_b{b}_960x544"] = (3, 544, 960, 3, 2)
    for c in (32, 16):
        v[f"y_c{c}_b4_960x544"] = (1, 544, 960, 1, 2)
    v["yhalf_c32_b4_960x544"] = (1, 544, 960, 1, 2)
    return v


@pytest.fixture(scope="session")
def out(tmp_path_factory):
    d = tmp_path_factory.mktemp("sweep")
    subprocess.run([sys.executable, str(HERE / "gen.py"), str(d)], check=True)
    return d


def manifest(out):
    return json.loads((out / "manifest.json").read_text())


def test_manifest_lists_every_variant_once(out):
    names = [m["name"] for m in manifest(out)["models"]]
    assert sorted(names) == sorted(expected_variants())


def test_every_listed_file_exists_in_mlpackage_layout(out):
    for m in manifest(out)["models"]:
        files = set(m["files"])
        assert files == {
            f"{m['name']}.mlpackage/Manifest.json",
            f"{m['name']}.mlpackage/Data/com.apple.CoreML/model.mlmodel",
            f"{m['name']}.mlpackage/Data/com.apple.CoreML/weights/weight.bin",
        }
        for f in files:
            assert (out / f).is_file(), f


def test_manifest_shapes_match_variant(out):
    exp = expected_variants()
    for m in manifest(out)["models"]:
        cin, h, w, cout, s = exp[m["name"]]
        assert m["input_shape"] == [1, cin, h, w]
        assert m["output_shape"] == [1, cout, h * s, w * s]
        assert m["input"] and m["output"]


def test_model_io_is_fp16_multiarray_with_those_shapes(out):
    for m in manifest(out)["models"]:
        spec = ct.utils.load_spec(str(out / f"{m['name']}.mlpackage"))
        (i,) = spec.description.input
        (o,) = spec.description.output
        assert i.name == m["input"] and o.name == m["output"]
        F16 = ct.proto.FeatureTypes_pb2.ArrayFeatureType.FLOAT16
        assert i.type.multiArrayType.dataType == F16
        assert o.type.multiArrayType.dataType == F16
        assert list(i.type.multiArrayType.shape) == m["input_shape"]
        assert list(o.type.multiArrayType.shape) == m["output_shape"]


def test_graphs_use_only_ane_native_ops(out):
    for m in manifest(out)["models"]:
        spec = ct.utils.load_spec(str(out / f"{m['name']}.mlpackage"))
        ops = {op.type for fn in spec.mlProgram.functions.values() for blk in fn.block_specializations.values() for op in blk.operations}
        assert ops <= ALLOWED_OPS, (m["name"], ops - ALLOWED_OPS)
        assert "conv" in ops


def test_stock_variant_has_pipersr_weight_count(out):
    size = (out / "rgb_c64_b6_960x544.mlpackage/Data/com.apple.CoreML/weights/weight.bin").stat().st_size
    assert 2 * 440_000 < size < 2 * 470_000 + 65_536


def test_conv_count_follows_blocks(out):
    for name, b in (("rgb_c32_b2_960x544", 2), ("rgb_c32_b6_960x544", 6), ("rgbhalf_c32_b4_960x544", 4)):
        spec = ct.utils.load_spec(str(out / f"{name}.mlpackage"))
        n = sum(op.type == "conv" for fn in spec.mlProgram.functions.values() for blk in fn.block_specializations.values() for op in blk.operations)
        assert n == 2 * b + 2, (name, n)
