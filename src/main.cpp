#include <iostream>

// #include <codec.h>
#include "gstrtpreceiver.h"
#include "scheduling_helper.hpp"
#include "spdlog/spdlog.h"
#include "concurrentqueue/blockingconcurrentqueue.h"
#include <csignal>
#include <execinfo.h>
#include <thread>

extern "C"
{
#include "util.h"
#include "aml.h"
}

using namespace std;

int signal_flag = 0;
int return_value = 0;

std::unique_ptr<GstRtpReceiver>
    receiver;
moodycamel::BlockingConcurrentQueue<std::shared_ptr<std::vector<uint8_t>>> decode_queue;
std::atomic<bool> decoding_active{false};
std::thread decode_thread;
std::atomic<uint64_t> last_queue_log_ms{0};

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

int main(int argc, char *argv[])
{
    spdlog::set_level(spdlog::level::info);

    spdlog::info("Starting GST RTP Receiver... Daivide");
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    signal(SIGINT, sig_handler);
    // Initialize AML library
    try
    {
        // char *addr = "0.0.0.0:5600";
        //  Create GST RTP Receiver instance
        aml_setup(0, 1920, 1080, 120, NULL, 0);
        receiver = std::make_unique<GstRtpReceiver>(5600, VideoCodec::H265);

        spdlog::info("GstRtpReceiver instance created.");
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
            if (first)
            {
                SchedulingHelper::set_thread_params_max_realtime("GstThread", SchedulingHelper::PRIORITY_REALTIME_LOW);
                first = false;
            }
            bytes_received += frame->size();
            // spdlog::debug("{}ms Received frame of size {} ", get_time_ms(), frame->size());
            decode_queue.enqueue(frame);
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

        bool flag = true;
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
    aml_cleanup();
    return 0;
}
