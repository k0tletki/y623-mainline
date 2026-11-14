# Bit-exact model of the V831 NPU (NVDLA nv_small) conv + SDP + LUT path, as measured:
#   acc (int32) -> BS: v = rha((acc + (b << sh)) * m, t)      [round half away from zero]
#   -> LUT (LO table, 257 entries): i = (v-start)>>sel, f = fraction;
#      out = rha(T[i]*(2^sel - f) + T[i+1]*f, sel)  (weighted sum, 249/249); under/overflow slopes
#   -> CVT: out = sat8(rha((x - off) * scale, trunc))
import numpy as np

def rha(v, t):
    v = np.asarray(v, np.int64)
    if t == 0: return v
    return np.sign(v) * ((np.abs(v) + (1 << (t - 1))) >> t)

def bs(acc, b, sh, m, t):
    return rha((acc + (np.asarray(b, np.int64) << sh)) * np.asarray(m, np.int64), t)

def lut_lo(v, T, start, end, sel, uf=(0, 0), of=(0, 0)):
    v = np.asarray(v, np.int64); T = np.asarray(T, np.int64)
    o = np.clip(v - start, 0, None)
    i = np.minimum(o >> sel, 256); f = o & ((1 << sel) - 1)
    i1 = np.minimum(i + 1, 256)
    out = rha(T[i] * ((1 << sel) - f) + T[i1] * f, sel)   # measured: weighted sum, rha
    out = np.where(v < start, T[0] + rha((v - start) * uf[0], uf[1]), out)
    out = np.where(v > end, T[256] + rha((v - end) * of[0], of[1]), out)
    return out

def cvt(x, scale, trunc, off=0):
    return np.clip(rha((np.asarray(x, np.int64) - off) * scale, trunc), -128, 127)

def mul_shift(M, bits=15):
    """real multiplier -> (int16 m, truncate t) with |m| < 2^bits, maximal precision"""
    M = np.asarray(M, np.float64)
    t = np.floor(np.log2((2 ** bits - 1) / np.max(np.abs(M)))).astype(int)
    t = int(max(0, min(t, 31)))
    m = np.round(M * (1 << t)).astype(np.int64)
    return m, t

def bias_shift(bq):
    """int bias in acc units -> (int16 b, left shift sh)"""
    mx = np.max(np.abs(bq)) if np.size(bq) else 0
    sh = 0
    while mx / (1 << sh) > 32767: sh += 1
    return np.round(np.asarray(bq) / (1 << sh)).astype(np.int64), sh

# ---- NVDLA memory layouts
def to_surface(x):            # x [C,H,W] int8 -> bytes, [C/8][H][W][8]
    C, H, W = x.shape; Cp = (C + 7) // 8 * 8
    p = np.zeros((Cp, H, W), np.int8); p[:C] = x
    return p.reshape(Cp // 8, 8, H, W).transpose(0, 2, 3, 1).tobytes()

def from_surface(b, C, H, W):
    Cp = (C + 7) // 8 * 8
    a = np.frombuffer(b, np.int8)[:Cp * H * W].reshape(Cp // 8, H, W, 8).transpose(0, 3, 1, 2)
    return a.reshape(Cp, H, W)[:C]

def to_weights(w):            # w [K,C,R,S] int8 -> [K/8][C/32][R][S][8 k][32 c] (last blocks narrower)
    K, C, R, S = w.shape
    out = bytearray()
    for kg in range(0, K, 8):
        kk = w[kg:kg + 8]
        for cb in range(0, C, 32):
            blk = kk[:, cb:cb + 32]               # [k<=8, c<=32, R, S]
            out += blk.transpose(2, 3, 0, 1).astype(np.int8).tobytes()
    return bytes(out)
