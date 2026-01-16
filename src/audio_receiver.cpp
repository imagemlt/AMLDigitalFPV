#include "audio_receiver.h"

#include "scheduling_helper.hpp"
#include "spdlog/spdlog.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <opus/opus.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{
constexpr size_t kMaxRtpPacket = 2048;
constexpr int kSelectTimeoutMs = 200;
} // namespace

AudioReceiver::AudioReceiver(int port, int payload_type, int sample_rate)
    : port_(port), payload_type_(payload_type), sample_rate_(sample_rate)
{
}

AudioReceiver::~AudioReceiver()
{
    stop();
}

bool AudioReceiver::init_decoder()
{
    int error = 0;
    decoder_ = opus_decoder_create(sample_rate_, 1, &error);
    if (!decoder_ || error != OPUS_OK)
    {
        spdlog::error("Failed to create Opus decoder: {}", opus_strerror(error));
        return false;
    }
    return true;
}

bool AudioReceiver::init_pulse()
{
    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = sample_rate_;
    ss.channels = 1;

    int error = 0;
    pulse_ = pa_simple_new(nullptr, "AMLDigitalFPV", PA_STREAM_PLAYBACK, nullptr,
                           "FPV Audio", &ss, nullptr, nullptr, &error);
    if (!pulse_)
    {
        spdlog::error("Failed to connect to PulseAudio: {}", pa_strerror(error));
        return false;
    }
    return true;
}

bool AudioReceiver::start()
{
    if (running_.load())
        return true;

    if (!init_decoder())
    {
        cleanup();
        return false;
    }
    if (!init_pulse())
    {
        cleanup();
        return false;
    }

    running_.store(true);
    worker_ = std::thread(&AudioReceiver::audio_loop, this);
    return true;
}

void AudioReceiver::stop()
{
    running_.store(false);
    if (worker_.joinable())
        worker_.join();
    cleanup();
}

void AudioReceiver::cleanup()
{
    if (pulse_)
    {
        int error = 0;
        pa_simple_drain(pulse_, &error);
        pa_simple_free(pulse_);
        pulse_ = nullptr;
    }
    if (decoder_)
    {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
    if (sock_fd_ >= 0)
    {
        close(sock_fd_);
        sock_fd_ = -1;
    }
}

void AudioReceiver::audio_loop()
{
    SchedulingHelper::set_thread_params_other("AudioThread", 10);

    sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0)
    {
        spdlog::error("Audio socket create failed: {}", strerror(errno));
        running_.store(false);
        return;
    }

    int reuse = 1;
    setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
    int rcvbuf = 1 * 1024 * 1024;
    setsockopt(sock_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (bind(sock_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        spdlog::error("Audio socket bind failed on port {}: {}", port_, strerror(errno));
        running_.store(false);
        return;
    }

    spdlog::info("Audio receiver listening on UDP port {} (payload={})", port_, payload_type_);

    std::vector<uint8_t> packet(kMaxRtpPacket);
    const int max_frame_samples = sample_rate_ * 60 / 1000; // 60ms margin
    std::vector<int16_t> pcm(max_frame_samples);

    while (running_.load())
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock_fd_, &readfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = kSelectTimeoutMs * 1000;

        int ready = select(sock_fd_ + 1, &readfds, nullptr, nullptr, &tv);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            spdlog::warn("Audio select failed: {}", strerror(errno));
            break;
        }
        if (ready == 0 || !FD_ISSET(sock_fd_, &readfds))
            continue;

        ssize_t len = recv(sock_fd_, packet.data(), packet.size(), 0);
        if (len <= 12)
            continue;

        const uint8_t pt = packet[1] & 0x7F;
        if (pt != payload_type_)
            continue;

        size_t offset = 12;
        const uint8_t cc = packet[0] & 0x0F;
        offset += cc * 4;
        if (offset >= static_cast<size_t>(len))
            continue;

        const bool extension = (packet[0] & 0x10) != 0;
        if (extension)
        {
            if (offset + 4 > static_cast<size_t>(len))
                continue;
            uint16_t ext_len = ntohs(*reinterpret_cast<uint16_t *>(&packet[offset + 2]));
            offset += 4 + ext_len * 4;
            if (offset >= static_cast<size_t>(len))
                continue;
        }

        const uint8_t *payload = packet.data() + offset;
        const size_t payload_len = len - offset;
        int decoded = opus_decode(decoder_, payload, static_cast<opus_int32>(payload_len),
                                  pcm.data(), max_frame_samples, 0);
        if (decoded <= 0)
        {
            if (decoded < 0)
                spdlog::debug("Opus decode error: {}", opus_strerror(decoded));
            continue;
        }

        if (pulse_)
        {
            size_t bytes = decoded * sizeof(int16_t);
            int error = 0;
            if (pa_simple_write(pulse_, pcm.data(), bytes, &error) < 0)
                spdlog::warn("PulseAudio write failed: {}", pa_strerror(error));
        }
    }

    running_.store(false);
}
