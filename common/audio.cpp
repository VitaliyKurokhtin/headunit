#include "audio.h"

AudioOutput::AudioOutput(const char *outDev)
{
    printf("snd_asoundlib_version: %s\n", snd_asoundlib_version());
    logd("Device name %s\n", outDev);
    int err = 0;
    if ((err = snd_pcm_open(&aud_handle, outDev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        loge("Playback open error: %s\n", snd_strerror(err));
    }
    if ((err = snd_pcm_set_params(aud_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2,48000, 1, 200000)) < 0) {   /* 0.2sec */
        loge("Playback open error: %s\n", snd_strerror(err));
    }

    if ((err = snd_pcm_prepare(aud_handle)) < 0) {
        loge("snd_pcm_prepare error: %s\n", snd_strerror(err));
    }

    if ((err = snd_pcm_open(&au1_handle, outDev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        loge("Playback open error: %s\n", snd_strerror(err));
    }
    if ((err = snd_pcm_set_params(au1_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 16000, 1, 200000)) < 0) {   /* 0.2sec */
        loge("Playback open error: %s\n", snd_strerror(err));
    }

    if ((err = snd_pcm_prepare(au1_handle)) < 0) {
        loge("snd_pcm_prepare error: %s\n", snd_strerror(err));
    }

    // Start writer threads so ALSA writes don't block the HU protocol thread
    if (aud_handle) {
        aud_writer_thread = std::thread(&AudioOutput::WriterThread, this, aud_handle, std::ref(aud_channel), "aud_writer");
    }
    if (au1_handle) {
        au1_writer_thread = std::thread(&AudioOutput::WriterThread, this, au1_handle, std::ref(au1_channel), "au1_writer");
    }
}

AudioOutput::~AudioOutput()
{
    quit_flag.store(true);

    // Interrupt any blocked snd_pcm_writei calls
    if (aud_handle) snd_pcm_drop(aud_handle);
    if (au1_handle) snd_pcm_drop(au1_handle);

    aud_channel.cv.notify_one();
    au1_channel.cv.notify_one();

    if (aud_writer_thread.joinable()) aud_writer_thread.join();
    if (au1_writer_thread.joinable()) au1_writer_thread.join();

    if (aud_handle) snd_pcm_close(aud_handle);
    if (au1_handle) snd_pcm_close(au1_handle);
}

void AudioOutput::WriterThread(snd_pcm_t* handle, AudioChannel& channel, const char* name)
{
    pthread_setname_np(pthread_self(), name);

    while (!quit_flag.load())
    {
        std::vector<uint8_t> packet;
        {
            std::unique_lock<std::mutex> lk(channel.mutex);
            channel.cv.wait(lk, [&] {
                return quit_flag.load() || channel.flush_requested || !channel.queue.empty();
            });

            if (quit_flag.load()) break;

            if (channel.flush_requested) {
                std::queue<std::vector<uint8_t>>().swap(channel.queue);
                channel.flush_requested = false;
                snd_pcm_prepare(handle);
                continue;
            }

            packet = std::move(channel.queue.front());
            channel.queue.pop();
        }

        // Write to ALSA outside the lock
        snd_pcm_sframes_t framecount = snd_pcm_bytes_to_frames(handle, packet.size());

        // Check for accumulated latency and resync if needed
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(handle, &delay) == 0 && delay > framecount * 4) {
            logw("%s: latency drift detected (%ld frames buffered, threshold %ld), resyncing\n",
                 name, (long)delay, (long)(framecount * 4));
            snd_pcm_drop(handle);
            snd_pcm_prepare(handle);
        }

        snd_pcm_sframes_t frames = snd_pcm_writei(handle, packet.data(), framecount);
        if (frames < 0) {
            frames = snd_pcm_recover(handle, frames, 1);
            if (frames >= 0) {
                frames = snd_pcm_writei(handle, packet.data(), framecount);
            }
        }
        if (frames >= 0 && frames < framecount) {
            loge("Short write (expected %i, wrote %i)\n", (int)framecount, (int)frames);
        }
    }
}

void AudioOutput::MediaPacketAUD(uint64_t timestamp, const byte *buf, int len)
{
    if (!aud_handle) return;
    {
        std::lock_guard<std::mutex> lk(aud_channel.mutex);
        aud_channel.queue.emplace(buf, buf + len);
    }
    aud_channel.cv.notify_one();
}

std::vector<uint8_t> AudioOutput::MonoToStereoLeft(const byte *buf, int len)
{
    int sampleCount = len / sizeof(int16_t);
    std::vector<uint8_t> stereoData(sampleCount * 2 * sizeof(int16_t));
    const int16_t* monoSamples = reinterpret_cast<const int16_t*>(buf);
    int16_t* stereoSamples = reinterpret_cast<int16_t*>(stereoData.data());
    for (int i = 0; i < sampleCount; i++) {
        stereoSamples[i * 2] = monoSamples[i];  // left
        stereoSamples[i * 2 + 1] = 0;           // right (silent)
    }
    return stereoData;
}

void AudioOutput::MediaPacketAU1(uint64_t timestamp, const byte *buf, int len)
{
    if (!au1_handle) return;

    auto stereoData = MonoToStereoLeft(buf, len);
    {
        std::lock_guard<std::mutex> lk(au1_channel.mutex);
        au1_channel.queue.push(std::move(stereoData));
    }
    au1_channel.cv.notify_one();
}

void AudioOutput::FlushAUD()
{
    if (!aud_handle) return;
    {
        std::lock_guard<std::mutex> lk(aud_channel.mutex);
        aud_channel.flush_requested = true;
        std::queue<std::vector<uint8_t>>().swap(aud_channel.queue);
    }
    snd_pcm_drop(aud_handle);
    aud_channel.cv.notify_one();
}

void AudioOutput::FlushAU1()
{
    if (!au1_handle) return;
    {
        std::lock_guard<std::mutex> lk(au1_channel.mutex);
        au1_channel.flush_requested = true;
        std::queue<std::vector<uint8_t>>().swap(au1_channel.queue);
    }
    snd_pcm_drop(au1_handle);
    au1_channel.cv.notify_one();
}

snd_pcm_sframes_t MicInput::read_mic_cancelable(snd_pcm_t* mic_handle, void *buffer, snd_pcm_uframes_t size, bool* canceled)
{
    int pollfdAllocCount = snd_pcm_poll_descriptors_count(mic_handle);
    struct pollfd* pfds = (struct pollfd*)alloca((pollfdAllocCount + 1) * sizeof(struct pollfd));
    unsigned short* revents = (unsigned short*)alloca(pollfdAllocCount * sizeof(unsigned short));

    int polldescs = snd_pcm_poll_descriptors(mic_handle, pfds, pollfdAllocCount);
    pfds[polldescs].fd = cancelPipeRead;
    pfds[polldescs].events = POLLIN;
    pfds[polldescs].revents = 0;

    *canceled = false;
    while (true)
    {
        if (poll(pfds, polldescs+1, -1) <= 0)
        {
            loge("poll failed");
            break;
        }

        if (pfds[polldescs].revents & POLLIN)
        {
            unsigned char bogusByte;
            read(cancelPipeRead, &bogusByte, 1);

            *canceled = true;
            return 0;
        }
        unsigned short audioEvents = 0;
        snd_pcm_poll_descriptors_revents(mic_handle, pfds, polldescs, &audioEvents);

        if (audioEvents & POLLIN)
        {
            //got it
            break;
        }
    }
    snd_pcm_sframes_t ret = snd_pcm_readi(mic_handle, buffer, size);
    return ret;
}

void MicInput::MicThreadMain(IHUAnyThreadInterface* threadInterface)
{
    pthread_setname_np(pthread_self(), "mic_thread");

    snd_pcm_t* mic_handle = nullptr;

    int err = 0;
    if ((err = snd_pcm_open(&mic_handle, micDevice.c_str(), SND_PCM_STREAM_CAPTURE,  SND_PCM_NONBLOCK)) < 0)
    {
        loge("Playback open error: %s\n", snd_strerror(err));
        return;
    }

    if ((err = snd_pcm_set_params(mic_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1,16000, 1, 250000)) < 0)
    {   /* 0.25sec */
        loge("Playback open error: %s\n", snd_strerror(err));
        snd_pcm_close(mic_handle);
        return;
    }
    if ((err = snd_pcm_prepare(mic_handle)) < 0)
    {
        loge("snd_pcm_prepare: %s\n", snd_strerror(err));
        snd_pcm_close(mic_handle);
        return;
    }

    if ((err = snd_pcm_start(mic_handle)) < 0)
    {
        loge("snd_pcm_start: %s\n", snd_strerror(err));
        snd_pcm_close(mic_handle);
        return;
    }

    const size_t tempSize = 1024*1024;
    const snd_pcm_sframes_t bufferFrameCount = snd_pcm_bytes_to_frames(mic_handle, tempSize);
    bool canceled = false;
    while(!canceled)
    {
        uint8_t* tempBuffer = new uint8_t[tempSize];
        snd_pcm_sframes_t frames = read_mic_cancelable(mic_handle, tempBuffer, bufferFrameCount, &canceled);
        if (frames < 0)
        {
            if (frames == -ESTRPIPE)
            {
                frames = snd_pcm_recover(mic_handle, frames, 0);
                if (frames < 0)
                {
                    loge("recover failed");
                }
                else
                {
                    frames = read_mic_cancelable(mic_handle, tempBuffer, bufferFrameCount, &canceled);
                }
            }

            if (frames < 0)
            {
                delete [] tempBuffer;
                canceled = true;
                continue;
            }
        }
        ssize_t bytesRead = snd_pcm_frames_to_bytes(mic_handle, frames);
        threadInterface->hu_queue_command([tempBuffer, bytesRead](IHUConnectionThreadInterface& s)
        {
            //doesn't seem like the timestamp is used so pass 0
            s.hu_aap_enc_send_media_packet(1, AA_CH_MIC, HU_PROTOCOL_MESSAGE::MediaDataWithTimestamp, 0, tempBuffer, bytesRead);
            delete [] tempBuffer;
        });
    }

    if ((err = snd_pcm_drop(mic_handle)) < 0)
    {
        loge("snd_pcm_drop: %s\n", snd_strerror(err));
    }

    snd_pcm_close(mic_handle);
}

MicInput::MicInput(const std::string& micDevice) : micDevice(micDevice)
{
    int cancelPipe[2];
    if (pipe(cancelPipe) < 0)
    {
        loge("pipe failed");
    }
    cancelPipeRead = cancelPipe[0];
    cancelPipeWrite = cancelPipe[1];
}

MicInput::~MicInput()
{
    Stop();

    close(cancelPipeRead);
    close(cancelPipeWrite);
}

void MicInput::Start(IHUAnyThreadInterface* threadInterface)
{
    if (!mic_readthread.joinable())
    {
        mic_readthread = std::thread([this, threadInterface](){ MicThreadMain(threadInterface);});
    }
}

void MicInput::Stop()
{
    if (mic_readthread.joinable())
    {
        //write single byte
        write(cancelPipeWrite, &cancelPipeWrite, 1);
        mic_readthread.join();
    }
}
