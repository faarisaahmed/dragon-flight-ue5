#!/usr/bin/env python3
"""Widen the Night Fury's pupils: the calm, friendly round-ish pupil instead of the hunting slit.

Data/Dragon/Fulo/Textures/T_Fulo_Eye_D.png (from export_fulo.py) is kept as T_Fulo_Eye_Slit_D.png
and rewritten with the iris pattern warped outward from the pupil, so the pupil grows into a
broad oval and the iris fibres stay continuous. Run after export_fulo.py:

    python3 Tools/DragonModel/calm_eyes.py [width_scale] [height_scale]
"""
import os, shutil, sys
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEX = os.path.join(ROOT, 'Data', 'Dragon', 'Fulo', 'Textures')
src = os.path.join(TEX, 'T_Fulo_Eye_Slit_D.png')
dst = os.path.join(TEX, 'T_Fulo_Eye_D.png')
if not os.path.exists(src):
    shutil.copy(dst, src)
WS = float(sys.argv[1]) if len(sys.argv) > 1 else 2.6   # pupil width x
HS = float(sys.argv[2]) if len(sys.argv) > 2 else 1.12  # pupil height x

im = np.asarray(Image.open(src).convert('RGBA')).astype(np.float32)
H, W = im.shape[:2]
lum = im[..., :3].mean(-1)
ys, xs = np.where(lum < 20)
cx, cy = xs.mean(), ys.mean()
R = 0.39 * W   # iris radius

def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)

yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
dx, dy = xx - cx, yy - cy
r = np.sqrt(dx * dx + dy * dy) / R
sx = 1 / WS + (1 - 1 / WS) * smooth(0.22, 0.85, r)
sy = 1 / HS + (1 - 1 / HS) * smooth(0.35, 0.85, r)
u, v = cx + dx * sx, cy + dy * sy

# bilinear sample
u0 = np.clip(np.floor(u).astype(int), 0, W - 2); v0 = np.clip(np.floor(v).astype(int), 0, H - 2)
fu = (u - u0)[..., None]; fv = (v - v0)[..., None]
out = (im[v0, u0] * (1 - fu) * (1 - fv) + im[v0, u0 + 1] * fu * (1 - fv)
       + im[v0 + 1, u0] * (1 - fu) * fv + im[v0 + 1, u0 + 1] * fu * fv)
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), 'RGBA').save(dst)
print('wrote', dst, 'pupil centre', (round(cx), round(cy)))
