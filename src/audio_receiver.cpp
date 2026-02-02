//
// Simple RTP(Opus) audio receiver + PulseAudio output.
//

#include "audio_receiver.h"
#include "scheduling_helper.hpp"
#include "spdlog/spdlog.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include <opus/opus.h>
#include <pulse/simple.h>
#include <pulse/error.h>

namespace {
constexpr size_t kMaxQueueDepth = 256;

} // namespace

AudioReceiver::AudioReceiver(int port, uint8_t payload_type, int sample_rate)
    : port_(port), payload_type_(payload_type), sample_rate_(sample_rate) {}

AudioReceiver::~AudioReceiver() {
    stop();
}

bool AudioReceiver::start() {
    if (running_.load()) {
        return true;
    }
    running_.store(true);
    play_thread_ = std::make_unique<std::thread>(&AudioReceiver::play_loop, this);
    return true;
}

void AudioReceiver::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (play_thread_) {
        play_thread_->join();
        play_thread_.reset();
    }
}

void AudioReceiver::enqueue_payload(const std::shared_ptr<std::vector<uint8_t>> &payload) {
    if (!payload || payload->empty()) {
        return;
    }
    static std::atomic<bool> first_logged{false};
    if (!first_logged.exchange(true)) {
        spdlog::info("AudioReceiver: first audio payload received ({} bytes)", payload->size());
    }
    if (queue_.size_approx() > kMaxQueueDepth) {
        drop_count_++;
        return;
    }
    queue_.enqueue(payload);
    pkt_count_++;
}

void AudioReceiver::play_loop() {
    SchedulingHelper::set_thread_params_other("AudioPlay", 8);

    int opus_err = 0;
    OpusDecoder *decoder = opus_decoder_create(sample_rate_, 1, &opus_err);
    if (!decoder || opus_err != OPUS_OK) {
        spdlog::error("AudioReceiver: opus_decoder_create failed: {}", opus_err);
        return;
    }

    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = static_cast<uint32_t>(sample_rate_);
    ss.channels = 1;

    int pa_err = 0;
    pa_simple *pa = pa_simple_new(nullptr, "AMStreamer", PA_STREAM_PLAYBACK, nullptr,
                                  "RTP Audio", &ss, nullptr, nullptr, &pa_err);
    if (!pa) {
        spdlog::error("AudioReceiver: pa_simple_new failed: {}", pa_strerror(pa_err));
        opus_decoder_destroy(decoder);
        return;
    }

    std::vector<int16_t> pcm(960 * 2);
    while (running_.load()) {
        std::shared_ptr<std::vector<uint8_t>> pkt;
        if (!queue_.wait_dequeue_timed(pkt, std::chrono::milliseconds(200))) {
            continue;
        }
        if (!pkt || pkt->empty()) {
            continue;
        }
        const int frame_size = opus_decode(decoder, pkt->data(),
                                           static_cast<opus_int32>(pkt->size()),
                                           pcm.data(),
                                           static_cast<int>(pcm.size()), 0);
        if (frame_size <= 0) {
            continue;
        }
        const size_t bytes = static_cast<size_t>(frame_size) * sizeof(int16_t);
        if (pa_simple_write(pa, pcm.data(), bytes, &pa_err) < 0) {
            spdlog::warn("AudioReceiver: pa_simple_write failed: {}", pa_strerror(pa_err));
        }
    }

    pa_simple_drain(pa, &pa_err);
    pa_simple_free(pa);
    opus_decoder_destroy(decoder);
}
