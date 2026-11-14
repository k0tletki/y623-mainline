import onnx, collections, sys
m = onnx.load(sys.argv[1])
g = m.graph
print("opset", [ (o.domain, o.version) for o in m.opset_import ])
for i in g.input: print("input", i.name, [d.dim_value for d in i.type.tensor_type.shape.dim])
for o in g.output: print("output", o.name, [d.dim_value for d in o.type.tensor_type.shape.dim])
print(collections.Counter(n.op_type for n in g.node).most_common())
inits = {t.name: t for t in g.initializer}
nparams = sum(int(__import__('numpy').prod(t.dims)) for t in g.initializer)
print("params", nparams)
# the last convs before Reshape (Detect head)
for n in g.node:
    if n.op_type == "Reshape" or (n.op_type == "Conv" and any(x.name == n.output[0] for x in [])):
        pass
convs = [n for n in g.node if n.op_type == "Conv"]
for n in convs[-4:]:
    w = inits[n.input[1]]
    print("conv", n.name, n.output[0], list(w.dims))
for n in g.node:
    if n.op_type in ("Resize", "MaxPool", "Concat", "Add") and len(sys.argv) > 2:
        print(n.op_type, n.name, [a.name+"="+str(onnx.helper.get_attribute_value(a)) for a in n.attribute])
# activations: Sigmoid*Mul = SiLU
print("sigmoid->mul pairs:", sum(1 for n in g.node if n.op_type == "Mul" and any(p.op_type == "Sigmoid" and p.output[0] in n.input for p in g.node)))
