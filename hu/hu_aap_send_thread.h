#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <memory>

#include "hu_uti.h"
#include "buffer_processor.h"

struct SendRequest {
    int retry;
    int chan;
    int overrideTimeout;
    std::vector<uint8_t> data;                          // used when copied
    std::shared_ptr<std::vector<uint8_t>> shared_data;  // used when shared

    byte* buf()    { return shared_data ? shared_data->data() : data.data(); }
    size_t size()  { return shared_data ? shared_data->size() : data.size(); }
};

class HUSenderThread : public BufferProcessor<SendRequest>
{
public:
    using SendFunc = std::function<int(int retry, int chan, byte* buf, int len, int overrideTimeout)>;

    explicit HUSenderThread(SendFunc sendFunc);

    // Enqueue a buffer to be sent. The data is copied; caller retains ownership.
    void enqueue(int retry, int chan, const byte* buf, int len, int overrideTimeout);

    // Enqueue a shared buffer. No copy — shared_ptr keeps data alive until sent.
    void enqueue(int retry, int chan, std::shared_ptr<std::vector<uint8_t>> buf, int overrideTimeout);

protected:
    void process(SendRequest& entry) override;

private:
    SendFunc sendFunc_;
};
