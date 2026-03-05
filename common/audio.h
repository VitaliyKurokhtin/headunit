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

class AudioOutput
{
    struct AudioChannel {
        std::queue<std::vector<uint8_t>> queue;
        std::mutex mutex;
        std::condition_variable cv;
        bool flush_requested = false;
    };

    snd_pcm_t* aud_handle = nullptr;
    snd_pcm_t* au1_handle = nullptr;

    AudioChannel aud_channel;
    AudioChannel au1_channel;

    std::atomic<bool> quit_flag{false};

    std::thread aud_writer_thread;
    std::thread au1_writer_thread;

    void WriterThread(snd_pcm_t* handle, AudioChannel& channel, const char* name);
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
    std::thread mic_readthread;
    int cancelPipeRead = -1, cancelPipeWrite = -1;

    snd_pcm_sframes_t read_mic_cancelable(snd_pcm_t* mic_handle, void *buffer, snd_pcm_uframes_t size, bool* canceled);
    void MicThreadMain(IHUAnyThreadInterface* threadInterface);
public:
    MicInput(const std::string& micDevice = "default");
    ~MicInput();

    void Start(IHUAnyThreadInterface* threadInterface);
    void Stop();
};
