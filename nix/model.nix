# YOLOv5n (ultralytics v7.0, AGPL-3.0) -> int8 NPU program yolov5n.nnp
{ pkgs, sources, src }:
let
  py = pkgs.python3.withPackages (p: with p; [ numpy onnx onnxruntime pillow ]);
in
pkgs.runCommand "yolov5n-nnp" { nativeBuildInputs = [ py ]; } ''
  cp -r ${src}/ml work; chmod -R u+w work; cd work
  cp ${sources.yolov5n} yolov5n.onnx
  python cut.py yolov5n.onnx yolov5n-192x320.onnx 192 320
  python to_fp32.py yolov5n-192x320.onnx yolov5n-192x320-fp32.onnx
  python compiler.py yolov5n.nnp
  install -D -m644 yolov5n.nnp $out/yolov5n.nnp
''
