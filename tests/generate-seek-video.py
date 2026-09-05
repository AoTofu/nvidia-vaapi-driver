#!/usr/bin/env python3
"""Generate an owned deterministic seek fixture with identical top/bottom IDs."""
import argparse
import subprocess

p = argparse.ArgumentParser()
p.add_argument('output')
p.add_argument('--width', type=int, default=1920)
p.add_argument('--height', type=int, default=1080)
p.add_argument('--codec', choices=['vp9', 'av1'], default='vp9')
a = p.parse_args()
w, h, fps, count = a.width, a.height, 30, 360
encoder = ['-c:v', 'libvpx-vp9', '-deadline', 'realtime', '-cpu-used', '6', '-crf', '24', '-b:v', '0'] if a.codec == 'vp9' else ['-c:v', 'libaom-av1', '-cpu-used', '8', '-crf', '0', '-b:v', '0', '-row-mt', '1']
cmd = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin', '-n', '-f', 'rawvideo',
       '-pixel_format', 'rgb24', '-video_size', f'{w}x{h}', '-framerate', str(fps), '-i', 'pipe:0',
       *encoder, '-g', '60', '-pix_fmt', 'yuv420p', a.output]
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
try:
    for n in range(count):
        frame = bytearray(bytes([40, 70, 100]) * (w * h))
        # Nine bits encode frame IDs 0..359. Sample the centers after decoding.
        for bit in range(9):
            color = 235 if (n >> bit) & 1 else 16
            row = bytes([color, color, color]) * 24
            for y0 in (16, h - 48):
                for y in range(y0, y0 + 32):
                    offset = (y * w + 32 + bit * 24) * 3
                    frame[offset:offset + len(row)] = row
        x = (n * 13) % (w - 128)
        row = bytes([220, 100, 30]) * 128
        for y in range(h // 2 - 64, h // 2 + 64):
            offset = (y * w + x) * 3
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
               for bit in range(9)) for y in (32, h - 32)]
    if ids != [frames, frames]:
        bad.append((frames, *ids))
    frames += 1
if decoder.wait() or frames != count or bad:
    raise SystemExit(f'Fixture validation failed: frames={frames}, bad={bad[:10]}')
print(f'Validated {frames} encoded frames and both frame-ID bands')
