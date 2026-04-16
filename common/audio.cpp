#include "audio.h"
#include "buffer_pool.h"

// --- AlsaWriter ---

AlsaWriter::AlsaWriter(snd_pcm_t* handle, const char* name, NavAudioChannel nav_channel)
    : handle_(handle), name_(name), nav_channel_(nav_channel)
{
    if (nav_channel_ != NavAudioChannel::STEREO)
        stereo_pool_.reset(new BufferPool(8192, 1, true, "mono-to-stereo"));
}

void AlsaWriter::onStarted()
{
    pthread_setname_np(pthread_self(), name_.c_str());
}

void AlsaWriter::onStopping()
{
    if (handle_) snd_pcm_drop(handle_);
}

int AlsaWriter::applyMonoToStereo(const uint8_t* in, int in_len, std::vector<uint8_t>& out)
{
    int out_len = in_len * 2;
    if ((int)out.size() < out_len)
        out.resize(out_len);
    int sampleCount = in_len / sizeof(int16_t);
    const int16_t* monoSamples = reinterpret_cast<const int16_t*>(in);
    int16_t* stereoSamples = reinterpret_cast<int16_t*>(out.data());
    for (int i = 0; i < sampleCount; i++) {
        if (nav_channel_ == NavAudioChannel::RIGHT) {
            stereoSamples[i * 2] = 0;
            stereoSamples[i * 2 + 1] = monoSamples[i];
        } else {
            stereoSamples[i * 2] = monoSamples[i];
            stereoSamples[i * 2 + 1] = 0;
        }
    }
    return out_len;
}

void AlsaWriter::process(AudioCommand& cmd)
{
    if (cmd.type == AudioCommand::Flush) {
        snd_pcm_drop(handle_);
        snd_pcm_prepare(handle_);
        return;
    }

    const uint8_t* pcm_data = cmd.audio_data();
    int pcm_size = cmd.audio_size();

    std::shared_ptr<std::vector<uint8_t>> stereo_buf;
    if (nav_channel_ != NavAudioChannel::STEREO) {
        stereo_buf = stereo_pool_->acquire();
        pcm_size = applyMonoToStereo(pcm_data, pcm_size, *stereo_buf);
        pcm_data = stereo_buf->data();
    }

    snd_pcm_sframes_t framecount = snd_pcm_bytes_to_frames(handle_, pcm_size);

    // Check for accumulated latency and resync if needed
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(handle_, &delay) == 0 && delay > framecount * 4) {
        logw("%s: latency drift detected (%ld frames buffered, threshold %ld), resyncing\n",
             name_.c_str(), (long)delay, (long)(framecount * 4));
        snd_pcm_drop(handle_);
        snd_pcm_prepare(handle_);
    }

    snd_pcm_sframes_t written = 0;
    while (written < framecount) {
        snd_pcm_sframes_t frames = snd_pcm_writei(handle_, pcm_data + snd_pcm_frames_to_bytes(handle_, written), framecount - written);
        if (frames < 0) {
            frames = snd_pcm_recover(handle_, frames, 1);
            if (frames < 0) {
                loge("Write error: %s\n", snd_strerror(frames));
                break;
            }
            continue;
        }
        written += frames;
        if (written < framecount) {
            logw("%s: short write: %ld of %ld frames\n", name_.c_str(), (long)written, (long)framecount);
        }
    }
}

void AlsaWriter::write(std::shared_ptr<std::vector<uint8_t>> buf, int offset, int len)
{
    AudioCommand cmd;
    cmd.type = AudioCommand::Data;
    cmd.pool_buf = std::move(buf);
    cmd.pool_offset = offset;
    cmd.pool_len = len;
    enqueueEntry(std::move(cmd));
}

void AlsaWriter::flush()
{
    clearQueue();
    snd_pcm_drop(handle_);
    AudioCommand cmd;
    cmd.type = AudioCommand::Flush;
    enqueueEntry(std::move(cmd));
}

// --- AudioOutput ---

AudioOutput::AudioOutput(const char *outDev, NavAudioChannel nav_channel)
{
    logi("snd_asoundlib_version: %s", snd_asoundlib_version());
    logd("Device name %s\n", outDev);
    int err = 0;
    if ((err = snd_pcm_open(&aud_handle, outDev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        loge("Playback open error: %s\n", snd_strerror(err));
    }
    if ((err = snd_pcm_set_params(aud_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2,48000, 1, 50000)) < 0) {   /* 0.05sec */
        loge("Playback open error: %s\n", snd_strerror(err));
    }

    if ((err = snd_pcm_prepare(aud_handle)) < 0) {
        loge("snd_pcm_prepare error: %s\n", snd_strerror(err));
    }

    if ((err = snd_pcm_open(&au1_handle, outDev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        loge("Playback open error: %s\n", snd_strerror(err));
    }
    if ((err = snd_pcm_set_params(au1_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 16000, 1, 50000)) < 0) {   /* 0.05sec */
        loge("Playback open error: %s\n", snd_strerror(err));
    }

    if ((err = snd_pcm_prepare(au1_handle)) < 0) {
        loge("snd_pcm_prepare error: %s\n", snd_strerror(err));
    }

    if (aud_handle) {
        aud_writer = new AlsaWriter(aud_handle, "aud_writer");
        aud_writer->start();
    }
    if (au1_handle) {
        au1_writer = new AlsaWriter(au1_handle, "au1_writer", nav_channel);
        au1_writer->start();
    }
}

AudioOutput::~AudioOutput()
{
    delete aud_writer;
    delete au1_writer;

    if (aud_handle) snd_pcm_close(aud_handle);
    if (au1_handle) snd_pcm_close(au1_handle);
}

void AudioOutput::MediaPacketAUD(uint64_t timestamp, std::shared_ptr<std::vector<uint8_t>> buf, int offset, int len)
{
    if (aud_writer) aud_writer->write(std::move(buf), offset, len);
}

void AudioOutput::MediaPacketAU1(uint64_t timestamp, std::shared_ptr<std::vector<uint8_t>> buf, int offset, int len)
{
    if (au1_writer) au1_writer->write(std::move(buf), offset, len);
}

void AudioOutput::FlushAUD()
{
    if (aud_writer) aud_writer->flush();
}

void AudioOutput::FlushAU1()
{
    if (au1_writer) au1_writer->flush();
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

    if ((err = snd_pcm_set_params(mic_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1,16000, 1, 50000)) < 0)
    {   /* 0.05sec */
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

    snd_pcm_uframes_t buffer_size, period_size;
    snd_pcm_get_params(mic_handle, &buffer_size, &period_size);
    const size_t tempSize = snd_pcm_frames_to_bytes(mic_handle, period_size);
    const snd_pcm_sframes_t bufferFrameCount = period_size;
    BufferPool pool(bufferStartPadding + tempSize, 2, false, "mic");
    bool canceled = false;
    while(!canceled)
    {
        auto tempBuffer = pool.acquire();
        snd_pcm_sframes_t frames = read_mic_cancelable(mic_handle, tempBuffer->data() + bufferStartPadding, bufferFrameCount, &canceled);
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
                    frames = read_mic_cancelable(mic_handle, tempBuffer->data() + bufferStartPadding, bufferFrameCount, &canceled);
                }
            }

            if (frames < 0)
            {
                canceled = true;
                continue;
            }
        }
        ssize_t bytesRead = snd_pcm_frames_to_bytes(mic_handle, frames);
        //doesn't seem like the timestamp is used so pass 0
        threadInterface->hu_queue_enc_send_media_packet(AA_CH_MIC, HU_PROTOCOL_MESSAGE::MediaDataWithTimestamp, 0, std::move(tempBuffer), bytesRead);
    }

    if ((err = snd_pcm_drop(mic_handle)) < 0)
    {
        loge("snd_pcm_drop: %s\n", snd_strerror(err));
    }

    snd_pcm_close(mic_handle);
}

MicInput::MicInput(const std::string& micDevice, size_t bufferStartPadding) : micDevice(micDevice), bufferStartPadding(bufferStartPadding)
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
