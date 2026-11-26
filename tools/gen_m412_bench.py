# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes tests/m4/bench/m412-heap-index.txt (the M4.12 bench: shaders that
pick resources from the view heap by index).

Usage: python -I tools/gen_m412_bench.py <out.txt>
"""
import sys

BUFFERS, VIEWS, TEXTURES, FRAMES, DRAW_FRAMES = 32, 4096, 128, 200, 100

out = [
    "# M4.12 shaders that pick resources from the view heap by index (time",
    "# per call). 4,096 raw buffer views of 32 buffers and 128 textures the",
    "# title fills in the heap. A dispatch cycle (reset, heaps, a shader that",
    "# picks a buffer by index, close, execute, wait): the frame's first (the",
    "# pass over every buffer view, the texture pass uploading 128 textures),",
    "# 1,000 more in that frame (the pass once a frame), one in each of 200",
    "# frames (a 64th of the views looked at, the changed ones), dispatches",
    "# recorded in one list; then draw cycles of a pipeline that picks",
    "# nothing: the first of each of 100 frames (the early texture pass) and",
    "# 1,000 in one frame. Written by tools/gen_m412_bench.py.",
    "runtime",
    "shader_cache c:/tmp/m4-shader-cache c:/tmp/m4-shader-cache/build.bin",
    "create_device dev device11",
    "create_queue dev q 0",
    "create_fence dev f 0",
    "allocator dev a 0",
    "list dev l 0 a",
    "list_close l",
    "set_interval dev 1000",
    "desc_heap dev views 0 8192 flags=1",
    "desc_heap dev cpu 0 4096",
    "desc_heap dev rtvs 2 8",
    "committed dev out 1 B65536",
    "committed dev rt 1 T3/64x64/f28/F1",
    "rtv dev rtvs:0 rt",
    "fill out 4096 0 0",
]
for i in range(BUFFERS):
    out += [f"committed dev b{i} 1 B65536", f"fill b{i} 1024 {i + 1} {i}",
            f"srv dev cpu:{i} b{i} w:39,1,5768,0,0,0,256,0,1,0"]
n = BUFFERS
while n < VIEWS:
    out.append(f"desc_copy1 dev {n} cpu:{n} cpu:0 0")
    n *= 2
out.append(f"desc_copy1 dev {VIEWS} views:0 cpu:0 0")
for i in range(TEXTURES):
    out += [f"committed dev t{i} 1 T3/64x64/f28", f"fill t{i} 16384 {i % 7 + 1} {i}", f"srv dev views:{VIEWS + i} t{i}"]
out += [
    "cs_heap ch",
    "compute_pipeline dev ph ch",
    "vs_fullscreen vs",
    "ps_color ps 1.0 0.5 0.25 1.0",
    "graphics_pipeline dev g vs ps 28",
    "list dev lr 0 a",
    "list_close lr",
    "quiet 1",
    "frame_wait dev 0 1000 0",
    "dispatch_cycle q l a f views ph out 1",
    "repeat 1000 dispatch_cycle q l a f views ph out 1",
]
for _ in range(FRAMES):
    out += ["frame_wait dev 0 1000 0", "dispatch_cycle q l a f views ph out 1"]
out += [
    "list_reset lr a",
    "set_heaps lr views",
    "set_pipeline lr ph",
    "compute_uav lr 0 out",
    "repeat 20000 dispatch lr 1 1 1",
    "list_close lr",
]
for _ in range(DRAW_FRAMES):
    out += ["frame_wait dev 0 1000 0", "draw_cycle q l a f views g rtvs:0 64 1"]
out += ["repeat 1000 draw_cycle q l a f views g rtvs:0 64 1", "end"]
open(sys.argv[1], "w", encoding="utf-8").write("\n".join(out) + "\n")
