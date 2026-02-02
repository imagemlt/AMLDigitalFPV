//
// Simple RTP(Opus) audio receiver + PulseAudio output.
//

#ifndef AMSTREAMER_AUDIO_RECEIVER_H
#define AMSTREAMER_AUDIO_RECEIVER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include "concurrentqueue/blockingconcurrentqueue.h"

class AudioReceiver {
public:
    AudioReceiver(int port, uint8_t payload_type, int sample_rate);
    ~AudioReceiver();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }
    void enqueue_payload(const std::shared_ptr<std::vector<uint8_t>> &payload);

private:
    void play_loop();

    int port_;
    uint8_t payload_type_;
    int sample_rate_;

    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> play_thread_;

    moodycamel::BlockingConcurrentQueue<std::shared_ptr<std::vector<uint8_t>>> queue_;
    std::atomic<uint64_t> pkt_count_{0};
    std::atomic<uint64_t> drop_count_{0};
};

#endif // AMSTREAMER_AUDIO_RECEIVER_H
