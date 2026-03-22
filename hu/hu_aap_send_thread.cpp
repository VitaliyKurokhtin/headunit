#define LOGTAG "hu_send"
#include "hu_aap_send_thread.h"

HUSenderThread::HUSenderThread(SendFunc sendFunc)
    : sendFunc_(std::move(sendFunc))
{
}

void HUSenderThread::enqueue(int retry, int chan, const byte* buf, int len, int overrideTimeout)
{
    SendRequest req;
    req.retry = retry;
    req.chan = chan;
    req.overrideTimeout = overrideTimeout;
    req.data.assign(buf, buf + len);

    enqueueEntry(std::move(req));
}

void HUSenderThread::enqueue(int retry, int chan, std::shared_ptr<std::vector<uint8_t>> buf, int overrideTimeout, int dataLen)
{
    SendRequest req;
    req.retry = retry;
    req.chan = chan;
    req.overrideTimeout = overrideTimeout;
    req.dataLen = dataLen;
    req.shared_data = std::move(buf);

    enqueueEntry(std::move(req));
}

void HUSenderThread::process(SendRequest& req)
{
    int ret = sendFunc_(req.retry, req.chan, req.buf(), req.size(), req.overrideTimeout);
    if (ret < 0) {
        loge("sender thread: send failed ret=%d chan=%d len=%zu", ret, req.chan, req.size());
    }
}
