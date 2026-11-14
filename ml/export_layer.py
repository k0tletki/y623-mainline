# export one conv(+SiLU) layer of the quantised YOLOv5n for the NPU, with the bit-exact
# expected output. usage: export_layer.py LAYER_NAME image out.nnl
import os, sys, struct, json
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import numpy as np, onnx, onnxruntime as ort
from onnx import helper
import qsim, hwmodel as h

LO_START, LO_END, LO_SEL = -4096, 4096, 5

def preact_amax(paths):
    """max |conv output| before SiLU, per fused layer"""
    mm = onnx.load(qsim.MODEL)
    names = [L["name"] for L in qsim.layers if L["op"] == "conv"]
    pre = {}
    for n in mm.graph.node:
        if n.op_type == "Conv": pre[n.name] = n.output[0]
    for nm in pre.values():
        mm.graph.output.append(helper.make_tensor_value_info(nm, onnx.TensorProto.FLOAT, None))
    sess = ort.InferenceSession(mm.SerializeToString(), providers=["CPUExecutionProvider"])
    on = [o.name for o in mm.graph.output]
    amax = {}
    for p in paths:
        x = (qsim.preprocess(p).astype(np.float32) / 255).transpose(2, 0, 1)[None]
        for nm, v in zip(on, sess.run(None, {"images": x})):
            amax[nm] = max(amax.get(nm, 0), float(np.percentile(np.abs(v), 99.99)))
    return {k: amax[v] for k, v in pre.items()}

def silu(x): return x / (1 + np.exp(-x))

def layer_params(qm, L, pmax):
    wq, sw, bq = qm.prep[L["name"]]
    sin, sout = qm.s[L["inp"]], qm.s[L["out"]]
    K = wq.shape[0]
    p = {}
    if L["silu"]:
        s_v = pmax / LO_END                       # LUT input units
        m, t = h.mul_shift(sin * sw / s_v)
        s_T = pmax / 32000                        # LUT output units
        xs = (LO_START + np.arange(257) * (1 << LO_SEL)) * s_v
        T = np.clip(np.round(silu(xs) / s_T), -32768, 32767).astype(np.int64)
        of_slope = s_v / s_T                      # dT/dv for SiLU(x) ~ x
        of_m, of_t = h.mul_shift(np.array([of_slope]), bits=15)
        c_m, c_t = h.mul_shift(np.array([s_T / sout]), bits=15)
        p.update(T=T, of=(int(of_m[0]), of_t), cvt=(int(c_m[0]), c_t))
    else:
        m, t = h.mul_shift(sin * sw / sout)       # straight to the output scale
        p.update(T=None, cvt=(1, 0))
    b16, sh = h.bias_shift(bq)
    p.update(m=m, t=t, b=b16, sh=sh)
    return p

def run_layer_hw(qm, L, p, q_in):
    wq, sw, bq = qm.prep[L["name"]]
    acc = qsim.conv_int(q_in, wq, L["stride"], L["pad"]).astype(np.int64)
    v = h.bs(acc, p["b"][:, None, None], p["sh"], p["m"][:, None, None], p["t"])
    if p["T"] is not None:
        v = h.lut_lo(v, p["T"], LO_START, LO_END, LO_SEL, uf=(0, 0), of=p["of"])
    return h.cvt(v, p["cvt"][0], p["cvt"][1]).astype(np.int8)

if __name__ == "__main__":
    name, img, outp = sys.argv[1:4]
    calib = sorted(__import__("glob").glob("calib/*.png")) + ["../dl/testimg/bus.jpg", "../dl/testimg/zidane.jpg"]
    amax = json.load(open("amax.json")) if os.path.exists("amax.json") else qsim.unify(qsim.calibrate(calib))
    pre = json.load(open("preact.json")) if os.path.exists("preact.json") else preact_amax(calib)
    json.dump(pre, open("preact.json", "w"))
    qm = qsim.QModel(amax)
    # int8 tensors of the whole network for this image
    tr = {}
    u8 = qsim.preprocess(img)
    q = {"images": u8.transpose(2, 0, 1).astype(np.float64) - 128}
    L = next(x for x in qsim.layers if x["name"] == name)
    # run the float-requant simulator up to the layer's input
    for X in qsim.layers:
        if X is L: break
        sub = qsim.QModel.__new__(qsim.QModel); sub.__dict__ = qm.__dict__
    # reuse QModel.run with a trace: simplest is to run all and keep tensors
    def run_all():
        qq = {"images": q["images"]}
        for X in qsim.layers:
            so = qm.s[X["out"]]
            if X["op"] == "conv":
                wq, sw, bq = qm.prep[X["name"]]
                acc = qsim.conv_int(qq[X["inp"]], wq, X["stride"], X["pad"]) + bq[:, None, None]
                real = acc * (qm.s[X["inp"]] * sw)[:, None, None]
                if X["silu"]: real = qsim.silu(real)
                qq[X["out"]] = np.clip(np.round(real / so), -128, 127)
            elif X["op"] == "add": qq[X["out"]] = np.clip(qq[X["inps"][0]] + qq[X["inps"][1]], -128, 127)
            elif X["op"] == "concat": qq[X["out"]] = np.concatenate([qq[t] for t in X["inps"]], 0)
            elif X["op"] == "maxpool":
                k, pp = X["k"], X["pad"]
                xx = np.pad(qq[X["inp"]], ((0, 0), (pp, pp), (pp, pp)), constant_values=-128)
                qq[X["out"]] = np.lib.stride_tricks.sliding_window_view(xx, (k, k), axis=(1, 2)).max((-1, -2))
            elif X["op"] == "up2": qq[X["out"]] = qq[X["inp"]].repeat(2, 1).repeat(2, 2)
        return qq
    qq = run_all()
    q_in = qq[L["inp"]].astype(np.int64)
    p = layer_params(qm, L, pre[L["name"]])
    y_hw = run_layer_hw(qm, L, p, q_in)
    y_ref = qq[L["out"]].astype(np.int8)
    d = y_hw.astype(int) - y_ref
    print("%s: in %s out %s  t=%d sh=%d cvt=%s  hw-model vs float-requant: exact %.1f%%, |diff|<=1 %.1f%%, max %d"
          % (name, q_in.shape, y_hw.shape, p["t"], p["sh"], p["cvt"], 100 * (d == 0).mean(), 100 * (abs(d) <= 1).mean(), abs(d).max()))
    wq = qm.prep[name][0].astype(np.int8)
    K, C, R, S = wq.shape; _, Hh, Ww = q_in.shape; _, Ho, Wo = y_hw.shape
    wbytes = h.to_weights(wq)
    x1 = np.stack([p["b"], p["m"]], 1).astype(np.int16).tobytes()
    inb = h.to_surface(q_in.astype(np.int8))
    T = p["T"] if p["T"] is not None else np.zeros(257, np.int64)
    of = p.get("of", (0, 0))
    hdr = struct.pack("<4s16i", b"NNL1", Ww, Hh, C, K, R, S, L["stride"], L["pad"], Wo, Ho,
                      p["sh"], p["t"], 1 if p["T"] is not None else 0, p["cvt"][0], p["cvt"][1], 0)
    hdr += struct.pack("<3i4i", LO_START, LO_END, LO_SEL, 0, 0, of[0], of[1])
    hdr += np.asarray(T, np.int16).tobytes()
    blob = hdr + struct.pack("<3I", len(wbytes), len(x1), len(inb)) + wbytes + x1 + inb
    open(outp, "wb").write(blob)
    open(outp + ".expect", "wb").write(h.to_surface(y_hw))
    print("wrote", outp, len(blob), "bytes; expected output", len(h.to_surface(y_hw)), "bytes")
