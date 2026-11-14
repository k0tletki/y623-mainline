# host reference: cut YOLOv5n (3 head outputs) -> boxes, letterbox in, NMS out
import sys, numpy as np, onnxruntime as ort
from PIL import Image, ImageDraw
H, W = 192, 320
ANCH = np.array([[10,13, 16,30, 33,23], [30,61, 62,45, 59,119], [116,90, 156,198, 373,326]], np.float32).reshape(3, 3, 2)
STRIDE = [8, 16, 32]
COCO = "person bicycle car motorcycle airplane bus train truck boat traffic-light fire-hydrant stop-sign parking-meter bench bird cat dog horse sheep cow elephant bear zebra giraffe backpack umbrella handbag tie suitcase frisbee skis snowboard sports-ball kite baseball-bat baseball-glove skateboard surfboard tennis-racket bottle wine-glass cup fork knife spoon bowl banana apple sandwich orange broccoli carrot hot-dog pizza donut cake chair couch potted-plant bed dining-table toilet tv laptop mouse remote keyboard cell-phone microwave oven toaster sink refrigerator book clock vase scissors teddy-bear hair-drier toothbrush".split()

def letterbox(img):
    r = min(W / img.width, H / img.height)
    nw, nh = round(img.width * r), round(img.height * r)
    canvas = Image.new("RGB", (W, H), (114, 114, 114))
    dx, dy = (W - nw) // 2, (H - nh) // 2
    canvas.paste(img.resize((nw, nh), Image.BILINEAR), (dx, dy))
    return canvas, r, dx, dy

def decode(outs, conf=0.25, iou=0.45):
    boxes = []
    for i, o in enumerate(outs):            # o: [1, 255, gh, gw]
        _, _, gh, gw = o.shape
        o = 1 / (1 + np.exp(-o.reshape(3, 85, gh, gw).transpose(0, 2, 3, 1)))  # [3,gh,gw,85]
        gy, gx = np.mgrid[0:gh, 0:gw]
        xy = (o[..., :2] * 2 - 0.5 + np.stack([gx, gy], -1)) * STRIDE[i]
        wh = (o[..., 2:4] * 2) ** 2 * ANCH[i][:, None, None, :]
        score = o[..., 4:5] * o[..., 5:]
        cls = score.argmax(-1); sc = score.max(-1)
        m = sc > conf
        for a, y, x in zip(*np.nonzero(m)):
            cx, cy = xy[a, y, x]; w, h = wh[a, y, x]
            boxes.append((sc[a, y, x], cls[a, y, x], cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2))
    boxes.sort(key=lambda b: -b[0]); keep = []
    for b in boxes:
        ok = True
        for k in keep:
            if k[1] != b[1]: continue
            ix = max(0, min(b[4], k[4]) - max(b[2], k[2])); iy = max(0, min(b[5], k[5]) - max(b[3], k[3]))
            inter = ix * iy; u = (b[4]-b[2])*(b[5]-b[3]) + (k[4]-k[2])*(k[5]-k[3]) - inter
            if inter / u > iou: ok = False; break
        if ok: keep.append(b)
    return keep

if __name__ == "__main__":
    model, img_path, out_path = sys.argv[1:4]
    img = Image.open(img_path).convert("RGB")
    lb, r, dx, dy = letterbox(img)
    x = (np.asarray(lb, np.float32) / 255).transpose(2, 0, 1)[None]
    sess = ort.InferenceSession(model, providers=["CPUExecutionProvider"])
    outs = sess.run(None, {"images": x})
    dets = decode(outs)
    d = ImageDraw.Draw(lb)
    for sc, c, x0, y0, x1, y1 in dets:
        print("%-10s %.2f  [%.0f %.0f %.0f %.0f]" % (COCO[c], sc, x0, y0, x1, y1))
        d.rectangle([x0, y0, x1, y1], outline=(255, 0, 0)); d.text((x0 + 2, y0 + 1), COCO[c], fill=(255, 255, 0))
    lb.save(out_path)
