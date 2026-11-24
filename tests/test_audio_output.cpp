// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>

#include "audio_output.hpp"

int main(int argc, char** argv) {
    assert(argc == 2);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(lib);
    typedef void (*SetError)(int);
    typedef int (*GetValue)();
    SetError setError = nullptr;
    GetValue closed = nullptr, recovered = nullptr, dropped = nullptr, starts = nullptr;
    *reinterpret_cast<void**>(&setError) = dlsym(lib, "fake_audio_error");
    *reinterpret_cast<void**>(&closed) = dlsym(lib, "fake_audio_closed");
    *reinterpret_cast<void**>(&recovered) = dlsym(lib, "fake_audio_recovered");
    *reinterpret_cast<void**>(&dropped) = dlsym(lib, "fake_audio_dropped");
    *reinterpret_cast<void**>(&starts) = dlsym(lib, "fake_audio_starts");
    assert(setError && closed && recovered && dropped && starts);
    evo::AudioOutput output;
    assert(!output.open("missing", argv[1]));
    assert(output.open("test", argv[1]));
    int16_t samples[8] = {};
    assert(output.write(samples, 4) == 2 && starts() == 1);
    setError(-EAGAIN);
    assert(output.write(samples, 4) == -EAGAIN);
    setError(-EPIPE);
    assert(output.write(samples, 4) == -EAGAIN && recovered() == 1);
    setError(-EIO);
    assert(output.write(samples, 4) == -EIO);
    output.close();
    assert(closed() == 1 && dropped() == 1);
    assert(output.write(samples, 4) == -EINVAL);
    dlclose(lib);
    puts("PASS audio output: nonblocking open, short start, partial writes, underrun and failure");
    return 0;
}
