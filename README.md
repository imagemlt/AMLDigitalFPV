# AMLDigitalFPV

[简体中文说明](README_zhCN.md)

AMLDigitalFPV is an Amlogic-based ground FPV receiver for the OpenIPC ecosystem. It targets cheap consumer Amlogic boxes (e.g., S905L3A/S905X2) with strong hardware decode. The design borrows ideas from CoreELEC, moonlight-embedded, and [pixelpilot_rk](https://github.com/openipc/pixelpilot_rk).

- Video: GStreamer + libamcodec
- Optional audio: RTP (payload 98, Opus) straight to ALSA, no A/V sync for lowest latency.

## Prerequisites
- CoreELEC toolchain (`armv8a-libreelec-linux-gnueabihf-`) and sysroot (`toolchain/armv8a-libreelec-linux-gnueabihf/sysroot`).
- GStreamer, glib, spdlog, fmt (or bundled via spdlog), libamcodec present in sysroot.

## Build
### CMake (recommended)
```bash
cmake -B build -S . \
  -DCMAKE_SYSROOT=${SYSROOT_PREFIX} \
  -DCMAKE_C_COMPILER=${CC} \
  -DCMAKE_CXX_COMPILER=${CXX}
cmake --build build -j$(nproc)
# optional install:
cmake --install build --prefix=/tmp/stage
```

### Legacy make
```bash
make            # builds AMLDigitalFPV
make clean      # removes build/ and binary
```
Edit `CMakeLists.txt` or `Makefile` to tweak sources/flags. Outputs live in `build/`, final binary `AMLDigitalFPV` in repo root.

## Source Layout
- `src/`: main.cpp, gstrtpreceiver.cpp, aml.c, util.c, etc.
- `CMakeLists.txt` / `Makefile`: build scripts.
- `systemd/`: amldigitalfpv.service unit.

## Audio RTP
Call `enable_audio_stream(port, 98, "default")` before `start_receiving()` to enable RTP audio (assumes Opus). Audio goes to `alsasink` without A/V sync to minimize latency.

## Notes
- Update toolchain/sysroot paths if your CoreELEC tree moves.
- Missing libs? install into the CoreELEC sysroot.
- Service can be installed/enabled via package.mk with `enable_service amldigitalfpv.service`.
