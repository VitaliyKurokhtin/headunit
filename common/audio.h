#pragma once

#include <glib.h>
#include <stdio.h>
#include <asoundlib.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

#include "hu_uti.h"
#include "hu_aap.h"
#include "buffer_processor.h"
#include "buffer_pool.h"

struct AudioCommand
{
    enum Type { Data, Flush };
    Type type = Data;
    std::vector<uint8_t> data;
    // Zero-copy: keeps pool buffer alive until processing is done
    std::shared_ptr<std::vector<uint8_t>> pool_buf;
    int pool_offset = 0;
    int pool_len = 0;

    const uint8_t* audio_data() const { return pool_buf ? pool_buf->data() + pool_offset : data.data(); }
    int audio_size() const { return pool_buf ? pool_len : (int)data.size(); }
};

class AlsaWriter : public BufferProcessor<AudioCommand>
{
    snd_pcm_t* handle_;
    std::string name_;
    bool mono_to_stereo_;
    std::unique_ptr<BufferPool> stereo_pool_;  // Allocated only when mono_to_stereo_ is true

    int applyMonoToStereo(const uint8_t* in, int in_len, std::vector<uint8_t>& out);

protected:
    void onStarted() override;
    void onStopping() override;
    void process(AudioCommand& cmd) override;

public:
    AlsaWriter(snd_pcm_t* handle, const char* name, bool mono_to_stereo = false);
    void write(const byte* buf, int len);
    void write(std::vector<uint8_t>&& data);
    void write(std::shared_ptr<std::vector<uint8_t>> buf, int offset, int len);
    void flush();
};

class AudioOutput
{
    snd_pcm_t* aud_handle = nullptr;
    snd_pcm_t* au1_handle = nullptr;

    AlsaWriter* aud_writer = nullptr;
    AlsaWriter* au1_writer = nullptr;

public:
    AudioOutput(const char* outDev = "default");
    ~AudioOutput();

    void MediaPacketAUD(uint64_t timestamp, const byte * buf, int len);
    void MediaPacketAU1(uint64_t timestamp, const byte * buf, int len);
    void MediaPacketAUD(uint64_t timestamp, std::shared_ptr<std::vector<uint8_t>> buf, int offset, int len);
    void MediaPacketAU1(uint64_t timestamp, std::shared_ptr<std::vector<uint8_t>> buf, int offset, int len);
    void FlushAUD();
    void FlushAU1();
};

class MicInput
{
    std::string micDevice;
    size_t bufferStartPadding;
    std::thread mic_readthread;
    int cancelPipeRead = -1, cancelPipeWrite = -1;

    snd_pcm_sframes_t read_mic_cancelable(snd_pcm_t* mic_handle, void *buffer, snd_pcm_uframes_t size, bool* canceled);
    void MicThreadMain(IHUAnyThreadInterface* threadInterface);
public:
    MicInput(const std::string& micDevice = "default", size_t bufferStartPadding = 0);
    ~MicInput();

    void Start(IHUAnyThreadInterface* threadInterface);
    void Stop();
};
