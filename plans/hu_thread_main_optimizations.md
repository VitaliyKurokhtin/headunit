# hu_thread_main Loop Optimizations

Target: minimize per-iteration latency in the hu_thread_main while loop.
Constraints: C++11, GCC 4.9.1, ARM Cortex-A9 (Mazda CMU cross-toolchain).

---

## 1. ~~Eliminate double select() on receive path~~ [DONE]

**Problem**: `hu_thread_main` does `select()` and enters `hu_aap_recv_process` only
when `transportFD` is readable. Inside, `hu_aap_tra_recv(tmo)` does a *second*
`select()` before `read()` when `tmo > 0` (WiFi) or `errorfd >= 0`.

For the first read (4-byte header), the fd is already known-readable — the inner
select is pure overhead. For subsequent reads in the `while(remaining_bytes_in_frame)`
loop, the inner select may be needed (data might not all be available yet), but
even there the errorfd check duplicates the outer loop's error handling.

**Fix**: Add a `hu_aap_tra_recv_direct(buf, len)` that skips `select()` entirely
and just calls `read()`. Use it for the first 4-byte header read in
`hu_aap_recv_process`. Keep the existing `hu_aap_tra_recv` with select/timeout
for the remaining-bytes loop where we may need to wait for more data.

```cpp
// New method — no select, no timeout, no errorfd check
int HUServer::hu_aap_tra_recv_direct(byte* buf, int len) {
    int ret = read(transport->GetReadFD(), buf, len);
    if (ret < 0) {
        loge("hu_aap_tra_recv_direct error: %d", ret);
        hu_aap_stop();
    }
    return ret;
}
```

Then in `hu_aap_recv_process`:
```cpp
have_len = hu_aap_tra_recv_direct(enc_buf, min_size_hdr);  // fd already readable
```

**Files**: `hu/hu_aap.cpp`, `hu/hu_aap.h`

---

## 2. Reduce SSL mutex contention on decrypt path [HIGH IMPACT]

**Problem**: `hu_ssl_decrypt` acquires `hu_ssl_mutex` on every encrypted frame.
The same mutex is held by `hu_ssl_encrypt` on send threads. Since both operate on
the same `SSL` object, the mutex cannot be removed — OpenSSL forbids concurrent
`SSL_read`/`SSL_write` on the same `SSL*`.

**Fix**: Minimize time spent under the lock by the encrypt path so the decrypt
path stalls less. Two approaches:

**a) Batch encrypt outside the lock**: In `hu_aap_enc_send_locked`, prepare the
plaintext fully, then hold `hu_ssl_mutex` only for the `BIO_write`+`SSL_write`+
`BIO_read` sequence. Currently `hu_ssl_encrypt` already does this, but the call
chain holds `send_mutex` *and then* `hu_ssl_mutex` (nested), meaning the encrypt
side holds the SSL lock for the minimum already. So this is already close to
optimal.

**b) Move encrypt to a dedicated lock-free path**: If we add a second SSL context
(second `SSL_CTX` + `SSL*` + BIO pair) exclusively for encrypting, the two SSL
objects would be independent and require no shared mutex. This requires duplicating
the SSL setup (cert/key loading) for the encrypt-only context. The encrypt context
only needs `SSL_write` + `BIO_read` from `wm_bio`.

Approach (b) is the real fix. The cost is ~2KB extra memory for the second SSL
object + BIOs.

**Implementation sketch for (b)**:
- Add members: `SSL* hu_ssl_encrypt_ssl`, `BIO* hu_ssl_encrypt_wm_bio`,
  `std::mutex hu_ssl_encrypt_mutex` (or reuse `send_mutex` which is already held).
- In `hu_ssl_begin_handshake`, after the main handshake completes, create a second
  SSL object from the same `SSL_CTX`, attach a new write-memory BIO pair, copy
  session keys. Note: this is non-trivial because after handshake, the session
  state (encryption keys, sequence numbers) must be shared or exported. OpenSSL
  does not support cloning an established SSL session into a second object for
  independent encrypt/decrypt use.

**REVISED assessment**: Splitting into two SSL objects is NOT feasible with
OpenSSL's API — the session state (symmetric keys, sequence counters) is internal
and cannot be extracted and assigned to a second object. The single `SSL*` must be
shared.

**Practical fix**: Since contention is between hu_thread (decrypt) and sender_thread
(encrypt), and the sender_thread calls are spaced out, the actual contention
window is small. The main optimization here is to ensure the encrypt path does not
hold the mutex while doing I/O. Currently `hu_ssl_encrypt` locks, does
BIO_write+SSL_write+BIO_read (all in-memory, fast), then unlocks. The subsequent
`hu_aap_tra_send` (actual USB/TCP write) is *outside* the SSL lock. This is
already well structured.

**Remaining action**: Verify that no code path accidentally holds `hu_ssl_mutex`
during transport I/O. If not, this item is already well-optimized and can be
**deprioritized**.

**Files**: `hu/hu_ssl.cpp`, `hu/hu_aap.cpp`

---

## 3. Eliminate mutex in BufferPool::acquire() for recv path [MEDIUM IMPACT]

**Problem**: `recv_pool.acquire()` locks a mutex every call. Encrypted frames
acquire 2 buffers per frame. The pool is only used for receiving (hu_thread), but
the release (custom deleter) can fire from any thread.

**Fix**: Replace the mutex-guarded `std::vector` pool with a lock-free
single-producer (hu_thread acquires) / multi-producer (any thread releases)
stack. In C++11 we can use `std::atomic` with a simple tagged-pointer free list.

Simpler alternative: use a thread-local free list for the hu_thread side. Since
only one thread ever calls `acquire()`, keep a non-atomic local list for fast
acquire. The `release()` path (from other threads) pushes to a separate
`std::atomic`-based return queue. At acquire time, drain the return queue first.

```cpp
class RecvBufferPool {
    size_t bufferSize_;
    // Fast path: hu_thread-only list (no sync needed for acquire)
    std::vector<Buffer> local_;
    // Return queue: lock-free or mutex-protected, fed by other threads
    std::mutex returnMutex_;
    std::vector<Buffer> returned_;
public:
    std::shared_ptr<Buffer> acquire() {
        // First, drain any returned buffers (rare, only when other threads released)
        if (!returned_.empty()) {        // relaxed check, ok to miss briefly
            std::lock_guard<std::mutex> lk(returnMutex_);
            for (auto& b : returned_)
                local_.push_back(std::move(b));
            returned_.clear();
        }
        Buffer buf;
        if (!local_.empty()) {
            buf = std::move(local_.back());
            local_.pop_back();
        } else {
            buf.resize(bufferSize_);
        }
        return std::shared_ptr<Buffer>(new Buffer(std::move(buf)),
            [this](Buffer* p) {
                std::lock_guard<std::mutex> lk(returnMutex_);
                returned_.push_back(std::move(*p));
                delete p;
            });
    }
};
```

This turns the hot-path acquire into a simple vector pop with no locking.

**Files**: `common/buffer_pool.h`, `common/buffer_pool.cpp`

---

## 4. ~~Short-circuit logd macro before argument evaluation~~ [DONE]

**Problem**: `logd(...)` expands to `hu_log(hu_LOG_DEB, ...)` which evaluates all
arguments (including `chan_get(chan)` with its switch) before the runtime
`ena_log_debug` check rejects them. Called on every frame at lines 862, 1455, etc.

With `ena_log_debug = 0` (the default), every call evaluates args for nothing.

**Fix**: Change the `logd` macro to short-circuit:

```cpp
#define logd(...) do { if (ena_log_debug) hu_log(hu_LOG_DEB, __FILE__ ":" STR(__LINE__), __FUNCTION__, __VA_ARGS__); } while(0)
```

Apply the same pattern to `logv`:
```cpp
#define logv(...) do { if (ena_log_verbo) hu_log(hu_LOG_VER, __FILE__ ":" STR(__LINE__), __FUNCTION__, __VA_ARGS__); } while(0)
```

This is zero-cost when logging is disabled — the compiler will optimize away dead
code and skip argument evaluation entirely.

**Files**: `hu/hu_uti.h`

---

## 5. Avoid per-frame heap allocation in shared_ptr control block [MEDIUM IMPACT]

**Problem**: `BufferPool::acquire()` does `new Buffer(std::move(buf))` — a heap
allocation for the Buffer object that the shared_ptr will manage, plus the
shared_ptr's own control block allocation. Every frame = 1-2 heap allocs.

**Fix**: Use `std::allocate_shared` with a pool allocator for the control block +
Buffer together (single allocation). In C++11, `std::allocate_shared` is available.

However, a simpler approach: pre-allocate the `shared_ptr<Buffer>` objects. Instead
of the pool holding `Buffer` objects and wrapping them in `shared_ptr` on acquire,
hold pre-built `shared_ptr<Buffer>` that get recycled. The challenge is the custom
deleter — it must return the buffer to the pool, which creates a circular reference.

**Simplest practical fix**: Replace `new Buffer(std::move(buf))` with placement
into a pre-allocated slab:

```cpp
// In BufferPool, keep a small fixed array of Buffer objects
struct Slot {
    Buffer buf;
    std::atomic<bool> in_use{false};
};
std::array<Slot, 8> slots_;  // fixed pool, no heap alloc needed

std::shared_ptr<Buffer> acquire() {
    for (auto& s : slots_) {
        bool expected = false;
        if (s.in_use.compare_exchange_strong(expected, true)) {
            s.buf.resize(bufferSize_);
            return std::shared_ptr<Buffer>(&s.buf, [&s](Buffer*) {
                s.in_use.store(false);
            });
        }
    }
    // fallback: heap alloc if all slots busy
    ...
}
```

This eliminates both the `new Buffer` and the `delete p` on the hot path.
`std::atomic<bool>` and `compare_exchange_strong` are C++11.

The `shared_ptr` control block itself still does a heap allocation though. To
fully eliminate that, use `std::allocate_shared` with a static pool allocator,
or switch to intrusive reference counting (more invasive change).

**Recommended approach**: The Slot-based pool above. It eliminates the Buffer
heap alloc. The shared_ptr control block alloc (~32-48 bytes) is harder to avoid
without significant refactoring. Accept it for now — it's a small fast-path
malloc.

**Files**: `common/buffer_pool.h`, `common/buffer_pool.cpp`

---

## 6. ~~Fix tv_timeout calculation bug~~ [DONE]

**Problem**: In `hu_aap_tra_recv` line 89:
```cpp
tv_timeout.tv_usec = tmo * 1000;
```
For `tmo=1000` (WiFi), this sets `tv_usec = 1,000,000` (= 1 second). But
`tv_usec` must be < 1,000,000. Combined with `tv_sec = 1`, the actual timeout
is ~2 seconds instead of 1.

**Fix**:
```cpp
tv_timeout.tv_sec  = tmo / 1000;
tv_timeout.tv_usec = (tmo % 1000) * 1000;
```

**Files**: `hu/hu_aap.cpp` line 88-89

---

## 7. Fix unaligned pointer accesses [LOW IMPACT / CORRECTNESS]

**Problem**: Multiple sites cast `byte*` to `uint16_t*`/`uint32_t*`/`uint64_t*`
and dereference. On ARM Cortex-A9, unaligned 64-bit access can trap or silently
corrupt. There are 18 such sites in `hu/hu_aap.cpp`.

**Fix**: Replace with `memcpy` + byte-swap. The compiler (even GCC 4.9) optimizes
`memcpy` of 2/4/8 bytes into a single load/store instruction on platforms that
support it, and uses safe byte-by-byte access on platforms that don't.

Helper functions (C++11 compatible):

```cpp
inline uint16_t read_be16(const byte* p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return be16toh(v);
}
inline uint64_t read_be64(const byte* p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return be64toh(v);
}
inline void write_be16(byte* p, uint16_t v) {
    v = htobe16(v);
    memcpy(p, &v, sizeof(v));
}
inline void write_be32(byte* p, uint32_t v) {
    v = htobe32(v);
    memcpy(p, &v, sizeof(v));
}
inline void write_be64(byte* p, uint64_t v) {
    v = htobe64(v);
    memcpy(p, &v, sizeof(v));
}
```

Then replace all 18 sites, e.g.:
- Line 1453: `be16toh(*((uint16_t*)&enc_buf[2]))` -> `read_be16(&enc_buf[2])`
- Line 861: `be64toh(*((uint64_t*)buf))` -> `read_be64(buf)`
- Line 285: `*((uint16_t*)&send_enc_buf[2]) = htobe16(bytes_read)` -> `write_be16(&send_enc_buf[2], bytes_read)`

**Files**: `hu/hu_aap.cpp`, `hu/hu_uti.h` (for helper definitions)

---

## 8. Replace select() with epoll in hu_thread_main [LOW IMPACT]

**Problem**: `select()` requires reinitializing `fd_set` and computing `maxfd`
every iteration. `epoll` is level-triggered by default and avoids this setup cost.

**Impact**: Negligible with only 2-3 fds. The `FD_ZERO` + `FD_SET` + `std::max`
overhead is a few dozen nanoseconds per iteration — dwarfed by the actual I/O and
SSL work.

**Fix**: Replace with `epoll_create1` + `epoll_ctl` (once at setup) +
`epoll_wait` (per iteration). All are available on the target Linux kernel.

```cpp
// Setup (once):
int epfd = epoll_create1(0);
struct epoll_event ev;
ev.events = EPOLLIN;
ev.data.fd = command_read_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, command_read_fd, &ev);
ev.data.fd = transportFD;
epoll_ctl(epfd, EPOLL_CTL_ADD, transportFD, &ev);
if (errorfd >= 0) {
    ev.data.fd = errorfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, errorfd, &ev);
}

// Loop:
struct epoll_event events[3];
int n = epoll_wait(epfd, events, 3, -1);
for (int i = 0; i < n; i++) {
    if (events[i].data.fd == errorfd) { ... }
    else if (events[i].data.fd == command_read_fd) { ... }
    else if (events[i].data.fd == transportFD) { ... }
}
```

**Verdict**: Optional. Only implement if doing the other changes anyway.

**Files**: `hu/hu_aap.cpp`

---

## Implementation Order

1. ~~**#4 logd short-circuit** — trivial, zero-risk, immediate benefit~~ [DONE]
2. ~~**#6 tv_timeout bug fix** — one-line fix, correctness~~ [DONE]
3. ~~**#1 eliminate double select** — small, high value ~~[DONE]
4. **#7 unaligned access fixes** — correctness on ARM, moderate effort
5. **#3 lock-free buffer pool acquire** — medium effort, medium value
6. **#5 avoid heap alloc in shared_ptr** — can combine with #3
7. **#2 SSL mutex** — already near-optimal, verify only
8. **#8 epoll** — optional, minimal gain
