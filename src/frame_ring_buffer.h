#pragma once

#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>

class FrameRingBuffer {
public:
    explicit FrameRingBuffer(size_t capacity)
        : capacity_(capacity), buffer_(capacity) {}

    void push(const std::shared_ptr<std::vector<uint8_t>> &frame)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (capacity_ == 0)
            return;
        if (size_ == capacity_) {
            head_ = (head_ + 1) % capacity_;
            --size_;
        }
        size_t tail = (head_ + size_) % capacity_;
        buffer_[tail] = frame;
        ++size_;
        cv_.notify_one();
    }

    std::shared_ptr<std::vector<uint8_t>> pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]{ return size_ > 0; });
        auto frame = buffer_[head_];
        head_ = (head_ + 1) % capacity_;
        --size_;
        return frame;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == 0;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_ = 0;
        head_ = 0;
    }

private:
    size_t capacity_;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> buffer_;
    size_t head_ = 0;
    size_t size_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

