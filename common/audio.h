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

struct AudioCommand
{
    enum Type { Data, Flush };
    Type type = Data;
    std::vector<uint8_t> data;
};

class AlsaWriter : public BufferProcessor<AudioCommand>
{
    snd_pcm_t* handle_;
    std::string name_;

protected:
    void onStarted() override;
    void onStopping() override;
    void process(AudioCommand& cmd) override;

public:
    AlsaWriter(snd_pcm_t* handle, const char* name);
    void write(const byte* buf, int len);
    void write(std::vector<uint8_t>&& data);
    void flush();
};

class AudioOutput
{
    snd_pcm_t* aud_handle = nullptr;
    snd_pcm_t* au1_handle = nullptr;

    AlsaWriter* aud_writer = nullptr;
    AlsaWriter* au1_writer = nullptr;

    std::vector<uint8_t> MonoToStereoLeft(const byte *buf, int len);
public:
    AudioOutput(const char* outDev = "default");
    ~AudioOutput();

    void MediaPacketAUD(uint64_t timestamp, const byte * buf, int len);
    void MediaPacketAU1(uint64_t timestamp, const byte * buf, int len);
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
