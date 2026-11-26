# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes tests/m4/bench/m414-memory.txt (the M4.14 bench: title memory by
address). Usage: python -I tools/gen_m414_bench.py <out.txt>
"""
import sys

FRAMES = 100
out = [
    "# M4.14 title memory by address (time per call). 64 MiB of title memory",
    "# at a console address; a shader that loads through an address in two",
    "# root constants. First 16 copies are numbered (an address in each 4 MiB",
    "# window). Then per frame a list whose first memory dispatch brings all",
    "# numbered copies up to date and reads the faults, and 20,000 dispatches",
    "# recorded in one list (the copies' refresh once a list). A plain",
    "# dispatch (root UAV) beside them. Written by tools/gen_m414_bench.py.",
    "runtime",
    "shader_cache c:/tmp/m4-shader-cache c:/tmp/m4-shader-cache/build.bin",
    "create_device dev device11", "create_queue dev q 0", "create_fence dev f 0", "allocator dev a 0",
    "list dev l 0 a", "list_close l", "set_interval dev 1000",
    "desc_heap dev views 0 4096 flags=1",
    "committed dev out 1 B65536",
    "title_memory mem 67108864 0x1234000000",
    "fill mem 1024 3 1",
    "cs_memory cm", "cs_store cs1 0x12345678",
    "compute_pipeline dev pm cm", "compute_pipeline dev p1 cs1",
    "frame_wait dev 0 1000 0",
    "list_reset l a", "set_heaps l views", "set_pipeline l pm", "compute_uav l 0 out",
]
for k in range(16):
    out += [f"compute_address l 1 mem+{k * 4194304}", "dispatch l 1 1 1"]
out += ["list_close l", "execute q l", "queue_signal q f 1", "fence_event f 1 null", "quiet 1"]
value = 2
for _ in range(FRAMES):
    out += ["frame_wait dev 0 1000 0", "list_reset l a", "set_heaps l views", "set_pipeline l pm", "compute_uav l 0 out",
            "compute_address l 1 mem", "dispatch l 1 1 1", "dispatch l 1 1 1", "list_close l", "execute q l",
            f"queue_signal q f {value}", f"fence_event f {value} null"]
    value += 1
out += ["list dev lr 0 a", "set_heaps lr views", "set_pipeline lr pm", "compute_uav lr 0 out", "compute_address lr 1 mem",
        "repeat 20000 dispatch lr 1 1 1", "set_pipeline lr p1", "repeat 20000 dispatch lr 1 1 1", "end"]
open(sys.argv[1], "w", encoding="utf-8").write("\n".join(out) + "\n")
