# AMLDigitalFPV

[简体中文说明](README_zhCN.md)

AMLDigitalFPV is an Amlogic-based ground FPV receiver for the OpenIPC ecosystem. It targets cheap consumer Amlogic boxes (e.g., S905L3A/S905X2) with strong hardware decode. The design borrows ideas from CoreELEC, moonlight-embedded, and [pixelpilot_rk](https://github.com/openipc/pixelpilot_rk).

- Video: GStreamer + libamcodec
- Optional audio: RTP (payload 98, Opus) to PulseAudio (pa_simple), no A/V sync for lowest latency.

## Prerequisites
- CoreELEC toolchain (`armv8a-libreelec-linux-gnueabihf-`) and sysroot (`toolchain/armv8a-libreelec-linux-gnueabihf/sysroot`).
- GStreamer, glib, spdlog, fmt (or bundled via spdlog), libamcodec, libopus, and PulseAudio (libpulse-simple) in sysroot.

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

## Runtime Options
`AMLDigitalFPV` accepts several CLI flags:

| Flag | Default | Description |
| ---- | ------- | ----------- |
| `-w <width>`  | `1920` | Expected video width used when calling `aml_setup` and when tagging recorded files. |
| `-h <height>` | `1080` | Expected video height. |
| `-p <fps>`    | `120`  | Expected frame rate (also controls DVR timestamp increments). |
| `-s <path>`   | *(empty)* | Override DVR output location: point to a directory to auto-increment files there, or specify a `.mp4` file for a fixed target. Without this flag the recorder searches `/var/media`, `/media`, `/run/media`, then `/storage`. |
| `-f <path>`   | `1` | Video output path: `1` = AMVIDEO, `0` = AMLVIDEO→AMVIDEO (v4l2 pipeline). |
| `-t <type>`   | `0` | Codec: `0` = H265, `1` = H264. |
| `-d <mode>`   | `0` | Stream type: `0` = frame, `1` = ES video. |
| `-l <level>`  | `4` | Decoder buffer level (passed to `aml_setup`). |
| `-a <0,1>`    | `0` | Enable audio: when `1`, use appsrc UDP reader and decode Opus payload `98`. |

Recording remains off until a UDP command arrives on port `5612`:
- `record=1` – start writing MP4.
- `record=0` – stop and close the file.
- `sound=1` – enable RTP audio (payload 98).
- `sound=0` – disable RTP audio.

## Audio RTP
Audio is optional and off by default. Enable it via `-a 1` at startup or by sending `sound=1` to UDP port `5612`. Opus payload `98` is decoded and sent to PulseAudio (pa_simple) without A/V sync to minimize latency.

## Notes
- Update toolchain/sysroot paths if your CoreELEC tree moves.
- Missing libs? install into the CoreELEC sysroot.
- Service can be installed/enabled via package.mk with `enable_service amldigitalfpv.service`.
