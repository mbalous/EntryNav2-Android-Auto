// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <errno.h>
#include <string.h>

static int fake_pcm, prompt_pcm, media_state, prompt_state, starts, next_error, closed, recovered, written;
static int prompt_written, dropped, stuck;
static int* state_for(void* pcm) {
    return pcm == &fake_pcm ? &media_state : &prompt_state;
}
int snd_pcm_open(void** pcm, const char* name, int stream, int flags) {
    if ((strcmp(name, "test") && strcmp(name, "prompt")) || stream != 0 || flags != 1) {
        return -ENOENT;
    }
    *pcm = strcmp(name, "prompt") == 0 ? &prompt_pcm : &fake_pcm;
    __atomic_store_n(state_for(*pcm), 2, __ATOMIC_SEQ_CST);
    return 0;
}
int snd_pcm_set_params(void* pcm, int format, int access, unsigned channels, unsigned rate, int resample,
                       unsigned latency) {
    return ((pcm == &fake_pcm && channels == 2) || (pcm == &prompt_pcm && channels == 1)) && format == 2 &&
                   access == 3 && rate == 48000 && resample == 1 && latency == 100000
               ? 0
               : -EINVAL;
}
long snd_pcm_writei(void* pcm, const void* data, unsigned long frames) {
    if ((pcm != &fake_pcm && pcm != &prompt_pcm) || !data || !frames) {
        return -EINVAL;
    }
    if (__atomic_load_n(&stuck, __ATOMIC_SEQ_CST)) {
        return -EAGAIN;
    }
    const int result = __atomic_exchange_n(&next_error, 0, __ATOMIC_SEQ_CST);
    if (result) {
        return result;
    }
    const long count = frames > 2 ? 2 : (long)frames;
    __atomic_add_fetch(pcm == &prompt_pcm ? &prompt_written : &written, (int)count, __ATOMIC_SEQ_CST);
    return count;
}
int snd_pcm_recover(void* pcm, int error, int silent) {
    if ((pcm != &fake_pcm && pcm != &prompt_pcm) || error != -EPIPE || silent != 1) {
        return -EINVAL;
    }
    __atomic_add_fetch(&recovered, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(state_for(pcm), 2, __ATOMIC_SEQ_CST);
    return 0;
}
int snd_pcm_close(void* pcm) {
    if (pcm != &fake_pcm && pcm != &prompt_pcm) {
        return -EINVAL;
    }
    __atomic_add_fetch(&closed, 1, __ATOMIC_SEQ_CST);
    return 0;
}
int snd_pcm_drop(void* pcm) {
    if (pcm != &fake_pcm && pcm != &prompt_pcm) {
        return -EINVAL;
    }
    __atomic_add_fetch(&dropped, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(state_for(pcm), 1, __ATOMIC_SEQ_CST);
    return 0;
}
int snd_pcm_prepare(void* pcm) {
    if (pcm != &fake_pcm && pcm != &prompt_pcm) {
        return -EINVAL;
    }
    __atomic_store_n(state_for(pcm), 2, __ATOMIC_SEQ_CST);
    return 0;
}
int snd_pcm_state(void* pcm) {
    return __atomic_load_n(state_for(pcm), __ATOMIC_SEQ_CST);
}
int snd_pcm_start(void* pcm) {
    __atomic_store_n(state_for(pcm), 3, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&starts, 1, __ATOMIC_SEQ_CST);
    return 0;
}
const char* snd_strerror(int error) {
    (void)error;
    return "fake error";
}
void fake_audio_error(int error) {
    __atomic_store_n(&next_error, error, __ATOMIC_SEQ_CST);
}
int fake_audio_closed(void) {
    return __atomic_load_n(&closed, __ATOMIC_SEQ_CST);
}
int fake_audio_recovered(void) {
    return __atomic_load_n(&recovered, __ATOMIC_SEQ_CST);
}
int fake_audio_written(void) {
    return __atomic_load_n(&written, __ATOMIC_SEQ_CST);
}
int fake_audio_prompt_written(void) {
    return __atomic_load_n(&prompt_written, __ATOMIC_SEQ_CST);
}
int fake_audio_dropped(void) {
    return __atomic_load_n(&dropped, __ATOMIC_SEQ_CST);
}
int fake_audio_starts(void) {
    return __atomic_load_n(&starts, __ATOMIC_SEQ_CST);
}
void fake_audio_stuck(int value) {
    __atomic_store_n(&stuck, value, __ATOMIC_SEQ_CST);
}
