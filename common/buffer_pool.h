#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <memory>

class BufferPool
{
public:
    using Buffer = std::vector<uint8_t>;

    BufferPool(size_t bufferSize, size_t prealloc, bool allowGrowth = false);

    // Take a buffer from the pool. The buffer is automatically
    // returned to the pool when the last shared_ptr reference drops.
    std::shared_ptr<Buffer> acquire();

private:
    size_t bufferSize_;
    bool allowGrowth_;
    std::vector<Buffer> pool_;
    std::mutex mutex_;

    void release(Buffer&& buf);
};
