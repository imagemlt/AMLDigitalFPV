#include <iostream>

// #include <codec.h>
#include "gstrtpreceiver.h"
#include "dvr_recorder.h"
#include "audio_receiver.h"
#include "scheduling_helper.hpp"
#include "spdlog/spdlog.h"
#include "concurrentqueue/blockingconcurrentqueue.h"
#include <csignal>
#include <execinfo.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <atomic>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <mutex>

extern "C"
{
#include "util.h"
#include "aml.h"
}

using namespace std;

constexpr int kDvrControlPort = 5612;

struct CmdOptions
{
    int width = 1920;
    int height = 1080;
    int fps = 120;
    std::string dvr_path; // if empty, auto path
    int enable_audio = 0;
    int frame_path = 1;   // 1 presents AMVIDEO, 0 presents AMLVIDEO_AMVIDEO (for amlvideo v4l2 pipeline)
    int type = 0;         // 0: H265, 1: H264
    int stream_type = 0;  // 0: frame, 1: es video
};

int signal_flag = 0;
int return_value = 0;
static CmdOptions g_opts;

std::unique_ptr<GstRtpReceiver>
    receiver;
std::unique_ptr<DvrRecorder> g_dvr;
std::unique_ptr<AudioReceiver> g_audio_receiver;
std::mutex g_audio_mutex;
moodycamel::BlockingConcurrentQueue<std::shared_ptr<std::vector<uint8_t>>> decode_queue;
std::atomic<bool> decoding_active{false};
std::thread decode_thread;
std::atomic<uint64_t> last_queue_log_ms{0};
std::thread dvr_command_thread;
std::atomic<bool> dvr_command_running{false};

static uint64_t monotonic_ms_main()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void signal_handler(int sig)
{
    void *array[10];
    size_t size;

    // 获取调用栈
    size = backtrace(array, 10);

    // 打印调用栈
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

void sig_handler(int signum)
{
    spdlog::info("Received signal {}", signum);
    signal_flag++;
    // mavlink_thread_signal++;
    // wfb_thread_signal++;
    // osd_thread_signal++;
    // if (dvr != NULL)
    //{
    //     dvr->shutdown();
    // }
    return_value = signum;
}

void dvr_command_loop(int port)
{
    SchedulingHelper::set_thread_params_max_realtime("DvrCommand", 10);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        spdlog::error("Failed to create DVR control socket");
        dvr_command_running.store(false);
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        spdlog::error("Failed to bind DVR control port {}", port);
        close(sock);
        dvr_command_running.store(false);
        return;
    }

    spdlog::info("DVR control listening on UDP {}", port);

    while (dvr_command_running.load())
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        timeval tv{};
        tv.tv_sec = 1;
        const int ret = select(sock + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            spdlog::warn("DVR control select() failed: {}", strerror(errno));
            break;
        }
        if (ret > 0 && FD_ISSET(sock, &rfds))
        {
            char buffer[128];
            sockaddr_in sender{};
            socklen_t sender_len = sizeof(sender);
            ssize_t len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                   reinterpret_cast<sockaddr *>(&sender), &sender_len);
            if (len > 0)
            {
                buffer[len] = '\0';
                std::string payload(buffer);
                if (payload.find("record=1") != std::string::npos)
                {
                    spdlog::info("DVR command: start recording");
                    if (g_dvr)
                    {
                        g_dvr->start_recording();
                    }
                }
                else if (payload.find("record=0") != std::string::npos)
                {
                    spdlog::info("DVR command: stop recording");
                    if (g_dvr)
                    {
                        g_dvr->stop_recording();
                    }
                }
                else if (payload.find("sound=1") != std::string::npos)
                {
                    std::lock_guard<std::mutex> lock(g_audio_mutex);
                    spdlog::info("Audio command: enable");
                    if (!g_audio_receiver)
                    {
                        g_audio_receiver = std::make_unique<AudioReceiver>(5600, 98, 8000);
                        if (!g_audio_receiver->start())
                        {
                            spdlog::error("Audio receiver failed to start");
                            g_audio_receiver.reset();
                        }
                    }
                }
                else if (payload.find("sound=0") != std::string::npos)
                {
                    std::lock_guard<std::mutex> lock(g_audio_mutex);
                    spdlog::info("Audio command: disable");
                    if (g_audio_receiver)
                    {
                        g_audio_receiver->stop();
                        g_audio_receiver.reset();
                    }
                }
                else if (payload.find("ping=1") != std::string::npos)
                {
                    constexpr const char *pong = "pong=1";
                    sendto(sock, pong, std::strlen(pong), 0,
                           reinterpret_cast<sockaddr *>(&sender), sender_len);
                }
            }
        }
    }

    close(sock);
    dvr_command_running.store(false);
}

int main(int argc, char *argv[])
{
    // parse args via getopt: -w width -h height -p fps -s path
    int opt;
    while ((opt = getopt(argc, argv, "w:h:p:s:f:t:d:a:")) != -1)
    {
        switch (opt)
        {
        case 'w':
            g_opts.width = std::atoi(optarg);
            break;
        case 'h':
            g_opts.height = std::atoi(optarg);
            break;
        case 'p':
            g_opts.fps = std::atoi(optarg);
            break;
        case 's':
            g_opts.dvr_path = optarg ? std::string(optarg) : "";
            break;
        case 'f':
            g_opts.frame_path = std::atoi(optarg);
            break;
        case 't':
            g_opts.type = std::atoi(optarg);
            break;
        case 'd':
            g_opts.stream_type = std::atoi(optarg);
            break;
        case 'a':
            g_opts.enable_audio = std::atoi(optarg);
            break;
        default:
            // ignore unknown options for now
            break;
        }
    }

    spdlog::set_level(spdlog::level::info);

    spdlog::info("Starting GST RTP Receiver... Daivide");
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    signal(SIGINT, sig_handler);
    // Initialize AML library
    try
    {
        aml_setup(g_opts.type, g_opts.width, g_opts.height, g_opts.fps, NULL, 0, g_opts.frame_path, g_opts.stream_type);
        const auto selected_codec = g_opts.type == 0 ? VideoCodec::H265 : VideoCodec::H264;
        receiver = std::make_unique<GstRtpReceiver>(5600, selected_codec);

        spdlog::info("GstRtpReceiver instance created.");
        g_dvr = std::make_unique<DvrRecorder>();
        g_dvr->set_video_params(g_opts.width, g_opts.height, g_opts.fps, selected_codec);
        if (!g_opts.dvr_path.empty())
        {
            g_dvr->set_override_path(g_opts.dvr_path);
        }
        if (g_opts.enable_audio)
        {
            std::lock_guard<std::mutex> lock(g_audio_mutex);
            spdlog::info("Audio CLI: enable (-a 1)");
            g_audio_receiver = std::make_unique<AudioReceiver>(5600, 98, 8000);
            if (!g_audio_receiver->start())
            {
                spdlog::error("Audio receiver failed to start");
                g_audio_receiver.reset();
            }
        }
        dvr_command_running = true;
        dvr_command_thread = std::thread(dvr_command_loop, kDvrControlPort);

        long long bytes_received = 0;
        long long frame_count = 0;
        uint64_t period_start = 0;

        decoding_active = true;
        decode_thread = std::thread([&frame_count]()
                                    {
            spdlog::info("decoding thread start");
            
            SchedulingHelper::set_thread_params_max_realtime("DecodeThread", SchedulingHelper::PRIORITY_REALTIME_MID);

            while (decoding_active || decode_queue.size_approx() > 0) {
                std::shared_ptr<std::vector<uint8_t>> frame;
                decode_queue.wait_dequeue(frame);
                if (frame != nullptr)
                {
                    const uint64_t queue_depth = decode_queue.size_approx();
                    if (queue_depth > 0) {
                        spdlog::debug("[decode] dequeued frame size={} queue_depth={}", frame->size(), queue_depth);
                    }
                    //spdlog::debug("{}ms Decoding frame of size {}", get_time_ms(), frame->size());
                    //if (frame_count % 60 == 0)
                    //{
                        //spdlog::info("frame size {}", frame->size());
                    //    measure_latency_breakdown();
                    //}
                    const uint64_t submit_begin = monotonic_ms_main();
                    int ret = aml_submit_decode_unit(frame->data(), frame->size());
                    const uint64_t submit_end = monotonic_ms_main();
                    const uint64_t submit_cost = submit_end - submit_begin;
                    if (submit_cost > 0)
                    {
                        spdlog::debug("[decode] aml_submit took {} ms (queue_depth={})",
                                      submit_cost,
                                      decode_queue.size_approx());
                    }
                    if (!ret) {
                        spdlog::debug("decode result: {}", ret);
                    }
                    frame_count++;
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            
            spdlog::info("decode thread terminated"); });

        auto cb = [/*&decoder_stalled_count,*/ &bytes_received, &frame_count, &period_start](std::shared_ptr<std::vector<uint8_t>> frame)
        {
            // Let the gst pull thread run at quite high priority
            static bool first = false;
            static uint64_t fps_window_start = monotonic_ms_main();
            static uint32_t fps_window_frames = 0;
            if (first)
            {
                SchedulingHelper::set_thread_params_max_realtime("GstThread", SchedulingHelper::PRIORITY_REALTIME_LOW);
                first = false;
            }
            bytes_received += frame->size();
            // spdlog::debug("{}ms Received frame of size {} ", get_time_ms(), frame->size());
            decode_queue.enqueue(frame);
            if (g_dvr)
            {
                g_dvr->enqueue_frame(frame);
            }
            const auto depth = decode_queue.size_approx();
            const auto now_ms = monotonic_ms_main();
            if (depth > 0)
            {
                spdlog::debug("[enqueue] frame={} bytes queue_depth={}", frame->size(), depth);
            }
            uint64_t expected = last_queue_log_ms.load();
            if (now_ms - expected >= 1000 && last_queue_log_ms.compare_exchange_strong(expected, now_ms))
            {
                spdlog::debug("[queue] depth={} bytes_received={} frame_count={}", depth, bytes_received, frame_count);
            }
            const bool recording_active = g_dvr && g_dvr->is_recording();
            if (recording_active)
            {
                fps_window_frames++;
                const uint64_t window_span = now_ms - fps_window_start;
                if (window_span >= 500)
                {
                    const double fps = fps_window_frames * 1000.0 / std::max<uint64_t>(1, window_span);
                    g_dvr->update_frame_rate(fps);
                    fps_window_frames = 0;
                    fps_window_start = now_ms;
                }
            }
            else
            {
                fps_window_frames = 0;
                fps_window_start = now_ms;
            }
            // osd_publish_uint_fact("gstreamer.received_bytes", NULL, 0, frame->size());
            // feed_packet_to_decoder(packet, frame->data(), frame->size());
            // if (dvr_enabled && dvr != NULL)
            //{
            //     dvr->frame(frame);
            // }
        };

        spdlog::info("register receiver");
        receiver->start_receiving(cb);

        spdlog::info("GST RTP Receiver is running. Press Ctrl-C to stop...");

        while (!signal_flag)
        {
            sleep(10);
        }
        receiver->stop_receiving();
    }
    catch (const std::exception &e)
    {
        spdlog::error("Exception occurred: {}", e.what());
        return -1;
    }
    spdlog::info("GST RTP Receiver stopped.");
    decoding_active = false;
    decode_queue.enqueue(nullptr);
    if (decode_thread.joinable())
    {
        decode_thread.join();
    }
    dvr_command_running = false;
    if (dvr_command_thread.joinable())
    {
        dvr_command_thread.join();
    }
    if (g_dvr)
    {
        g_dvr->shutdown();
    }
    if (g_audio_receiver)
    {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_audio_receiver->stop();
        g_audio_receiver.reset();
    }
    aml_cleanup();
    return 0;
}
