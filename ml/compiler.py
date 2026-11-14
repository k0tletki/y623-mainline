# YOLOv5n (cut, int8) -> NPU program for nvdla-rt/netrun.
#
#   compiler.py OUT.nnp [image]   (with an image: also OUT.nnp.expect + OUT.nnp.input)
#
# Program ("NNP1", little endian):
#   header u32[16]: magic, version, arena, wsize, x1size, nluts, nops, in_off, in_line,
#                   in_surf, in_w, in_h, nout, f32 out_scale[3]
#   outputs: nout x u32[6] (off, line, surf, c, h, w)
#   luts:    nluts x (i32[7] lo_start, lo_end, sel, uf_scale, uf_shift, of_scale, of_shift; i16[257] table)
#   ops:     nops x i32[48]
#   weights blob, x1 blob
# Feature tensors live in one arena (int8 NVDLA surfaces); concat inputs are channel
# slices of the concat output; storage is reused once a tensor is dead.
import os, sys, struct, glob, json
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import qsim, hwmodel as h, export_layer as ex

OP_CONV, OP_ADD, OP_MAXPOOL, OP_UP2 = 1, 2, 3, 4
CBUF_BANKS, BANK = 32, 4096
DATA_BANKS_MAX = 12      # measured envelope (3x3, C=64 grid): data <= 12 banks
TOTAL_BANKS_MAX = 28     # and data + weights <= 28; larger data allocations broke at
                         # 13+14, 15+5 banks although the nominal CBUF is 32 banks
ALIGN = 64

def shapes():
    import onnx
    from onnx import shape_inference
    m = shape_inference.infer_shapes(onnx.load(qsim.MODEL))
    sh = {}
    for v in list(m.graph.value_info) + list(m.graph.input) + list(m.graph.output):
        sh[v.name] = [d.dim_value for d in v.type.tensor_type.shape.dim][1:]
    return sh

class Storage:
    """a surface region: root tensor + channel offset; strides of the root"""
    def __init__(s, c, hh, w):
        s.c, s.h, s.w = c, hh, w
        s.line = w * 8; s.surf = s.line * hh
        s.size = s.surf * ((c + 7) // 8)
        s.off = None; s.first = s.last = None

def build(qm, pre):
    sh = shapes()
    layers = qsim.layers
    # ---- storage and aliasing (concat inputs are slices of the concat output)
    root = {}        # tensor -> (Storage, channel offset)
    for L in reversed(layers):           # outputs first so concats can claim inputs
        if L["op"] == "concat":
            if L["out"] not in root:
                c, hh, w = sh[L["out"]]; root[L["out"]] = (Storage(c, hh, w), 0)
            st, base = root[L["out"]]
            off = base
            for t in L["inps"]:
                assert off % 8 == 0
                assert t not in root, "tensor in two concats: " + t
                root[t] = (st, off); off += sh[t][0]
    for t in ["images"] + [L["out"] for L in layers]:
        if t not in root:
            c, hh, w = sh[t] if t != "images" else (8, *sh["images"][1:])
            root[t] = (Storage(c, hh, w), 0)
    # lifetimes in op order (index of layer); network inputs/outputs live throughout
    for i, L in enumerate(layers):
        for t in (L.get("inps") or [L["inp"]]) + [L["out"]]:
            st = root[t][0]
            st.first = i if st.first is None else min(st.first, i)
            st.last = i if st.last is None else max(st.last, i)
    stores = list({id(v[0]): v[0] for v in root.values()}.values())
    root["images"][0].first = -1
    for o in qsim.outputs: root[o][0].last = len(layers)
    # ---- arena: greedy first-fit over lifetimes
    placed = []
    for st in sorted(stores, key=lambda s: (-s.size)):
        live = [p for p in placed if not (p.last < st.first or st.last < p.first)]
        cands = sorted({0} | {(p.off + p.size + ALIGN - 1) // ALIGN * ALIGN for p in live})
        for cand in cands:
            if all(cand + st.size <= p.off or p.off + p.size <= cand for p in live):
                st.off = cand; break
        placed.append(st)
    arena = max(s.off + s.size for s in stores)
    def cube(t, y0=0, rows=None):
        st, ch = root[t]
        c, hh, w = sh[t] if t != "images" else (8, *sh["images"][1:])
        rows = hh if rows is None else rows
        return [st.off + (ch // 8) * st.surf + y0 * st.line, w, rows, c, st.line, st.surf]
    # ---- ops
    ops, wblob, x1blob, luts = [], bytearray(), bytearray(), []
    prog_names = []
    for L in layers:
        if L["op"] == "conv":
            wq, sw, bq = qm.prep[L["name"]]
            p = ex.layer_params(qm, L, pre[L["name"]])
            K, C, R, S = wq.shape
            Cin = (C + 7) // 8 * 8
            if Cin != C:                                   # first layer: 3 -> 8 channels
                wq = np.concatenate([wq, np.zeros((K, Cin - C, R, S))], 1)
            _, Hi, Wi = (Cin, *sh[L["inp"]][1:]) if L["inp"] == "images" else sh[L["inp"]]
            _, Ho, Wo = sh[L["out"]]
            st, pad = L["stride"], L["pad"]
            lut = -1
            if p["T"] is not None:
                lut = len(luts); luts.append(p)
            # K split: weights of a kernel group of 8 are contiguous
            bpk = R * S * Cin
            best = None
            for kk in range(8, ((K + 7) // 8) * 8 + 1, 8):
                wb = -(-(kk * bpk) // BANK)
                if wb >= TOTAL_BANKS_MAX: break
                db = min(TOTAL_BANKS_MAX - wb, DATA_BANKS_MAX)
                # rows of input that fit db banks
                eps = Wi * (Cin // 8)
                rin = db * 512 // eps
                rout = min(Ho, (rin - R) // st + 1) if rin >= R else 0
                if rout <= 0: continue
                nops_ = -(-K // kk) * -(-Ho // rout)
                if best is None or nops_ < best[0]: best = (nops_, kk, rout)
            assert best, "conv %s does not fit" % L["name"]
            _, kk, rout = best
            wfull = h.to_weights(wq.astype(np.int8)); woff0 = len(wblob); wblob += wfull
            wblob += b"\0" * (-len(wblob) % 32)
            x1 = np.stack([p["b"], p["m"]], 1).astype(np.int16).tobytes(); x1off0 = len(x1blob)
            x1blob += x1 + b"\0" * (-len(x1) % 32)
            for k0 in range(0, K, kk):
                k1 = min(K, k0 + kk)
                for y0 in range(0, Ho, rout):
                    y1 = min(Ho, y0 + rout)
                    iy0 = y0 * st - pad; iy1 = (y1 - 1) * st - pad + R      # input rows [iy0, iy1)
                    pt = max(0, -iy0); pb = max(0, iy1 - Hi)
                    iy0c, iy1c = max(0, iy0), min(Hi, iy1)
                    ic = cube(L["inp"], iy0c, iy1c - iy0c)
                    ic[3] = Cin
                    oc = cube(L["out"], y0, y1 - y0)
                    oc[0] += (k0 // 8) * oc[5]; oc[3] = k1 - k0
                    prog_names.append(L["name"])
                    op = [OP_CONV] + ic + oc + [woff0 + k0 * bpk, (k1 - k0) * bpk, k1 - k0, R, S, st,
                          pt, pb, pad, pad, x1off0 + k0 * 4, p["sh"], p["t"],
                          2 if lut >= 0 else 0, lut, p["cvt"][0], p["cvt"][1], 0]
                    ops.append(op)
        elif L["op"] == "add":
            prog_names.append(L["name"])
            ops.append([OP_ADD] + cube(L["inps"][0]) + cube(L["inps"][1]) + cube(L["out"]) + [1, 0, 0])
        elif L["op"] == "maxpool":
            prog_names.append(L["name"])
            ops.append([OP_MAXPOOL] + cube(L["inp"]) + cube(L["out"]) + [L["k"], L["pad"]])
        elif L["op"] == "up2":
            prog_names.append(L["name"])
            ops.append([OP_UP2] + cube(L["inp"]) + cube(L["out"]))
        elif L["op"] == "concat":
            pass                                           # placement only
    return dict(names=prog_names, ops=ops, w=bytes(wblob), x1=bytes(x1blob), luts=luts, arena=arena,
                cube=cube, root=root, sh=sh)

def check_bounds(prog):
    """every NPU/CPU access must stay inside its buffer (the NPU has no MMU here)"""
    A, W, X = prog["arena"], len(prog["w"]), len(prog["x1"])
    bad = 0
    def ext(c):   # off, w, h, c, line, surf -> last byte + 1
        return c[0] + c[5] * ((c[3] + 7) // 8 - 1) + c[4] * (c[2] - 1) + c[1] * 8
    for i, op in enumerate(prog["ops"]):
        cubes = [op[1:7], op[7:13]] + ([op[13:19]] if op[0] == OP_ADD else [])
        for c in cubes:
            if c[0] < 0 or ext(c) > A:
                print("op %d: cube %s ends at %d > arena %d" % (i, c, ext(c), A)); bad += 1
        if op[0] == OP_CONV:
            woff, wsz, k = op[13], op[14], op[15]
            if woff + wsz > W: print("op %d: weights %d+%d > %d" % (i, woff, wsz, W)); bad += 1
            if op[23] + 4 * k > X: print("op %d: x1 out of range" % i); bad += 1
            if wsz != k * op[16] * op[17] * op[4]:
                print("op %d: weight size %d != k*r*s*c %d" % (i, wsz, k * op[16] * op[17] * op[4])); bad += 1
    print("bounds check: %d problems in %d ops" % (bad, len(prog["ops"])))
    return bad == 0

def write(prog, path):
    ops = prog["ops"]; luts = prog["luts"]; cube = prog["cube"]
    ic = cube("images")
    outs = [cube(o) for o in qsim.outputs]
    # header words 13..15: float32 dequant scale of each head output (int8 * scale = logit)
    osc = [struct.unpack("<I", struct.pack("<f", prog["oscale"][i]))[0] if i < len(outs) else 0 for i in range(3)]
    hdr = struct.pack("<16I", 0x31504e4e, 1, prog["arena"], len(prog["w"]), len(prog["x1"]), len(luts),
                      len(ops), ic[0], ic[4], ic[5], ic[1], ic[2], len(outs), *osc)
    body = b"".join(struct.pack("<6I", o[0], o[4], o[5], o[3], o[2], o[1]) for o in outs)
    for p in luts:
        body += struct.pack("<7i", ex.LO_START, ex.LO_END, ex.LO_SEL, 0, 0, p["of"][0], p["of"][1])
        body += np.asarray(p["T"], np.int16).tobytes()
    for op in ops:
        assert len(op) <= 48, op
        body += struct.pack("<48i", *(op + [0] * (48 - len(op))))
    open(path, "wb").write(hdr + body + prog["w"] + prog["x1"])

def expected(qm, pre, prog, img):
    """bit-exact tensors for every layer (hwmodel), in arena layout"""
    u8 = qsim.preprocess(img)
    q = {"images": u8.transpose(2, 0, 1).astype(np.int64) - 128}
    q["images"] = np.concatenate([q["images"], np.zeros((5,) + q["images"].shape[1:], np.int64)], 0)
    arena = bytearray(prog["arena"])
    for L in qsim.layers:
        if L["op"] == "conv":
            p = ex.layer_params(qm, L, pre[L["name"]])
            x = q[L["inp"]][:qm.prep[L["name"]][0].shape[1]]
            y = ex.run_layer_hw(qm, L, p, x).astype(np.int64)
        elif L["op"] == "add":
            y = np.clip(q[L["inps"][0]] + q[L["inps"][1]], -128, 127)
        elif L["op"] == "concat":
            y = np.concatenate([q[t] for t in L["inps"]], 0)
        elif L["op"] == "maxpool":
            k, pp = L["k"], L["pad"]
            xx = np.pad(q[L["inp"]], ((0, 0), (pp, pp), (pp, pp)), constant_values=-128)
            y = np.lib.stride_tricks.sliding_window_view(xx, (k, k), axis=(1, 2)).max((-1, -2))
        elif L["op"] == "up2":
            y = q[L["inp"]].repeat(2, 1).repeat(2, 2)
        q[L["out"]] = y
    return q, u8

def tensor_bytes(x):
    return h.to_surface(np.asarray(x, np.int8))

if __name__ == "__main__":
    out = sys.argv[1]
    amax = json.load(open("amax.json")); pre = json.load(open("preact.json"))
    qm = qsim.QModel(amax)
    prog = build(qm, pre)
    prog["oscale"] = [qm.s[o] for o in qsim.outputs]
    if not check_bounds(prog): sys.exit(1)
    write(prog, out)
    open(out + ".opmap", "w").write("".join("%d %s %s\n" % (i, n, op[:7]) for i, (n, op) in enumerate(zip(prog["names"], prog["ops"]))))
    nconv = sum(o[0] == OP_CONV for o in prog["ops"])
    print("ops %d (conv tiles %d, add %d, cpu %d)  arena %d KB  weights %d KB  x1 %d KB  luts %d"
          % (len(prog["ops"]), nconv, sum(o[0] == OP_ADD for o in prog["ops"]),
             sum(o[0] in (OP_MAXPOOL, OP_UP2) for o in prog["ops"]), prog["arena"] // 1024,
             len(prog["w"]) // 1024, len(prog["x1"]) // 1024, len(prog["luts"])))
    if len(sys.argv) > 2:
        q, u8 = expected(qm, pre, prog, sys.argv[2])
        # input in arena layout, and the expected head outputs
        open(out + ".input", "wb").write(tensor_bytes(q["images"]))
        with open(out + ".expect", "wb") as f:
            for o in qsim.outputs:
                f.write(tensor_bytes(q[o]))
        # per-tensor expectation for debugging: name, arena cube, bytes
        with open(out + ".tensors", "wb") as f:
            for L in qsim.layers:
                t = L["out"]; c = prog["cube"](t)
                b = tensor_bytes(q[t]); nm = L["name"].encode()
                f.write(struct.pack("<I", len(nm)) + nm + struct.pack("<6I", *c) + struct.pack("<I", len(b)) + b)
        print("wrote input, expected outputs and %d layer tensors" % len(qsim.layers))
