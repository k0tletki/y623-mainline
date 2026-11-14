import onnx, sys, numpy as np
from onnx import numpy_helper, TensorProto
m = onnx.load(sys.argv[1]); g = m.graph
def fix(t):
    if t.data_type == TensorProto.FLOAT16:
        a = numpy_helper.to_array(t).astype(np.float32)
        t.CopyFrom(numpy_helper.from_array(a, t.name))
n16 = 0
for t in g.initializer:
    n16 += t.data_type == TensorProto.FLOAT16; fix(t)
for n in g.node:
    for a in n.attribute:
        if a.type == onnx.AttributeProto.TENSOR: fix(a.t)
        if n.op_type == "Cast" and a.name == "to" and a.i == TensorProto.FLOAT16: a.i = TensorProto.FLOAT
for v in list(g.input) + list(g.output) + list(g.value_info):
    if v.type.tensor_type.elem_type == TensorProto.FLOAT16: v.type.tensor_type.elem_type = TensorProto.FLOAT
print("fp16 initializers converted:", n16, "Cast nodes:", sum(n.op_type == "Cast" for n in g.node))
onnx.checker.check_model(m); onnx.save(m, sys.argv[2])
