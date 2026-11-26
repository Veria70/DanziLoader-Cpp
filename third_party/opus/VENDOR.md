# libopus 1.6.1, vendored

- Source: https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
- SHA-256: 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
  (as listed in the release folder's SHA256SUMS.txt)
- Licence: BSD-3-Clause (`COPYING`); authors in `AUTHORS`.

Only what the default float build of the decoder and encoder library needs
is kept, unchanged:

- `celt/`, `silk/`, `src/`, `include/`, `cmake/`;
- `CMakeLists.txt`, the `*_sources.mk` / `*_headers.mk` lists,
  `Makefile.am`, `configure.ac`, `package_version`;
- the `dnn/` headers.

The release's DNN weight data, its documentation, its tests and its autotools
files are left out. The port builds the library with `OPUS_DRED`,
`OPUS_OSCE` and the DNN features off (`CMakeLists.txt`), and uses only the
decoder (`src/hle/apu.cpp`).
