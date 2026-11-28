// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Nonblocking playback through the head unit's ALSA library; owned by the playback worker.
#ifndef ENTRY_AAD_PLATFORM_AUDIO_OUTPUT_HPP
#define ENTRY_AAD_PLATFORM_AUDIO_OUTPUT_HPP

#include <stddef.h>
#include <stdint.h>

namespace evo {

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Load and open an existing PCM, 48 kHz S16LE. The library argument also permits a fake in host tests.
    bool open(const char* pcm, const char* library = "libasound.so.2", unsigned channels = 2);
    // Returns written frames, -EAGAIN when full, or a negative ALSA error. Handles a recoverable underrun once.
    long write(const int16_t* samples, size_t frames);
    bool flush();
    void close();

private:
    void* lib_;
    void* pcm_;
    int (*open_)(void**, const char*, int, int);
    int (*params_)(void*, int, int, unsigned, unsigned, int, unsigned);
    long (*write_)(void*, const void*, unsigned long);
    int (*recover_)(void*, int, int);
    int (*drop_)(void*);
    int (*prepare_)(void*);
    int (*state_)(void*);
    int (*start_)(void*);
    int (*close_)(void*);
    const char* (*error_)(int);
};

}  // namespace evo

#endif
