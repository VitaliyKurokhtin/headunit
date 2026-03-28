#include "buffer_pool.h"

BufferPool::BufferPool(size_t bufferSize, size_t prealloc, bool allowGrowth)
    : bufferSize_(bufferSize), allowGrowth_(allowGrowth)
{
    pool_.reserve(prealloc);
    for (size_t i = 0; i < prealloc; ++i)
        pool_.emplace_back(bufferSize);
}

std::shared_ptr<BufferPool::Buffer> BufferPool::acquire()
{
    Buffer buf;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!pool_.empty())
        {
            buf = std::move(pool_.back());
            pool_.pop_back();
        }
    }
    
    if (buf.empty())
        buf.resize(bufferSize_);

    return std::shared_ptr<Buffer>(new Buffer(std::move(buf)),
        [this](Buffer* p) {
            release(std::move(*p));
            delete p;
        });
}

void BufferPool::release(Buffer&& buf)
{
    if (buf.size() < bufferSize_ || !allowGrowth_)
        buf.resize(bufferSize_);
    std::lock_guard<std::mutex> lk(mutex_);
    pool_.push_back(std::move(buf));
}
