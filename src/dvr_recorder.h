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

struct MP4E_mux_tag;
struct mp4_h26x_writer_tag;

class DvrRecorder {
public:
    DvrRecorder();
    ~DvrRecorder();

    void set_video_params(uint32_t width, uint32_t height, uint32_t fps, VideoCodec codec);
    void set_override_path(const std::string &path);

    void enqueue_frame(const std::shared_ptr<std::vector<uint8_t>> &frame);

    void start_recording();
    void stop_recording();
    void shutdown();

    bool is_recording() const { return recording_.load(); }

private:
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

    bool open_writer();
    void close_writer();
    bool ready_to_write() const;
    std::filesystem::path resolve_target_file();
    std::filesystem::path find_default_media_root() const;
    std::filesystem::path make_incremental_filename(const std::filesystem::path &dir) const;

    static int file_write_callback(int64_t offset, const void *buffer, size_t size, void *token);

    moodycamel::BlockingConcurrentQueue<Command> queue_;
    std::thread worker_;
    std::atomic<bool> running_{true};

    mutable std::mutex state_mutex_;
    uint32_t video_width_{0};
    uint32_t video_height_{0};
    uint32_t video_fps_{0};
    uint32_t frame_duration_{0};
    VideoCodec codec_{VideoCodec::H265};
    std::filesystem::path override_path_;

    std::atomic<bool> recording_{false};
    bool writer_ready_{false};

    FILE *file_{nullptr};
    MP4E_mux_tag *mux_{nullptr};
    mp4_h26x_writer_tag *writer_{nullptr};
};
