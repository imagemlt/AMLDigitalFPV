# AMLDigitalFPV（中文）

[English README](README.md)

AMLDigitalFPV 是面向 OpenIPC 生态的 Amlogic 盒子地面端接收程序，目标硬件为价格低廉且解码性能强的消费级盒子（如 S905L3A/S905X2）。实现思路参考了 CoreELEC、moonlight-embedded 以及 [pixelpilot_rk](https://github.com/openipc/pixelpilot_rk)。

- 视频：GStreamer + libamcodec
- 可选音频：RTP（payload 98，Opus）输出到 PulseAudio（pa_simple），为降低延迟不做音画同步。

## 依赖
- CoreELEC 交叉编译工具链 `armv8a-libreelec-linux-gnueabihf-`，sysroot 默认在 `toolchain/armv8a-libreelec-linux-gnueabihf/sysroot`。
- sysroot 内已有 GStreamer、glib、spdlog、fmt（或由 spdlog 自带）、libamcodec、libopus、PulseAudio（libpulse-simple）等库。

## 编译
### 推荐：CMake
```bash
cmake -B build -S . \
  -DCMAKE_SYSROOT=${SYSROOT_PREFIX} \
  -DCMAKE_C_COMPILER=${CC} \
  -DCMAKE_CXX_COMPILER=${CXX}
cmake --build build -j$(nproc)
# 可选安装：
cmake --install build --prefix=/tmp/stage
```

### 传统 make
```bash
make            # 生成 AMLDigitalFPV
make clean      # 清理 build/ 与可执行文件
```
编译参数可在 `CMakeLists.txt` 或 `Makefile` 中调整。产物在 `build/`，最终可执行文件 `AMLDigitalFPV` 位于仓库根目录。

## 目录说明
- `src/`：核心源码（main.cpp, gstrtpreceiver.cpp, aml.c, util.c 等）。
- `CMakeLists.txt` / `Makefile`：构建脚本。
- `systemd/`：amldigitalfpv.service 单元。

## 运行参数
`AMLDigitalFPV` 支持以下命令行参数：

| 参数 | 默认值 | 说明 |
| ---- | ------ | ---- |
| `-w <width>`  | `1920` | 传递给 `aml_setup` 的期望视频宽度，同时写入录像文件元数据。 |
| `-h <height>` | `1080` | 期望视频高度。 |
| `-p <fps>`    | `120`  | 期望帧率，也用于 DVR 计算时间戳。 |
| `-s <path>`   | *(空)* | 覆盖录像输出位置：指向目录时会自动递增命名；指定 `.mp4` 文件时则写入该文件。未设置则会依次尝试 `/var/media`、`/media`、`/run/media`、`/storage`。 |
| `-f <path>`   | `1` | 视频输出路径：`1` = AMVIDEO，`0` = AMLVIDEO→AMVIDEO（v4l2 管线）。 |
| `-t <type>`   | `0` | 编码类型：`0` = H265，`1` = H264。 |
| `-d <mode>`   | `0` | 码流类型：`0` = frame，`1` = ES video。 |
| `-l <level>`  | `4` | 解码缓冲等级（传给 `aml_setup`）。 |
| `-a <0|1>`    | `0` | 启用音频：`1` 时使用 appsrc UDP 读包并解码 Opus payload `98`。 |

录像默认关闭，需要向 UDP 端口 `5612` 发送指令：
- `record=1`：开始录制。
- `record=0`：停止录制并关闭文件。
- `sound=1`：开启 RTP 音频（payload 98）。
- `sound=0`：关闭 RTP 音频。

## 音频 RTP
音频默认关闭。可通过启动参数 `-a 1` 或向 UDP `5612` 发送 `sound=1` 开启。payload `98` 的 Opus 会解码后输出到 PulseAudio（pa_simple），不做音画同步以降低延迟。

## 其他
- 工具链/sysroot 路径变化时需要同步更新构建配置。
- 缺库时请在 CoreELEC sysroot 内安装。
- 通过 package.mk 已加入 `enable_service amldigitalfpv.service`，可在系统中启用服务。
