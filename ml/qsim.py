# int8 simulation of the cut YOLOv5n, designed to map onto NVDLA:
#  - weights int8 per output channel, activations int8 symmetric per tensor
#  - Conv(+SiLU) fused; bias in int32 accumulator units; requant to the output scale
#  - Concat / residual Add inputs share the output's scale (pure placement / int add)
#  - input fed as (pixel - 128), the +128 folded into the first conv's bias
import os, sys, glob, json
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import numpy as np, onnx, onnxruntime as ort
from onnx import numpy_helper, helper
from PIL import Image
import ref

MODEL = "yolov5n-192x320-fp32.onnx"
m = onnx.load(MODEL); g = m.graph
init = {t.name: numpy_helper.to_array(t) for t in g.initializer}
nodes = list(g.node)
consumers = {}
for n in nodes:
    for i in n.input: consumers.setdefault(i, []).append(n)

# ---- graph -> layer list (fuse Conv+Sigmoid+Mul)
layers = []
skip = set()
for n in nodes:
    if n.name in skip or n.op_type == "Constant": continue
    if n.op_type == "Conv":
        c = consumers.get(n.output[0], [])
        sig = [x for x in c if x.op_type == "Sigmoid"]
        mul = [x for x in c if x.op_type == "Mul"]
        attrs = {a.name: helper.get_attribute_value(a) for a in n.attribute}
        L = dict(op="conv", name=n.name, inp=n.input[0], w=init[n.input[1]],
                 b=init[n.input[2]] if len(n.input) > 2 else None,
                 stride=attrs.get("strides", [1, 1])[0], pad=attrs.get("pads", [0] * 4)[0],
                 silu=False, out=n.output[0])
        if sig and mul and len(c) == 2:
            L["silu"] = True; L["out"] = mul[0].output[0]
            skip.add(sig[0].name); skip.add(mul[0].name)
        layers.append(L)
    elif n.op_type in ("Add", "Concat"):
        layers.append(dict(op=n.op_type.lower(), name=n.name, inps=list(n.input), out=n.output[0]))
    elif n.op_type == "MaxPool":
        a = {x.name: helper.get_attribute_value(x) for x in n.attribute}
        layers.append(dict(op="maxpool", name=n.name, inp=n.input[0], k=a["kernel_shape"][0],
                           pad=a["pads"][0], out=n.output[0]))
    elif n.op_type == "Resize":
        layers.append(dict(op="up2", name=n.name, inp=n.input[0], out=n.output[0]))
    else:
        raise SystemExit("unhandled op %s %s" % (n.op_type, n.name))
outputs = [o.name for o in g.output]

def preprocess(path):
    img = Image.open(path).convert("RGB")
    lb, *_ = ref.letterbox(img)
    return np.asarray(lb, np.uint8)            # H W 3

# ---- calibration: max |x| of every layer output (fp32, ORT)
def calibrate(paths):
    mm = onnx.load(MODEL)
    names = [L["out"] for L in layers]
    have = {o.name for o in mm.graph.output}
    for nm in names:
        if nm not in have: mm.graph.output.append(helper.make_tensor_value_info(nm, onnx.TensorProto.FLOAT, None))
    sess = ort.InferenceSession(mm.SerializeToString(), providers=["CPUExecutionProvider"])
    onames = [o.name for o in mm.graph.output]
    amax = {}
    for p in paths:
        x = (preprocess(p).astype(np.float32) / 255).transpose(2, 0, 1)[None]
        for nm, v in zip(onames, sess.run(None, {"images": x})):
            amax[nm] = max(amax.get(nm, 0), float(np.percentile(np.abs(v), 99.99)))
    return amax

def unify(amax):
    """Concat/Add: inputs share the output scale (iterate to a fixed point)."""
    for _ in range(10):
        changed = False
        for L in layers:
            if L["op"] in ("concat", "add"):
                group = L["inps"] + [L["out"]]
                mx = max(amax[t] for t in group)
                for t in group:
                    if amax[t] != mx: amax[t] = mx; changed = True
            if L["op"] in ("maxpool", "up2"):     # scale preserving
                mx = max(amax[L["inp"]], amax[L["out"]])
                if amax[L["inp"]] != mx or amax[L["out"]] != mx:
                    amax[L["inp"]] = amax[L["out"]] = mx; changed = True
        if not changed: break
    return amax

def conv_int(q, w, stride, pad):
    """q: [C,H,W] int8 values (as float64), w: [K,C,kh,kw] -> [K,Ho,Wo] exact integers"""
    C, Hh, Ww = q.shape; K, _, kh, kw = w.shape
    qp = np.pad(q, ((0, 0), (pad, pad), (pad, pad)))
    Ho = (Hh + 2 * pad - kh) // stride + 1; Wo = (Ww + 2 * pad - kw) // stride + 1
    cols = np.lib.stride_tricks.sliding_window_view(qp, (kh, kw), axis=(1, 2))[:, ::stride, ::stride][:, :Ho, :Wo]
    cols = cols.transpose(1, 2, 0, 3, 4).reshape(Ho * Wo, C * kh * kw)
    return (cols @ w.reshape(K, -1).T.astype(np.float64)).T.reshape(K, Ho, Wo)

def silu(x): return x / (1 + np.exp(-x))

class QModel:
    def __init__(self, amax):
        self.s = {t: max(v, 1e-6) / 127 for t, v in amax.items()}
        self.s["images"] = 1 / 255.0          # q = pixel - 128 (offset folded below)
        self.prep = {}
        for L in layers:
            if L["op"] != "conv": continue
            w = L["w"].astype(np.float64); K = w.shape[0]
            sw = np.maximum(np.abs(w).reshape(K, -1).max(1), 1e-12) / 127
            wq = np.clip(np.round(w / sw[:, None, None, None]), -127, 127)
            b = L["b"].astype(np.float64) if L["b"] is not None else np.zeros(K)
            sin = self.s[L["inp"]]
            if L["inp"] == "images":            # x = (q + 128)/255 -> add 128*sum(w) to the acc
                b = b + 128 * sin * (wq * sw[:, None, None, None]).reshape(K, -1).sum(1)
            bq = np.round(b / (sin * sw))      # int32 bias in accumulator units
            self.prep[L["name"]] = (wq, sw, bq)
    def run(self, img_u8, trace=None):
        q = {"images": img_u8.transpose(2, 0, 1).astype(np.float64) - 128}
        for L in layers:
            so = self.s[L["out"]]
            if L["op"] == "conv":
                wq, sw, bq = self.prep[L["name"]]
                acc = conv_int(q[L["inp"]], wq, L["stride"], L["pad"]) + bq[:, None, None]
                real = acc * (self.s[L["inp"]] * sw)[:, None, None]
                if L["silu"]: real = silu(real)
                out = np.clip(np.round(real / so), -128, 127)
            elif L["op"] == "add":
                out = np.clip(q[L["inps"][0]] + q[L["inps"][1]], -128, 127)   # shared scale
            elif L["op"] == "concat":
                out = np.concatenate([q[t] for t in L["inps"]], 0)            # shared scale
            elif L["op"] == "maxpool":
                k, p = L["k"], L["pad"]
                x = np.pad(q[L["inp"]], ((0, 0), (p, p), (p, p)), constant_values=-128)
                out = np.lib.stride_tricks.sliding_window_view(x, (k, k), axis=(1, 2)).max((-1, -2))
            elif L["op"] == "up2":
                out = q[L["inp"]].repeat(2, 1).repeat(2, 2)
            q[L["out"]] = out
        return [q[o] * self.s[o] for o in outputs]   # dequantised head outputs

if __name__ == "__main__":
    calib = sorted(glob.glob("calib/*.png")) + ["../dl/testimg/bus.jpg", "../dl/testimg/zidane.jpg"]
    amax = unify(calibrate(calib))
    json.dump(amax, open("amax.json", "w"))
    qm = QModel(amax)
    sess = ort.InferenceSession(MODEL, providers=["CPUExecutionProvider"])
    print("layers:", len(layers), " convs:", sum(L["op"] == "conv" for L in layers),
          " silu-fused:", sum(L.get("silu", False) for L in layers))
    for p in ["../dl/testimg/bus.jpg", "../dl/testimg/zidane.jpg", calib[0]]:
        u8 = preprocess(p)
        ref_out = sess.run(None, {"images": (u8.astype(np.float32) / 255).transpose(2, 0, 1)[None]})
        q_out = qm.run(u8)
        sim = [float(np.dot(a.ravel(), b.ravel()) / np.linalg.norm(a) / np.linalg.norm(b)) for a, b in zip(ref_out, q_out)]
        d_ref = ref.decode(ref_out); d_q = ref.decode([o[None] for o in q_out])
        fmt = lambda ds: ", ".join("%s %.2f" % (ref.COCO[c], s) for s, c, *_ in ds) or "-"
        print("%s\n  cosine per head %s\n  fp32: %s\n  int8: %s" % (p.split("/")[-1], ["%.4f" % s for s in sim], fmt(d_ref), fmt(d_q)))
