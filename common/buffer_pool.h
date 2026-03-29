#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>

class BufferPool
{
public:
    using Buffer = std::vector<uint8_t>;

    BufferPool(size_t bufferSize, size_t prealloc, bool allowGrowth = false,
               const char* name = "unnamed");

    // Take a buffer from the pool. The buffer is automatically
    // returned to the pool when the last shared_ptr reference drops.
    std::shared_ptr<Buffer> acquire();

private:
    const char* name_;
    size_t bufferSize_;
    bool allowGrowth_;

    // Fast path: pre-allocated fixed slots, lock-free via atomic flag.
    // Acquire uses relaxed-order CAS (single acquirer assumed).
    // Release from any thread uses release-store.
    struct Slot {
        Buffer buf;
        std::atomic<bool> in_use;
        Slot() : in_use(false) {}
    };
    std::vector<Slot> slots_;

    // Slow path: mutex-protected overflow for when all slots are busy.
    std::mutex overflow_mutex_;
    std::vector<Buffer> overflow_pool_;
    std::atomic<int> overflow_active_{0};   // currently outstanding overflow buffers
    std::atomic<int> overflow_peak_{0};     // high-water mark

    void release_to_overflow(Buffer&& buf);
};
