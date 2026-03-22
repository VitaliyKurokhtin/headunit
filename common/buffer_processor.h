#pragma once

#include <cstdint>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

template<typename QueueEntry>
class BufferProcessor
{
public:
    virtual ~BufferProcessor() { stop(); }

    void start()
    {
        quit_.store(false);
        thread_ = std::thread(&BufferProcessor::run, this);
    }

    void stop()
    {
        if (!thread_.joinable())
            return;

        quit_.store(true);
        onStopping();
        cv_.notify_one();
        thread_.join();
    }

protected:
    BufferProcessor() = default;

    // Enqueue a fully constructed entry.
    void enqueueEntry(QueueEntry&& entry)
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            queue_.push(std::move(entry));
        }
        cv_.notify_one();
    }

    // Atomically clear all pending entries.
    void clearQueue()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::queue<QueueEntry>().swap(queue_);
    }

    // Called on the worker thread before entering the processing loop.
    virtual void onStarted() {}

    // Called from stop() after quit is signalled but before join.
    virtual void onStopping() {}

    // Called on the worker thread for each dequeued entry.
    virtual void process(QueueEntry& entry) = 0;

private:
    std::queue<QueueEntry> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> quit_{false};
    std::thread thread_;

    void run()
    {
        onStarted();
        while (!quit_.load())
        {
            QueueEntry entry;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [&] {
                    return quit_.load() || !queue_.empty();
                });

                if (quit_.load())
                    break;

                entry = std::move(queue_.front());
                queue_.pop();
            }

            process(entry);
        }
    }
};
