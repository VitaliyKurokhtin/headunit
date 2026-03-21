#include "buffer_pool.h"

BufferPool::BufferPool(size_t bufferSize, size_t prealloc)
    : bufferSize_(bufferSize)
{
    pool_.reserve(prealloc);
    for (size_t i = 0; i < prealloc; ++i)
        pool_.emplace_back(bufferSize);
}

BufferPool::Buffer BufferPool::acquire()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (pool_.empty())
        return Buffer(bufferSize_);

    Buffer buf = std::move(pool_.back());
    pool_.pop_back();
    return buf;
}

void BufferPool::release(Buffer&& buf)
{
    buf.resize(bufferSize_);
    std::lock_guard<std::mutex> lk(mutex_);
    pool_.push_back(std::move(buf));
}
