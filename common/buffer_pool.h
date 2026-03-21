#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>

class BufferPool
{
public:
    using Buffer = std::vector<uint8_t>;

    BufferPool(size_t bufferSize, size_t prealloc);

    // Take a buffer from the pool (allocates if pool is empty).
    // Returned buffer has size == bufferSize.
    Buffer acquire();

    // Return a buffer to the pool for reuse.
    void release(Buffer&& buf);

private:
    size_t bufferSize_;
    std::vector<Buffer> pool_;
    std::mutex mutex_;
};
