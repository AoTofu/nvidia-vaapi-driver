#!/usr/bin/env python3
"""Generate an owned deterministic seek fixture with identical top/bottom IDs."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('output')
p.add_argument('--width', type=int, default=1920)
p.add_argument('--height', type=int, default=1080)
p.add_argument('--codec', choices=['vp9', 'av1', 'h264'], default='vp9')
p.add_argument('--fps', type=int, default=30)
p.add_argument('--frames', type=int, default=360)
a = p.parse_args()
w, h, fps, count = a.width, a.height, a.fps, a.frames
if w < 256 or h < 96 or w % 2 or h % 2 or not 1 <= fps <= 120 or not 31 <= count <= 512:
    p.error('Use even dimensions >=256x96, FPS 1..120 and 31..512 frames (9-bit IDs)')
manifest = Path(a.output + '.json')
if manifest.exists():
    p.error(f'Manifest already exists: {manifest}')
encoder = {
    'vp9': ['-c:v', 'libvpx-vp9', '-deadline', 'realtime', '-cpu-used', '6', '-crf', '24', '-b:v', '0'],
    'av1': ['-c:v', 'libaom-av1', '-cpu-used', '8', '-crf', '0', '-b:v', '0', '-row-mt', '1'],
    'h264': ['-c:v', 'libx264', '-preset', 'fast', '-crf', '18'],
}[a.codec]
bands = sorted(set([*range(16, h - 48, 128), h - 48]))
cmd = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin', '-n', '-f', 'rawvideo',
       '-pixel_format', 'rgb24', '-video_size', f'{w}x{h}', '-framerate', str(fps), '-i', 'pipe:0',
       *encoder, '-g', '60', '-pix_fmt', 'yuv420p', a.output]
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
try:
    for n in range(count):
        frame = bytearray(bytes([40, 70, 100]) * (w * h))
        x = (n * 13) % (w - 128)
        row = bytes([220, 100, 30]) * 128
        for y in range(max(0, h // 2 - 64), min(h, h // 2 + 64)):
            offset = (y * w + x) * 3
            frame[offset:offset + len(row)] = row
        # Repeat IDs down the image to detect splits between interior bands too.
        for bit in range(9):
            color = 235 if (n >> bit) & 1 else 16
            row = bytes([color, color, color]) * 24
            for y0 in bands:
                for y in range(y0, y0 + 32):
                    offset = (y * w + 32 + bit * 24) * 3
                    frame[offset:offset + len(row)] = row
        proc.stdin.write(frame)
finally:
    proc.stdin.close()
if proc.wait() != 0:
    raise SystemExit('Video encoding failed')

# Validate the encoded fixture before interpreting browser pixel mismatches.
# Lossy encoding can change binary marker bits even with valid source pixels.
decoder = subprocess.Popen(['ffmpeg', '-v', 'error', '-i', a.output,
    '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'], stdout=subprocess.PIPE)
bad, frames = [], 0
while True:
    frame = decoder.stdout.read(w * h * 3)
    if not frame:
        break
    if len(frame) != w * h * 3:
        raise SystemExit('Truncated decoded frame')
    ids = [sum((frame[(y * w + 44 + bit * 24) * 3] > 128) << bit
               for bit in range(9)) for y in (band + 16 for band in bands)]
    if ids != [frames] * len(bands):
        bad.append((frames, *ids))
    frames += 1
if decoder.wait() or frames != count or bad:
    raise SystemExit(f'Fixture validation failed: frames={frames}, bad={bad[:10]}')
with open(a.output, 'rb') as encoded:
    digest = hashlib.file_digest(encoded, 'sha256').hexdigest()
manifest.write_text(json.dumps(dict(width=w, height=h, fps=fps, frames=frames,
    markerRows=[band + 16 for band in bands], sha256=digest), indent=2) + '\n')
print(f'Validated {frames} encoded frames and {len(bands)} frame-ID bands; {manifest}')
