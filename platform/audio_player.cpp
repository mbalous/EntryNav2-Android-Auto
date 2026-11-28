// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "audio_player.hpp"

#include <errno.h>
#include <unistd.h>

#include "aa_protocol.hpp"
#include "audio_route.hpp"
#include "evo_log.hpp"

namespace evo {

AudioPlayer::AudioPlayer()
    : route_(nullptr), thread_(0), started_(false), quit_(false), inCall_(false), generation_(0), failure_(0) {
    active_[0] = active_[1] = false;
    seen_[0] = seen_[1] = false;
    sessions_[0] = sessions_[1] = configs_[0] = configs_[1] = 0;
    lastData_[0] = lastData_[1] = 0;
    pthread_mutex_init(&mu_, nullptr);
    pthread_cond_init(&cv_, nullptr);
}

AudioPlayer::~AudioPlayer() {
    stop();
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
}

bool AudioPlayer::start(const char* pcm, const char* library) {
    if (started_) {
        ELOG("AudioPlayer", "already started");
        return false;
    }
    if (!output_.open(pcm, library)) {
        return false;
    }
    pthread_mutex_lock(&mu_);
    route_ = nullptr;
    pthread_mutex_unlock(&mu_);
    return startWorker();
}

bool AudioPlayer::startRouted(AudioRoute& route, const char* mediaPcm, const char* promptPcm, const char* library) {
    if (started_) {
        ELOG("AudioPlayer", "already started");
        return false;
    }
    if (!output_.open(mediaPcm, library)) {
        return false;
    }
    if (!promptOutput_.open(promptPcm, library, 1)) {
        output_.close();
        return false;
    }
    pthread_mutex_lock(&mu_);
    route_ = &route;
    pthread_mutex_unlock(&mu_);
    return startWorker();
}

bool AudioPlayer::startWorker() {
    quit_ = false;
    failure_.store(0);
    if (pthread_create(&thread_, nullptr, threadMain, this) != 0) {
        ELOG("AudioPlayer", "cannot start playback worker");
        output_.close();
        promptOutput_.close();
        pthread_mutex_lock(&mu_);
        route_ = nullptr;
        pthread_mutex_unlock(&mu_);
        return false;
    }
    started_ = true;
    return true;
}

void AudioPlayer::stop() {
    if (!started_) {
        return;
    }
    pthread_mutex_lock(&mu_);
    quit_ = true;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
    pthread_join(thread_, nullptr);
    started_ = false;
    pthread_mutex_lock(&mu_);
    if (route_) {
        route_->request(0);
    }
    route_ = nullptr;
    active_[0] = active_[1] = false;
    seen_[0] = seen_[1] = false;
    mixer_.clearMedia();
    mixer_.clearSpeech();
    pthread_mutex_unlock(&mu_);
    output_.close();
    promptOutput_.close();
}

void AudioPlayer::setCall(bool active) {
    pthread_mutex_lock(&mu_);
    if (inCall_ != active) {
        inCall_ = active;
        mixer_.clearMedia();
        mixer_.clearSpeech();
        seen_[0] = seen_[1] = false;
        ++generation_;
        if (active && route_) {
            route_->prohibitRestore();
            route_->request(0);
        }
        pthread_cond_signal(&cv_);
    }
    pthread_mutex_unlock(&mu_);
}

void AudioPlayer::stream(uint8_t channel, uint32_t session, uint32_t config, bool started, void* user) {
    AudioPlayer* self = static_cast<AudioPlayer*>(user);
    if (!self || !aa::channel::isAudioSink(channel)) {
        ELOG("AudioPlayer", "invalid stream callback");
        return;
    }
    const unsigned index = channel == aa::channel::kMediaAudio ? 1 : 0;
    pthread_mutex_lock(&self->mu_);
    self->active_[index] = started;
    self->seen_[index] = false;
    self->sessions_[index] = session;
    self->configs_[index] = config;
    if (index) {
        self->mixer_.clearMedia();
    } else {
        self->mixer_.clearSpeech();
    }
    ++self->generation_;
    pthread_cond_signal(&self->cv_);
    pthread_mutex_unlock(&self->mu_);
}

void AudioPlayer::data(uint8_t channel, uint32_t session, const uint8_t* pcm, size_t bytes, uint64_t, void* user) {
    AudioPlayer* self = static_cast<AudioPlayer*>(user);
    if (!self || !aa::channel::isAudioSink(channel)) {
        ELOG("AudioPlayer", "invalid PCM callback");
        return;
    }
    const unsigned index = channel == aa::channel::kMediaAudio ? 1 : 0;
    if (pthread_mutex_trylock(&self->mu_) != 0) {
        ELOG("AudioPlayer", "PCM dropped while playback buffer is busy");
        return;
    }
    if (self->inCall_) {
        pthread_mutex_unlock(&self->mu_);
        return;
    }
    if (self->active_[index] && self->sessions_[index] == session) {
        const bool ok =
            index ? self->mixer_.pushMedia(pcm, bytes) : self->mixer_.pushSpeech(pcm, bytes, self->configs_[index]);
        if (ok) {
            self->lastData_[index] = monoMs();
            self->seen_[index] = true;
            pthread_cond_signal(&self->cv_);
        } else {
            ELOG("AudioPlayer", "invalid PCM for channel %u", channel);
        }
    } else {
        ELOG("AudioPlayer", "PCM for inactive stream on channel %u", channel);
    }
    pthread_mutex_unlock(&self->mu_);
}

void* AudioPlayer::threadMain(void* user) {
    static_cast<AudioPlayer*>(user)->run();
    return nullptr;
}

void AudioPlayer::run() {
    int16_t samples[480 * 2];
    size_t count = 0, offset = 0;
    uint32_t generation = 0;
    uint32_t stalledSince = 0;
    bool stalled = false;
    uint16_t selected = 0;
    for (;;) {
        pthread_mutex_lock(&mu_);
        if (generation != generation_) {
            count = offset = 0;
            generation = generation_;
            stalled = false;
            const bool quit = quit_;
            const bool routed = route_ != nullptr;
            pthread_mutex_unlock(&mu_);
            if (quit) {
                break;
            }
            if (!output_.flush() || (routed && !promptOutput_.flush())) {
                failure_.store(-EIO);
                break;
            }
            continue;
        }
        if (route_) {
            const uint32_t now = monoMs();
            uint16_t wanted = 0;
            if (!inCall_) {
                if (mixer_.mediaFrames() || (active_[1] && seen_[1] && now - lastData_[1] < 1000)) {
                    wanted = 0x315;
                } else if (mixer_.speechFrames() || (active_[0] && seen_[0] && now - lastData_[0] < 1000)) {
                    wanted = 0x31A;
                }
            }
            route_->request(wanted);
            if (route_->failure()) {
                failure_.store(-EIO);
                pthread_mutex_unlock(&mu_);
                break;
            }
            if (selected != wanted) {
                selected = wanted;
                count = offset = 0;
                stalled = false;
                pthread_mutex_unlock(&mu_);
                if (!output_.flush() || !promptOutput_.flush()) {
                    failure_.store(-EIO);
                    break;
                }
                continue;
            }
            if (!wanted || !route_->owns(wanted)) {
                timespec until;
                clock_gettime(CLOCK_REALTIME, &until);
                until.tv_nsec += 20000000;
                if (until.tv_nsec >= 1000000000) {
                    ++until.tv_sec;
                    until.tv_nsec -= 1000000000;
                }
                if (!quit_) {
                    pthread_cond_timedwait(&cv_, &mu_, &until);
                }
                const bool quit = quit_;
                pthread_mutex_unlock(&mu_);
                if (quit) {
                    break;
                }
                continue;
            }
        }
        while (!quit_ && offset == count) {
            count = mixer_.read(samples, 480, !route_ || selected == 0x315, true);
            offset = 0;
            if (count) {
                if (route_ && selected == 0x31A) {
                    for (size_t i = 0; i < count; ++i) {
                        samples[i] = samples[i * 2];
                    }
                }
                break;
            }
            stalled = false;
            if (route_) {
                break;
            }
            pthread_cond_wait(&cv_, &mu_);
            if (generation != generation_) {
                break;
            }
        }
        const bool quit = quit_;
        pthread_mutex_unlock(&mu_);
        if (quit) {
            break;
        }
        if (offset == count) {
            usleep(10000);
            continue;
        }
        const bool prompt = route_ && selected == 0x31A;
        const long written = prompt ? promptOutput_.write(samples + offset, count - offset)
                                    : output_.write(samples + offset * 2, count - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            stalled = false;
        } else if (written == 0 || written == -EAGAIN) {
            const uint32_t now = monoMs();
            if (!stalled) {
                stalled = true;
                stalledSince = now;
            } else if (now - stalledSince >= 2000) {
                failure_.store(-ETIMEDOUT);
                ELOG("AudioPlayer", "playback stalled for 2 seconds");
                break;
            }
            usleep(10000);
        } else {
            failure_.store(static_cast<int>(written));
            ELOG("AudioPlayer", "playback stopped: error %ld", written);
            break;
        }
    }
}

}  // namespace evo
