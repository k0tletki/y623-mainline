import onnx, sys, numpy as np
from onnx import shape_inference
m = shape_inference.infer_shapes(onnx.load(sys.argv[1]))
g = m.graph
shp = {v.name: [d.dim_value for d in v.type.tensor_type.shape.dim] for v in list(g.value_info) + list(g.input) + list(g.output)}
init = {t.name: list(t.dims) for t in g.initializer}
tot = 0; rows = []; peak = 0
for n in g.node:
    if n.op_type == "Conv":
        w = init[n.input[1]]; o = shp[n.output[0]]; i = shp[n.input[0]]
        group = next((a.i for a in n.attribute if a.name == "group"), 1)
        macs = o[1] * o[2] * o[3] * (w[1] * w[2] * w[3])
        tot += macs
        rows.append((macs, n.name, i[1:], o[1:], w[2:], group))
        peak = max(peak, np.prod(i) + np.prod(o))
print("total %.1f MMAC = %.2f GOPs" % (tot / 1e6, 2 * tot / 1e9))
print("largest activation pair (in+out) %.0f KB int8" % (peak / 1024))
for r in sorted(rows, reverse=True)[:8]: print("%7.1f MMAC %-34s in %-15s out %-15s k %s g %d" % (r[0] / 1e6, r[1], r[2], r[3], r[4], r[5]))
