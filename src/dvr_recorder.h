#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <filesystem>

#include "concurrentqueue/blockingconcurrentqueue.h"
#include "gstrtpreceiver.h"
#include "frame_ring_buffer.h"

struct MP4E_mux_tag;
struct mp4_h26x_writer_tag;

class DvrRecorder {
public:
    DvrRecorder();
    ~DvrRecorder();

    void set_video_params(uint32_t width, uint32_t height, uint32_t fps_hint, VideoCodec codec);
    void set_override_path(const std::string &path);
    void update_frame_rate(double fps);

    void enqueue_frame(const std::shared_ptr<std::vector<uint8_t>> &frame);

    void start_recording();
    void stop_recording();
    void shutdown();

    bool is_recording() const { return recording_.load(); }
    bool rotation_requested() const { return rotate_requested_.load(); }
    void clear_rotation_request() { rotate_requested_.store(false); }

private:
    bool rotate_recording_file();
    enum class CommandType {
        Start,
        Stop,
        Frame,
        Shutdown
    };

    struct Command {
        CommandType type;
        std::shared_ptr<std::vector<uint8_t>> frame;
    };

    void worker_loop();
    void push_command(const Command &cmd);
    void reset_warmup_state();

    bool open_writer(bool mark_recording);
    void close_writer(bool clear_recording);
    bool ready_to_write() const;
    std::filesystem::path resolve_target_file();
    std::filesystem::path find_default_media_root() const;
    std::filesystem::path make_incremental_filename(const std::filesystem::path &dir) const;

    static int file_write_callback(int64_t offset, const void *buffer, size_t size, void *token);

    moodycamel::BlockingConcurrentQueue<Command> queue_;
    FrameRingBuffer ring_buffer_{120};
    std::atomic<bool> warmup_done_{false};
    uint64_t warmup_start_ms_{0};
    size_t warmup_frame_count_{0};
    std::thread worker_;
    std::atomic<bool> running_{true};

    mutable std::mutex state_mutex_;
    uint32_t video_width_{0};
    uint32_t video_height_{0};
    uint32_t video_fps_hint_{0};
    std::atomic<uint32_t> frame_duration_{1500};
    VideoCodec codec_{VideoCodec::H265};
    std::filesystem::path override_path_;
    std::filesystem::path current_path_;

    std::atomic<bool> recording_{false};
    bool writer_ready_{false};

    FILE *file_{nullptr};
    MP4E_mux_tag *mux_{nullptr};
    mp4_h26x_writer_tag *writer_{nullptr};
    std::atomic<uint64_t> current_file_bytes_{0};
    std::atomic<bool> rotate_requested_{false};
    std::atomic<bool> stop_requested_{false};
    static constexpr uint64_t kMaxFat32Size = (4ULL * 1024 * 1024 * 1024) - (64ULL * 1024);
};
