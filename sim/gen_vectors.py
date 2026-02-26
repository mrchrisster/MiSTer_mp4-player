#!/usr/bin/env python3
"""
gen_vectors.py — generate YUV→RGB565 test vectors for tb_yuv_to_rgb_stream.v

Usage:
  python3 sim/gen_vectors.py                   # 5 synthetic test patterns
  python3 sim/gen_vectors.py video.mp4         # 5 real frames from video (needs ffmpeg)

Output:
  sim/video_vectors.txt — one line per pixel: YY UU VV EEEE (hex)
"""

import sys, os, random, subprocess

OUT_FILE = os.path.join(os.path.dirname(__file__), "video_vectors.txt")

# ── BT.601 reference (must match yuv_to_rgb.sv integer arithmetic exactly) ────

def bt601(y, u, v):
    c, d, e = y - 16, u - 128, v - 128
    r = max(0, min(255, (298*c + 409*e         + 128) >> 8))
    g = max(0, min(255, (298*c - 100*d - 208*e + 128) >> 8))
    b = max(0, min(255, (298*c + 516*d         + 128) >> 8))
    return r, g, b

def rgb565(r, g, b):
    """Standard RGB565: R[15:11] G[10:5] B[4:0]"""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def rbg565_wrong(r, g, b):
    """OLD wrong packing: R[15:11] B[10:5] G[4:0]"""
    return ((r & 0xF8) << 8) | ((b & 0xFC) << 3) | (g >> 3)

# ── Synthetic test patterns ────────────────────────────────────────────────────

def synthetic_frames():
    vecs = []

    # Frame 1: Luminance ramp — Y 16→235, neutral chroma
    for y in range(16, 236, 5):
        vecs.append((y, 128, 128))

    # Frame 2: U (Cb) chroma sweep — Y=128, V=128
    for u in range(16, 241, 5):
        vecs.append((128, u, 128))

    # Frame 3: V (Cr) chroma sweep — Y=128, U=128
    for v in range(16, 241, 5):
        vecs.append((128, 128, v))

    # Frame 4: Standard BT.601 colour bars repeated
    bars = [
        (235, 128, 128),  # White
        (210,  16, 146),  # Yellow
        (170, 166,  16),  # Cyan
        (145,  54,  34),  # Green
        (106, 202, 222),  # Magenta
        ( 81,  90, 240),  # Red
        ( 41, 240, 110),  # Blue
        ( 16, 128, 128),  # Black
    ]
    for _ in range(16):
        vecs.extend(bars)

    # Frame 5: Deterministic pseudo-random values
    rng = random.Random(0xDEADBEEF)
    for _ in range(200):
        vecs.append((rng.randint(16, 235), rng.randint(16, 240), rng.randint(16, 240)))

    return vecs

# ── Real video frames via ffmpeg ───────────────────────────────────────────────

def video_frames(path, w=16, h=16, num=5):
    """Decode num frames from path at w×h using ffmpeg, return list of (Y,U,V)."""
    cmd = [
        "ffmpeg", "-loglevel", "quiet",
        "-i", path,
        "-vf", f"scale={w}:{h}",
        "-frames:v", str(num),
        "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"
    ]
    data = subprocess.check_output(cmd)
    fsize = w * h + (w // 2) * (h // 2) * 2
    if len(data) < fsize * num:
        raise ValueError(f"Got {len(data)} B, expected {fsize*num} B")

    vecs = []
    for f in range(num):
        base   = f * fsize
        y_pl   = data[base                          : base + w*h]
        u_pl   = data[base + w*h                    : base + w*h + (w//2)*(h//2)]
        v_pl   = data[base + w*h + (w//2)*(h//2)   : base + fsize]
        for row in range(h):
            for col in range(w):
                yi = y_pl[row * w + col]
                ui = u_pl[(row // 2) * (w // 2) + (col // 2)]
                vi = v_pl[(row // 2) * (w // 2) + (col // 2)]
                vecs.append((yi, ui, vi))
    return vecs

# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
        print(f"Decoding 5 frames from {path} at 16×16 ...")
        try:
            vecs = video_frames(path)
            print(f"Decoded {len(vecs)} pixels from video.")
        except Exception as e:
            print(f"ffmpeg failed ({e}). Falling back to synthetic patterns.")
            vecs = synthetic_frames()
    else:
        print("Generating synthetic 5-frame test patterns ...")
        vecs = synthetic_frames()

    # Count pixels where correct RGB565 ≠ old wrong RBG565
    differ = sum(
        1 for y,u,v in vecs
        if rgb565(*bt601(y,u,v)) != rbg565_wrong(*bt601(y,u,v))
    )

    os.makedirs(os.path.dirname(OUT_FILE) or ".", exist_ok=True)
    with open(OUT_FILE, "w") as f:
        for y, u, v in vecs:
            r, g, b = bt601(y, u, v)
            exp = rgb565(r, g, b)
            f.write(f"{y:02x} {u:02x} {v:02x} {exp:04x}\n")

    print(f"Wrote {len(vecs)} vectors -> {OUT_FILE}")
    print(f"  {differ}/{len(vecs)} pixels differ between correct RGB565 and old wrong RBG565")
    print(f"  (failing those would confirm the B/G swap was real)")

if __name__ == "__main__":
    main()
