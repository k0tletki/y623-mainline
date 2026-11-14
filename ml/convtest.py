# convtest.py OUT R S stride pad [C K W H]: random int8 conv for layerrun, no SiLU,
# SDP x1 BOTH (bias 0, mul 1, truncate t) + CVT, exact expectation in OUT.expect
import os, sys, struct
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import numpy as np, hwmodel as h, qsim
out = sys.argv[1]; R, S, st, pad = map(int, sys.argv[2:6])
C, K, W, H = (map(int, sys.argv[6:10]) if len(sys.argv) > 9 else (8, 8, 32, 16))
rng = np.random.default_rng(1)
x = rng.integers(-128, 128, (C, H, W)); w = rng.integers(-20, 21, (K, C, R, S))
acc = qsim.conv_int(x.astype(float), w, st, pad).astype(np.int64)
t = int(np.ceil(np.log2(np.abs(acc).max() / 127 + 1e-9))) if np.abs(acc).max() > 127 else 0
b = np.zeros(K, np.int64); m = np.ones(K, np.int64)
y = h.cvt(h.bs(acc, 0, 0, 1, t), 1, 0).astype(np.int8)
Ho, Wo = y.shape[1:]
wb = h.to_weights(w.astype(np.int8)); x1 = np.stack([b, m], 1).astype(np.int16).tobytes(); inb = h.to_surface(x.astype(np.int8))
hdr = struct.pack("<4s16i", b"NNL1", W, H, C, K, R, S, st, pad, Wo, Ho, 0, t, 0, 1, 0, 0)
hdr += struct.pack("<3i4i", -4096, 4096, 5, 0, 0, 0, 0) + np.zeros(257, np.int16).tobytes()
open(out, "wb").write(hdr + struct.pack("<3I", len(wb), len(x1), len(inb)) + wb + x1 + inb)
open(out + ".expect", "wb").write(h.to_surface(y))
print("%s: %dx%dx%d * %dx%d s%d p%d -> %dx%dx%d, t=%d" % (out, C, H, W, R, S, st, pad, K, Ho, Wo, t))
