# Notices

DanziLoader-Cpp is a C++20 port of DanziLoader, which is itself based on
victory-perf. It is a derivative work and is distributed under the same
licence: **GPL-3.0-or-later** (see [LICENSE](LICENSE), copied unchanged from
the reference checkout). The licence file and retained source notices identify the applicable terms.

## Upstream projects (credits as given by the reference)

- **DanziLoader**: Windows frame-pacing experiments based on
  [victory-perf at 97c3aa2](https://github.com/vitutu1/victory-perf/tree/97c3aa24263b90fa6691fca8ca88fca179593a19).
  Reference checkout used for this port: commit
  `38ea5b65121ddf3f0c33e6257b214c1ec01ef45e` (public reference used for this checkpoint).
- **[vitutu1/victory-perf](https://github.com/vitutu1/victory-perf)**
  (GPL-3.0-or-later): performance- and lighting-focused Windows compatibility
  layer.
- **[Marvisak/VIctory](https://github.com/Marvisak/VIctory)**
  (GPL-3.0-or-later): foundational PE32+ loader, runtime HLE, `d3d12_xs`
  graphics layer, BCPack/swizzle texture decoders, and the RDNA2 â†’ DXIL
  shader recompiler.
- **[fxceless/victory-fork](https://github.com/fxceless/victory-fork)**
  (GPL-3.0-or-later): world rendering, shader optimizations, card buffers,
  relay threading, in-place memory work.
- **[ZigZagBoi1/ScarlettPort](https://github.com/ZigZagBoi1/ScarlettPort)**
  (GPL-3.0-or-later): ported modules and design ideas listed in the
  reference README.
- **[ivall/xcl](https://github.com/ivall/xcl)** (GPL-2.0-only): referenced
  by upstream solely for behavioural comparison. The upstream notes no
  GPL-2.0-only code, and none is used here.

## Asset policy

The reference's asset policy (`docs/asset-policy.md` there) applies here
unchanged:

- Nothing from any game is stored in this repository.
- Tests use synthetic data that our own code generates.
- Personal game data, caches and saves live outside source control.

## The test oracle

`oracle/` compiles code taken verbatim from the reference checkout at build
time. The code is read from the reference and never copied into this
repository. That code keeps its upstream licence and authorship.

The card comparison shader in src/gpu/card_compare.inc is derived from CARD_COMPARE in the Rust reference host.rs. The shader uses the same GPL-3.0-or-later licence.

## Ported modules

- src/core/inplace.cpp — crates/victory-gpu/src/inplace.rs
- src/core/inplace.hpp — crates/victory-gpu/src/inplace.rs
- src/core/memcopy.cpp — crates/victory-gpu/src/memcopy.rs
- src/core/memcopy.hpp — crates/victory-gpu/src/memcopy.rs
- src/core/readback.cpp — crates/victory-gpu/src/readback.rs
- src/core/readback.hpp — crates/victory-gpu/src/readback.rs
- src/core/shadow.cpp — crates/victory-gpu/src/shadow.rs, host.rs
- src/core/shadow.hpp — crates/victory-gpu/src/shadow.rs, host.rs
- src/core/title_copy.cpp — crates/victory-gpu/src/host.rs
- src/core/title_copy.hpp — crates/victory-gpu/src/host.rs
- src/core/watch.cpp — crates/victory-gpu/src/watch.rs
- src/core/watch.hpp — crates/victory-gpu/src/watch.rs
- src/core/wide.hpp — crates/victory-gpu/src/shadow.rs
- src/core/windows.cpp — crates/victory-gpu/src/shadow.rs
- src/core/windows.hpp — crates/victory-gpu/src/shadow.rs
- src/d3d12x/device.cpp — crates/victory-gpu/src/device.rs, console.rs
- src/d3d12x/device.hpp — crates/victory-gpu/src/device.rs, console.rs, dxgi.rs
- src/d3d12x/dxgi.cpp — crates/victory-gpu/src/dxgi.rs
- src/d3d12x/object.cpp — crates/victory-gpu/src/object.rs
- src/d3d12x/object.hpp — crates/victory-gpu/src/object.rs
- src/gpu/card.cpp — crates/victory-gpu/src/host.rs
- src/gpu/card.hpp — crates/victory-gpu/src/host.rs, shadow.rs
- src/gpu/card_compare.inc — host.rs
- src/gpu/device.hpp — crates/victory-gpu/src/host.rs
- src/gpu/inplace_heap.cpp — crates/victory-gpu/src/inplace.rs
- src/gpu/inplace_heap.hpp — crates/victory-gpu/src/inplace.rs
- src/gpu/lookup.cpp — crates/victory-gpu/src/shadow.rs
- src/gpu/lookup.hpp — crates/victory-gpu/src/shadow.rs
- src/gpu/rebar.cpp — crates/victory-gpu/src/rebar.rs
- src/gpu/rebar.hpp — crates/victory-gpu/src/rebar.rs
