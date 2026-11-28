// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// A bounded playback worker; native source routing is managed separately.
#ifndef ENTRY_AAD_PLATFORM_AUDIO_PLAYER_HPP
#define ENTRY_AAD_PLATFORM_AUDIO_PLAYER_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "audio_mixer.hpp"
#include "audio_output.hpp"

namespace evo {

class AudioRoute;

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Called before session discovery. False leaves the caller free to select Bluetooth instead.
    bool start(const char* pcm, const char* library = "libasound.so.2");
    bool startRouted(AudioRoute& route, const char* mediaPcm, const char* promptPcm,
                     const char* library = "libasound.so.2");
    void stop();
    int failure() const { return failure_.load(); }
    void setCall(bool active);

    // The session callbacks copy to bounded buffers; no ALSA operation runs on the session thread.
    static void data(uint8_t channel, uint32_t session, const uint8_t* pcm, size_t bytes, uint64_t pts, void* user);
    static void stream(uint8_t channel, uint32_t session, uint32_t config, bool started, void* user);

private:
    static void* threadMain(void* user);
    void run();
    bool startWorker();

    AudioOutput output_;
    AudioOutput promptOutput_;
    AudioMixer mixer_;
    AudioRoute* route_;
    pthread_mutex_t mu_;
    pthread_cond_t cv_;
    pthread_t thread_;
    bool started_, quit_, active_[2], inCall_, seen_[2];
    uint32_t sessions_[2], configs_[2], generation_, lastData_[2];
    std::atomic<int> failure_;
};

}  // namespace evo

#endif
