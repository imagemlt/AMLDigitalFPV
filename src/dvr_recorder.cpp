#include "dvr_recorder.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

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

void DvrRecorder::set_video_params(uint32_t width, uint32_t height, uint32_t fps, VideoCodec codec)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    video_width_ = width;
    video_height_ = height;
    video_fps_ = fps;
    codec_ = codec;
    if (fps > 0) {
        frame_duration_ = std::max(1u, static_cast<uint32_t>(90000 / fps));
    } else {
        frame_duration_ = kDefaultFrameDuration;
    }
}

void DvrRecorder::set_override_path(const std::string &path)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    override_path_ = fs::path(path);
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
    SchedulingHelper::set_thread_params_max_realtime("DvrRecorder", 15);
    while (true) {
        Command cmd;
        queue_.wait_dequeue(cmd);

        switch (cmd.type) {
        case CommandType::Start:
            open_writer();
            break;
        case CommandType::Stop:
            close_writer();
            break;
        case CommandType::Frame:
            if (ready_to_write() && cmd.frame) {
                auto duration = frame_duration_;
                if (duration == 0) {
                    duration = kDefaultFrameDuration;
                }
                const int res = mp4_h26x_write_nal(writer_,
                                                   cmd.frame->data(),
                                                   static_cast<int>(cmd.frame->size()),
                                                   static_cast<int>(duration));
                if (!(res == MP4E_STATUS_OK || res == MP4E_STATUS_BAD_ARGUMENTS)) {
                    spdlog::warn("mp4_h26x_write_nal returned {}", res);
                }
            }
            break;
        case CommandType::Shutdown:
            running_.store(false);
            break;
        }

        if (cmd.type == CommandType::Shutdown) {
            break;
        }
    }

    close_writer();
}

bool DvrRecorder::ready_to_write() const
{
    return recording_.load() && writer_ready_;
}

bool DvrRecorder::open_writer()
{
    if (recording_) {
        return true;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    VideoCodec codec = VideoCodec::H265;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        width = video_width_;
        height = video_height_;
        fps = video_fps_;
        codec = codec_;
    }
    if (width == 0 || height == 0 || fps == 0) {
        spdlog::warn("DVR cannot start: invalid video params {}x{} @{}", width, height, fps);
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
    MP4E_mux_t *mux = MP4E_open(0, kFragmentationMode, file, file_write_callback);
    if (!mux) {
        spdlog::error("MP4E_open failed");
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
        std::fclose(file);
        return false;
    }

    file_ = file;
    mux_ = mux;
    writer_ready_ = true;
    recording_.store(true);
    spdlog::info("DVR recording to {}", path_str);
    return true;
}

void DvrRecorder::close_writer()
{
    if (!recording_) {
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
    recording_.store(false);
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
    auto *file = static_cast<FILE *>(token);
    if (!file) {
        return -1;
    }
    if (std::fseek(file, clamp_positive(offset), SEEK_SET) != 0) {
        return -1;
    }
    const size_t written = std::fwrite(buffer, 1, size, file);
    return written == size ? 0 : -1;
}
