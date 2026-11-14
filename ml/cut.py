import onnx, sys
from onnx import helper, shape_inference
src, dst, H, W = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
outs = ["/model.24/m.0/Conv_output_0", "/model.24/m.1/Conv_output_0", "/model.24/m.2/Conv_output_0"]
onnx.utils.extract_model(src, dst, ["images"], outs)
m = onnx.load(dst)
d = m.graph.input[0].type.tensor_type.shape.dim
d[2].dim_value, d[3].dim_value = H, W
for o in m.graph.output:
    o.type.tensor_type.ClearField("shape")
del m.graph.value_info[:]
m = shape_inference.infer_shapes(m)
onnx.checker.check_model(m)
onnx.save(m, dst)
for o in m.graph.output: print(o.name, [x.dim_value for x in o.type.tensor_type.shape.dim])
