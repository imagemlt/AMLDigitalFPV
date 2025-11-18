# Amlogic-ng 视频解码与显示链路延迟因素

> 记录分析 Amlogic-ng 平台（CoreELEC/内核 4.9）上影响 H.265 解码及 `/sys/class/video` 显示路径时延的关键因素，便于调试与调优。

## 1. 解码输入侧缓冲门限

- **`/sys/module/amvdec_h265/parameters/start_decode_buf_level`**  
  默认约 `0x8000`（32768 B），要求 VIFIFO 先累计到该字节数才开始调度解码，直接决定首帧启动延迟。  
  - 可按需调小（甚至 0），首帧秒出的同时要注意弱网/低码率下可能出现“等粮”卡顿。
- **`/sys/module/amvdec_h265/parameters/pre_decode_buf_level`**  
  默认 `0x1000`（4096 B），在 stream-based 模式且尚未出第一帧时会再次检查；与 `start_decode_buf_level` 叠加使用。
- **PTS 服务配合**  
  `vdec_ready_to_run()` 还会要求 FIFO 中存在足够的 PTS（`vdec_check_rec_num_enough()`）；当 `prepare_level` 太高或输入音视频不同步时，会进一步延迟首帧。

## 2. 显示队列与节流

- **`disp_vframe_valve_level` / `run_ready_display_q_num` / `run_ready_max_buf_num`**（`/sys/module/amvdec_h265/parameters/…`）  
  默认 0（关闭）。若设置为非零，将在 `vh265_run_ready()` 中限制 display queue 内的帧数，防止过量缓冲；一旦触发，会延迟后续帧继续入队。
- **`dynamic_buf_num_margin`**  
  默认 8，用于计算 DPB 预留余量；值越大越保守。
- **`run_ready_max_vf_only_num`**  
  限制“仅用于参考”的帧数，同样会影响并行实例/低缓冲场景。

## 3. 解码内存与 MMU 缓存

- 启用 **MMU (`mmu_enable=1`)** 时，首帧需提前准备 codec_mm scatter 缓存：  
  - `decoder_mmu_box_alloc_box()` 调用 `codec_mm_scatter_mgt_delay_free_swith()`，要求后台凑齐 `need_cache_size`（4K 默认 64 MB，1080p 24 MB）。  
  - `vh265_run_ready()` 会调用 `decoder_mmu_box_sc_check()` 等待缓存就绪；内存紧张或 TVP 模式下首帧延迟会明显拉长。  
  - 关闭 MMU (`mmu_enable=0`) 可缩短首帧，但失去压缩/高分辨率支持，也可能因连续内存不足而失败。

## 4. 多实例 / V4L2 模式行为

- 当 `is_used_v4l=1` 时，缓冲和 canvas 配置会推迟到用户态提供 buffer 后进行：  
  - `init_pic_list()` 和 `config_pic()` 都有 “alloc/config canvas will be delay if work on v4l” 逻辑。  
  - 如果应用端最初没有及时 queue buffer，会导致首帧长时间黑屏。
- 需要关注 `display_q` 队列长度（`kfifo_len(&hevc->display_q)`），避免 `disp_vframe_valve_level` 等设置叠加阻塞。

## 5. `/sys/class/video` 级别的延迟开关

- **`video_delay_val`**：单位 90 kHz tick，通过 PTS 对齐显式增加播放滞后。  
- **`hold_video`**：拉高时冻结当前帧（配合 `hold_property_changed`）；常用于切换时避免闪烁，但会阻塞新帧。  
- **`enable_hdmi_delay_normal_check` & `hdmin_delay_*`**：HDMI-IN 场景用于 AV 同步，可根据 `last_required_total_delay` 主动插帧/丢帧。  
- 其它开关如 `video_seek_flag`、`show_first_frame_nosync`、`show_first_picture` 可用来调试首帧策略。

## 6. 显示回收队列与 `DISPBUF_TO_PUT_MAX`

- `video_priv.h` 定义 `DISPBUF_TO_PUT_MAX = 3`，配合 `dispbuf_to_put[]` 和 `recycle_buf[][]`，为**待归还的旧帧提供暂存**：  
  - 当 VSYNC_RDMA 等原因导致 `video_vf_put()` 暂时失败时，旧帧会塞入该数组，最多保留 3 帧。  
  - 最坏情况下会占用 3 个解码 buffer，因此推迟新帧可用时间 ≈ `3 × 帧周期`（60 fps ≈ 50 ms，50 fps ≈ 60 ms）。  
  - 改小该值可以减少峰值排队，但风险是更容易出现 `put err, que full` 与掉帧，需在 DI/Dolby/PIP 等模式下充分测试。

## 7. 其它可见指标

- **`vframe_walk_delay`**：`/sys/class/video/vframe_walk_delay` 暴露当前“帧走完队列”的耗时，可用于观测 HDMI 延迟和显示阻塞。  
- **`hdmin_delay_count_debug`** 等节点可跟踪 HDMI 延迟逻辑是否生效。  
- `video_state`、`vframe_ready_cnt`、`vframe_states` 等可辅助判断缓冲状态。

## 8. 调优建议摘要

1. 首帧特别慢时，优先检查并适度调低 `start_decode_buf_level` / `pre_decode_buf_level`，同时确认输入流稳定。  
2. 确认 `video_delay_val`、`hold_video`、`enable_hdmi_delay_normal_check` 等处于默认 0，避免显示侧人为拉长等待。  
3. 观察 `dispbuf_to_put_num` 是否长期达到 3，若是则追根排查 VSYNC_RDMA/DI 回收链路。  
4. 若 MMU 缓存初始化耗时过长，可尝试：  
   - 预加载/预热相关模块，  
   - 评估暂时关闭 double write 或减小分辨率需求，  
   - 在允许的场景下测试 `mmu_enable=0` 的首帧差异。  
5. 对于流式/直播场景，可结合 `show_first_frame_nosync`、`show_first_picture` 提前出画面，再通过播放层处理后续同步。

