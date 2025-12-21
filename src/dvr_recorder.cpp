#include "dvr_recorder.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <algorithm>

#include "spdlog/spdlog.h"

extern "C" {
#include "minimp4.h"
}
#include "scheduling_helper.hpp"

namespace fs = std::filesystem;

namespace {
constexpr uint32_t kDefaultFrameDuration = 1500; // ~60fps fallback

int64_t clamp_positive(int64_t value) {
    return value < 0 ? 0 : value;
}

uint64_t monotonic_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

DvrRecorder::DvrRecorder()
{
    writer_ = static_cast<mp4_h26x_writer_t *>(std::malloc(sizeof(mp4_h26x_writer_t)));
    if (writer_) {
        std::memset(writer_, 0, sizeof(mp4_h26x_writer_t));
    }
    worker_ = std::thread(&DvrRecorder::worker_loop, this);
}

DvrRecorder::~DvrRecorder()
{
    shutdown();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (writer_) {
        std::free(writer_);
        writer_ = nullptr;
    }
}

void DvrRecorder::set_video_params(uint32_t width, uint32_t height, uint32_t fps_hint, VideoCodec codec)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    video_width_ = width;
    video_height_ = height;
    video_fps_hint_ = fps_hint;
    codec_ = codec;
    frame_duration_.store(kDefaultFrameDuration, std::memory_order_relaxed);
}

void DvrRecorder::set_override_path(const std::string &path)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    override_path_ = fs::path(path);
}

void DvrRecorder::update_frame_rate(double fps)
{
    if (fps <= 1.0) {
        return;
    }
    const double raw = 90000.0 / fps;
    uint32_t duration = static_cast<uint32_t>(raw);
    if (duration == 0) {
        duration = kDefaultFrameDuration;
    }
    duration = std::clamp<uint32_t>(duration, 375, 90000); // clamp to 240fps..1fps
    frame_duration_.store(duration, std::memory_order_relaxed);
}

void DvrRecorder::enqueue_frame(const std::shared_ptr<std::vector<uint8_t>> &frame)
{
    if (!frame || !running_.load()) {
        return;
    }
    if (!recording_.load()) {
        return;
    }
    push_command(Command{CommandType::Frame, frame});
}

void DvrRecorder::start_recording()
{
    if (!running_.load()) {
        return;
    }
    push_command(Command{CommandType::Start, nullptr});
}

void DvrRecorder::stop_recording()
{
    if (!running_.load()) {
        return;
    }
    push_command(Command{CommandType::Stop, nullptr});
}

void DvrRecorder::shutdown()
{
    if (!running_.load()) {
        return;
    }
    push_command(Command{CommandType::Shutdown, nullptr});
}

void DvrRecorder::push_command(const Command &cmd)
{
    queue_.enqueue(cmd);
}

void DvrRecorder::worker_loop()
{
    SchedulingHelper::set_thread_params_other("DvrRecorder");
    while (true) {
        Command cmd;
        queue_.wait_dequeue(cmd);

        switch (cmd.type) {
        case CommandType::Start:
            if (!recording_.load()) {
                ring_buffer_.clear();
                reset_warmup_state();
                recording_.store(true, std::memory_order_relaxed);
                spdlog::info("DVR warm-up started");
            }
            break;
        case CommandType::Stop:
            close_writer(true);
            ring_buffer_.clear();
            reset_warmup_state();
            break;
        case CommandType::Frame:
            if (!cmd.frame) {
                break;
            }
            if (!recording_.load(std::memory_order_relaxed)) {
                break;
            }

            if (!warmup_done_.load(std::memory_order_relaxed)) {
                if (warmup_start_ms_ == 0) {
                    warmup_start_ms_ = monotonic_ms();
                    warmup_frame_count_ = 0;
                }
                ++warmup_frame_count_;
                const uint64_t elapsed = monotonic_ms() - warmup_start_ms_;
                if (elapsed >= 1000 && warmup_frame_count_ > 0) {
                    const double fps = std::max(1.0,
                                                std::ceil((warmup_frame_count_ * 1000.0) /
                                                          static_cast<double>(elapsed)));
                    if (open_writer(false)) {
                        update_frame_rate(fps);
                        warmup_done_.store(true, std::memory_order_relaxed);
                        warmup_start_ms_ = 0;
                        warmup_frame_count_ = 0;
                        spdlog::info("DVR warm-up completed, fps={:.2f}", fps);
                    } else {
                        spdlog::error("Failed to open DVR writer after warm-up");
                        recording_.store(false, std::memory_order_relaxed);
                        reset_warmup_state();
                        ring_buffer_.clear();
                    }
                }
                break;
            }

            ring_buffer_.push(cmd.frame);
            while (ready_to_write() && warmup_done_.load(std::memory_order_relaxed) && !ring_buffer_.empty()) {
                auto frame = ring_buffer_.pop();
                if (!frame)
                    break;
                auto duration = frame_duration_.load(std::memory_order_relaxed);
                if (duration == 0)
                    duration = kDefaultFrameDuration;
                const int res = mp4_h26x_write_nal(writer_,
                                                   frame->data(),
                                                   static_cast<int>(frame->size()),
                                                   static_cast<int>(duration));
                if (!(res == MP4E_STATUS_OK || res == MP4E_STATUS_BAD_ARGUMENTS)) {
                    spdlog::warn("mp4_h26x_write_nal returned {}", res);
                    stop_requested_.store(true, std::memory_order_relaxed);
                    break;
                }
                if (rotate_requested_.load(std::memory_order_relaxed)) {
                    rotate_requested_.store(false, std::memory_order_relaxed);
                    if (!rotate_recording_file()) {
                        spdlog::error("Failed to rotate DVR file");
                    }
                }
                if (stop_requested_.load(std::memory_order_relaxed)) {
                    stop_requested_.store(false, std::memory_order_relaxed);
                    spdlog::error("Stopping DVR due to write failures (disk full?)");
                    close_writer(true);
                    ring_buffer_.clear();
                    reset_warmup_state();
                    break;
                } else if (current_file_bytes_.load(std::memory_order_relaxed) >= kMaxFat32Size) {
                    if (!rotate_recording_file()) {
                        spdlog::error("Failed to rotate DVR file");
                    }
                }
            }
            break;
        case CommandType::Shutdown:
            running_.store(false);
            close_writer(true);
            ring_buffer_.clear();
            reset_warmup_state();
            break;
        }

        if (cmd.type == CommandType::Shutdown) {
            break;
        }
    }

    close_writer(true);
}

bool DvrRecorder::ready_to_write() const
{
    return recording_.load() && writer_ready_;
}

bool DvrRecorder::rotate_recording_file()
{
    if (!recording_.load()) {
        return false;
    }
    close_writer(false);
    return open_writer(false);
}

bool DvrRecorder::open_writer(bool mark_recording)
{
    if (writer_ready_) {
        return true;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_hint = 0;
    VideoCodec codec = VideoCodec::H265;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        width = video_width_;
        height = video_height_;
        fps_hint = video_fps_hint_;
        codec = codec_;
    }
    if (width == 0 || height == 0) {
        spdlog::warn("DVR cannot start: invalid video params {}x{}", width, height);
        return false;
    }

    auto output_path = resolve_target_file();
    if (output_path.empty()) {
        spdlog::warn("DVR cannot find writable RECORD directory");
        return false;
    }

    const std::string path_str = output_path.string();
    FILE *file = std::fopen(path_str.c_str(), "wb");
    if (!file) {
        spdlog::error("Failed to open DVR file {}", output_path.string());
        return false;
    }

    constexpr int kFragmentationMode = 0;
    file_ = file;
    MP4E_mux_tag *mux = MP4E_open(0, kFragmentationMode, this, file_write_callback);
    if (!mux) {
        spdlog::error("MP4E_open failed");
        file_ = nullptr;
        std::fclose(file);
        return false;
    }

    if (!writer_) {
        writer_ = static_cast<mp4_h26x_writer_t *>(std::malloc(sizeof(mp4_h26x_writer_t)));
    }
    if (!writer_) {
        spdlog::error("Failed to allocate mp4 writer");
        MP4E_close(mux);
        std::fclose(file);
        return false;
    }
    std::memset(writer_, 0, sizeof(mp4_h26x_writer_t));
    if (MP4E_STATUS_OK != mp4_h26x_write_init(writer_,
                                              mux,
                                              static_cast<int>(width),
                                              static_cast<int>(height),
                                              codec == VideoCodec::H265)) {
        spdlog::error("mp4_h26x_write_init failed");
        MP4E_close(mux);
        mux = nullptr;
        file_ = nullptr;
        std::fclose(file);
        return false;
    }

    mux_ = mux;
    writer_ready_ = true;
    current_file_bytes_.store(0, std::memory_order_relaxed);
    rotate_requested_.store(false, std::memory_order_relaxed);
    if (mark_recording) {
        recording_.store(true);
    }
    if (fps_hint > 0) {
        frame_duration_.store(std::max(1u, static_cast<uint32_t>(90000 / fps_hint)), std::memory_order_relaxed);
    }
    spdlog::info("DVR recording to {}", path_str);
    return true;
}

void DvrRecorder::close_writer(bool clear_recording)
{
    if (!writer_ready_) {
        if (clear_recording) {
            recording_.store(false, std::memory_order_relaxed);
        }
        return;
    }
    writer_ready_ = false;
    if (mux_) {
        MP4E_close(mux_);
        mux_ = nullptr;
    }
    if (writer_) {
        mp4_h26x_write_close(writer_);
    }
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    current_file_bytes_.store(0, std::memory_order_relaxed);
    rotate_requested_.store(false, std::memory_order_relaxed);
    if (clear_recording) {
        recording_.store(false);
    }
    spdlog::info("DVR stopped");
}

fs::path DvrRecorder::resolve_target_file()
{
    fs::path custom_path;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        custom_path = override_path_;
    }

    std::error_code ec;
    if (!custom_path.empty()) {
        if (custom_path.extension() == ".mp4") {
            if (!custom_path.parent_path().empty()) {
                fs::create_directories(custom_path.parent_path(), ec);
            }
            return custom_path;
        }
        fs::create_directories(custom_path, ec);
        if (ec) {
            spdlog::error("Failed to create override record dir {}: {}", custom_path.string(), ec.message());
            return {};
        }
        return make_incremental_filename(custom_path);
    }

    auto root = find_default_media_root();
    if (root.empty()) {
        spdlog::warn("No removable storage detected under /var/media,/media,/run/media");
        return {};
    }
    fs::path record_dir = root / "RECORD";
    fs::create_directories(record_dir, ec);
    if (ec) {
        spdlog::error("Failed to create {}", record_dir.string());
        return {};
    }
    return make_incremental_filename(record_dir);
}

fs::path DvrRecorder::find_default_media_root() const
{
    const std::vector<fs::path> bases{"/var/media", "/media", "/run/media"};
    std::error_code ec;
    for (const auto &base : bases) {
        if (!fs::exists(base, ec)) {
            continue;
        }
        for (const auto &entry : fs::directory_iterator(base, ec)) {
            if (entry.is_directory(ec)) {
                return entry.path();
            }
        }
    }
    if (fs::exists("/storage", ec)) {
        return fs::path("/storage");
    }
    return {};
}

fs::path DvrRecorder::make_incremental_filename(const fs::path &dir) const
{
    int max_index = -1;
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        for (const auto &entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            if (entry.path().extension() != ".mp4") {
                continue;
            }
            const std::string stem = entry.path().stem().string();
            size_t digits = 0;
            while (digits < stem.size() && std::isdigit(static_cast<unsigned char>(stem[digits]))) {
                ++digits;
            }
            if (digits == 0) {
                continue;
            }
            try {
                const int value = std::stoi(stem.substr(0, digits));
                if (value > max_index) {
                    max_index = value;
                }
            } catch (...) {
                continue;
            }
        }
    }

    const int next_index = max_index + 1;
    std::ostringstream oss;
    oss << std::setw(4) << std::setfill('0') << next_index << ".mp4";
    return dir / oss.str();
}

int DvrRecorder::file_write_callback(int64_t offset, const void *buffer, size_t size, void *token)
{
    auto *self = static_cast<DvrRecorder *>(token);
    if (!self || !self->file_) {
        return -1;
    }
    if (std::fseek(self->file_, clamp_positive(offset), SEEK_SET) != 0) {
        return -1;
    }
    const size_t written = std::fwrite(buffer, 1, size, self->file_);
    if (written != size) {
        return -1;
    }
    const uint64_t new_end = static_cast<uint64_t>(clamp_positive(offset)) + written;
    uint64_t prev = self->current_file_bytes_.load(std::memory_order_relaxed);
    while (new_end > prev && !self->current_file_bytes_.compare_exchange_weak(prev, new_end, std::memory_order_relaxed)) {
        /* retry */
    }
    if (new_end >= self->kMaxFat32Size) {
        self->rotate_requested_.store(true, std::memory_order_relaxed);
    }
    return 0;
}

void DvrRecorder::reset_warmup_state()
{
    warmup_done_.store(false, std::memory_order_relaxed);
    warmup_start_ms_ = 0;
    warmup_frame_count_ = 0;
}
