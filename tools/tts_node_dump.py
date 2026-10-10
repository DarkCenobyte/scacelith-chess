#!/usr/bin/env python3
"""Every intermediate value of one Supertonic 3 graph under onnxruntime, for per-node comparisons
with the C++ interpreter (tests/tts_tests.cpp, test tts_node_diff, enabled by the environment
variable SCACELITH_TTS_NODE_DUMP).

    tools/tts_node_dump.py --model DIR STAGE REFERENCE.bin OUT.bin

STAGE is dp, te, ve (first Euler step) or voc; REFERENCE.bin is a dump of tools/tts_reference.py
(its inputs are reused). OUT.bin uses the same container, with ONNX element type codes (1 float,
2 uint8, 3 int8, 6 int32, 7 int64, 9 bool); graph optimisations are disabled so the values are
those of the graph as exported.
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from tts_reference import write_container  # noqa: E402

FILES = {"dp": "duration_predictor", "te": "text_encoder", "ve": "vector_estimator", "voc": "vocoder"}
CODES = {1: np.float32, 2: np.uint8, 3: np.int8, 6: np.int32, 7: np.int64, 9: np.bool_}


def read_container(path):
    data = open(path, "rb").read()
    assert data[:4] == b"STTD"
    _, count = struct.unpack_from("<II", data, 4)
    pos = 12
    out = {}
    for _ in range(count):
        (n,) = struct.unpack_from("<H", data, pos)
        pos += 2
        name = data[pos:pos + n].decode()
        pos += n
        code, rank = struct.unpack_from("<BB", data, pos)
        pos += 2
        dims = struct.unpack_from("<%dq" % rank, data, pos)
        pos += 8 * rank
        dt = np.dtype(CODES[code])
        count_el = int(np.prod(dims)) if rank else 1
        out[name] = np.frombuffer(data, dtype=dt, count=count_el, offset=pos).reshape(dims)
        pos += count_el * dt.itemsize
    return out


def graph_path(model_dir, stage):
    """The stage's graph in either layout (tools/tts_reference.py): official flat or under onnx/,
    else the old INT8 file."""
    for p in (model_dir + "/" + FILES[stage] + ".onnx", model_dir + "/onnx/" + FILES[stage] + ".onnx"):
        if os.path.isfile(p):
            return p
    return model_dir + "/" + FILES[stage] + ".int8.onnx"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("stage", choices=sorted(FILES))
    ap.add_argument("reference")
    ap.add_argument("out")
    a = ap.parse_args()
    import onnx
    import onnxruntime as ort
    ref = read_container(a.reference)
    m = onnx.load(graph_path(a.model, a.stage))
    known = {o.name for o in m.graph.output}
    for node in m.graph.node:
        for o in node.output:
            if o and o not in known:
                m.graph.output.extend([onnx.ValueInfoProto(name=o)])
                known.add(o)
    opts = ort.SessionOptions()
    opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    opts.intra_op_num_threads = 1
    s = ort.InferenceSession(m.SerializeToString(), sess_options=opts, providers=["CPUExecutionProvider"])
    feeds = {}
    for i in s.get_inputs():
        if i.name == "noisy_latent":
            feeds[i.name] = ref["noise"]
        elif i.name == "current_step":
            feeds[i.name] = np.array([0], dtype=np.float32)
        elif i.name == "total_step":
            feeds[i.name] = ref["steps"].astype(np.float32)
        elif i.name == "latent":
            feeds[i.name] = ref["latent%d" % int(ref["steps"][0])]
        else:
            feeds[i.name] = ref[i.name]
    names = [o.name for o in s.get_outputs()]
    values = s.run(names, feeds)
    out = {}
    for n, v in zip(names, values):
        out[n] = np.asarray(v)
    write_container(a.out, out)
    print(len(out), "values")
    return 0


if __name__ == "__main__":
    sys.exit(main())
