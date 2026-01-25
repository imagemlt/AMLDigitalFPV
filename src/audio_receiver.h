#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

struct pa_simple;
struct OpusDecoder;

class AudioReceiver
{
public:
    explicit AudioReceiver(int port = 5600, int payload_type = 98, int sample_rate = 8000);
    ~AudioReceiver();

    bool start();
    bool start_external();
    void stop();
    void feed_opus_payload(const std::shared_ptr<std::vector<uint8_t>> &payload);

private:
    void audio_loop();
    bool init_decoder();
    bool init_pulse();
    void cleanup();

    const int port_;
    const int payload_type_;
    const int sample_rate_;

    int sock_fd_ = -1;
    pa_simple *pulse_ = nullptr;
    OpusDecoder *decoder_ = nullptr;

    std::atomic<bool> running_{false};
    bool external_mode_ = false;
    std::thread worker_;
};
