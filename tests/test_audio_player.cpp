// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "audio_player.hpp"

int main(int argc, char** argv) {
    assert(argc == 2);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(lib);
    typedef void (*SetError)(int);
    typedef int (*GetValue)();
    SetError setError = nullptr, setStuck = nullptr;
    GetValue written = nullptr, closed = nullptr;
    *reinterpret_cast<void**>(&setError) = dlsym(lib, "fake_audio_error");
    *reinterpret_cast<void**>(&written) = dlsym(lib, "fake_audio_written");
    *reinterpret_cast<void**>(&closed) = dlsym(lib, "fake_audio_closed");
    *reinterpret_cast<void**>(&setStuck) = dlsym(lib, "fake_audio_stuck");
    assert(setError && setStuck && written && closed);
    evo::AudioPlayer player;
    assert(!player.start("missing", argv[1]));
    assert(player.start("test", argv[1]));
    const uint8_t pcm[] = {1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0};
    evo::AudioPlayer::stream(9, 7, 0, true, &player);
    evo::AudioPlayer::data(9, 7, pcm, sizeof(pcm), 0, &player);
    for (unsigned i = 0; i < 200 && written() < 3; ++i) {
        usleep(10000);
    }
    assert(written() == 3 && player.failure() == 0);
    evo::AudioPlayer::stream(9, 7, 0, false, &player);
    setError(-EIO);
    evo::AudioPlayer::stream(9, 8, 0, true, &player);
    evo::AudioPlayer::data(9, 8, pcm, sizeof(pcm), 0, &player);
    for (unsigned i = 0; i < 200 && !player.failure(); ++i) {
        usleep(10000);
    }
    assert(player.failure() == -EIO);
    player.stop();
    assert(closed() == 1);
    assert(player.start("test", argv[1]));
    setStuck(1);
    evo::AudioPlayer::stream(9, 9, 0, true, &player);
    evo::AudioPlayer::data(9, 9, pcm, sizeof(pcm), 0, &player);
    for (unsigned i = 0; i < 400 && !player.failure(); ++i) {
        usleep(10000);
    }
    assert(player.failure() == -ETIMEDOUT);
    setStuck(0);
    player.stop();
    assert(closed() == 2);
    dlclose(lib);
    puts("PASS audio player: bounded worker, partial writes, stream lifecycle and error reporting");
    return 0;
}
