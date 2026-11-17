"""Find little-endian float32 constants (within 1e-4) in main's image: findf.py VALUE..."""

import os
import sys
from pathlib import Path

import numpy as np

sys.path[:0] = ["recomp", "tools/gpu"]
from module import Module

m = Module("main", Path(os.environ.get("MKW_GAME", "game")) / "exefs/main", 0, simd="c")
img = bytes(m.image)

a = np.frombuffer(img[: len(img) // 4 * 4], dtype="<f4")
for v in map(float, sys.argv[1:]):
    hits = np.nonzero(np.abs(a - v) < 1e-4)[0][:20]
    print(v, [hex(int(h) * 4) for h in hits])
