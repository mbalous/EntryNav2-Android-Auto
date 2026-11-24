// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "audio_output.hpp"

#include <dlfcn.h>
#include <errno.h>

#include "evo_log.hpp"

namespace evo {

AudioOutput::AudioOutput()
    : lib_(nullptr),
      pcm_(nullptr),
      open_(nullptr),
      params_(nullptr),
      write_(nullptr),
      recover_(nullptr),
      drop_(nullptr),
      prepare_(nullptr),
      state_(nullptr),
      start_(nullptr),
      close_(nullptr),
      error_(nullptr) {}

AudioOutput::~AudioOutput() {
    close();
}

bool AudioOutput::open(const char* pcm, const char* library, unsigned channels) {
    close();
    if (!pcm || !*pcm || !library || !*library || (channels != 1 && channels != 2)) {
        ELOG("AudioOutput", "missing PCM or ALSA library name");
        return false;
    }
    lib_ = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
        ELOG("AudioOutput", "cannot load ALSA: %s", dlerror());
        return false;
    }
    *reinterpret_cast<void**>(&open_) = dlsym(lib_, "snd_pcm_open");
    *reinterpret_cast<void**>(&params_) = dlsym(lib_, "snd_pcm_set_params");
    *reinterpret_cast<void**>(&write_) = dlsym(lib_, "snd_pcm_writei");
    *reinterpret_cast<void**>(&recover_) = dlsym(lib_, "snd_pcm_recover");
    *reinterpret_cast<void**>(&drop_) = dlsym(lib_, "snd_pcm_drop");
    *reinterpret_cast<void**>(&prepare_) = dlsym(lib_, "snd_pcm_prepare");
    *reinterpret_cast<void**>(&state_) = dlsym(lib_, "snd_pcm_state");
    *reinterpret_cast<void**>(&start_) = dlsym(lib_, "snd_pcm_start");
    *reinterpret_cast<void**>(&close_) = dlsym(lib_, "snd_pcm_close");
    *reinterpret_cast<void**>(&error_) = dlsym(lib_, "snd_strerror");
    if (!open_ || !params_ || !write_ || !recover_ || !drop_ || !prepare_ || !state_ || !start_ || !close_ || !error_) {
        ELOG("AudioOutput", "ALSA playback functions unavailable");
        close();
        return false;
    }
    const int err = open_(&pcm_, pcm, 0, 1);  // playback, nonblocking
    if (err < 0) {
        ELOG("AudioOutput", "cannot open %s: %s", pcm, error_(err));
        close();
        return false;
    }
    const int configured = params_(pcm_, 2, 3, channels, 48000, 1, 100000);  // S16LE, interleaved
    if (configured < 0) {
        ELOG("AudioOutput", "cannot configure %s: %s", pcm, error_(configured));
        close();
        return false;
    }
    return true;
}

long AudioOutput::write(const int16_t* samples, size_t frames) {
    if (!pcm_ || !samples || !frames) {
        ELOG("AudioOutput", "write without an open PCM or samples");
        return -EINVAL;
    }
    long result = write_(pcm_, samples, static_cast<unsigned long>(frames));
    if (result == -EPIPE) {
        const int recovered = recover_(pcm_, static_cast<int>(result), 1);
        result = recovered < 0 ? recovered : -EAGAIN;
    }
    if (result > 0 && state_(pcm_) == 2) {  // ALSA PREPARED: start short prompts without waiting for a full buffer.
        const int started = start_(pcm_);
        if (started < 0) {
            result = started;
        }
    }
    if (result < 0 && result != -EAGAIN) {
        ELOG("AudioOutput", "playback write failed: %s", error_(static_cast<int>(result)));
    }
    return result;
}

bool AudioOutput::flush() {
    if (!pcm_ || !drop_ || !prepare_) {
        ELOG("AudioOutput", "flush without an open PCM");
        return false;
    }
    const int dropped = drop_(pcm_);
    const int prepared = dropped < 0 ? dropped : prepare_(pcm_);
    if (prepared < 0) {
        ELOG("AudioOutput", "cannot flush PCM: %s", error_(prepared));
        return false;
    }
    return true;
}

void AudioOutput::close() {
    if (pcm_ && close_) {
        if (drop_) {
            const int stopped = drop_(pcm_);
            if (stopped < 0 && error_) {
                ELOG("AudioOutput", "stopping PCM failed: %s", error_(stopped));
            }
        }
        const int err = close_(pcm_);
        if (err < 0 && error_) {
            ELOG("AudioOutput", "closing PCM failed: %s", error_(err));
        }
    }
    pcm_ = nullptr;
    if (lib_) {
        dlclose(lib_);
        lib_ = nullptr;
    }
    open_ = nullptr;
    params_ = nullptr;
    write_ = nullptr;
    recover_ = nullptr;
    drop_ = nullptr;
    prepare_ = nullptr;
    state_ = nullptr;
    start_ = nullptr;
    close_ = nullptr;
    error_ = nullptr;
}

}  // namespace evo
