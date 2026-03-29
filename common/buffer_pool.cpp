#include "buffer_pool.h"

#define LOGTAG "buffer_pool"
#include "hu_uti.h"

BufferPool::BufferPool(size_t bufferSize, size_t prealloc, bool allowGrowth,
                       const char* name)
    : name_(name), bufferSize_(bufferSize), allowGrowth_(allowGrowth),
      slots_(prealloc)
{
    for (size_t i = 0; i < prealloc; ++i)
        slots_[i].buf.resize(bufferSize);
}

std::shared_ptr<BufferPool::Buffer> BufferPool::acquire()
{
    // Fast path: find a free slot (no mutex, no heap alloc for Buffer)
    for (auto& s : slots_)
    {
        bool expected = false;
        if (s.in_use.compare_exchange_strong(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed))
        {
            return std::shared_ptr<Buffer>(&s.buf, [this, &s](Buffer*) {
                if (s.buf.size() < bufferSize_ || !allowGrowth_)
                    s.buf.resize(bufferSize_);
                s.in_use.store(false, std::memory_order_release);
            });
        }
    }

    // Slow path: all slots busy — use mutex-protected overflow
    int active = overflow_active_.fetch_add(1, std::memory_order_relaxed) + 1;
    int peak = overflow_peak_.load(std::memory_order_relaxed);
    if (active > peak)
    {
        overflow_peak_.store(active, std::memory_order_relaxed);
        logw("BufferPool '%s': new peak %d overflow (slots=%zu)",
                name_, active, slots_.size());
    }

    Buffer buf;
    {
        std::lock_guard<std::mutex> lk(overflow_mutex_);
        if (!overflow_pool_.empty())
        {
            buf = std::move(overflow_pool_.back());
            overflow_pool_.pop_back();
        }
    }

    if (buf.empty())
        buf.resize(bufferSize_);

    return std::shared_ptr<Buffer>(new Buffer(std::move(buf)),
        [this](Buffer* p) {
            overflow_active_.fetch_sub(1, std::memory_order_relaxed);
            release_to_overflow(std::move(*p));
            delete p;
        });
}

void BufferPool::release_to_overflow(Buffer&& buf)
{
    if (buf.size() < bufferSize_ || !allowGrowth_)
        buf.resize(bufferSize_);
    std::lock_guard<std::mutex> lk(overflow_mutex_);
    overflow_pool_.push_back(std::move(buf));
}
