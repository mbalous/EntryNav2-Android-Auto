// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <assert.h>
#include <stdio.h>

#include <vector>

#include "audio_mixer.hpp"

int main() {
    evo::AudioMixer mixer;
    int16_t out[16] = {};
    const uint8_t music[] = {0xe8, 0x03, 0x18, 0xfc, 0xe8, 0x03, 0x18, 0xfc,
                             0xe8, 0x03, 0x18, 0xfc, 0xe8, 0x03, 0x18, 0xfc};
    const uint8_t speech[] = {0xe8, 0x03};
    assert(mixer.pushMedia(music, sizeof(music)));
    assert(mixer.pushSpeech(speech, sizeof(speech), 0));
    assert(mixer.read(out, 8) == 4);
    for (size_t i = 0; i < 3; ++i) {
        assert(out[2 * i] == 1300 && out[2 * i + 1] == 700);
    }
    assert(out[6] == 1000 && out[7] == -1000);
    assert(mixer.read(out, 8) == 0);
    assert(!mixer.pushMedia(music, 3) && !mixer.pushSpeech(speech, 1, 0) && !mixer.pushSpeech(speech, 2, 2));
    const uint8_t loud[] = {0xff, 0x7f, 0x00, 0x80};
    const uint8_t loudVoice[] = {0xff, 0x7f};
    assert(mixer.pushMedia(loud, sizeof(loud)) && mixer.pushSpeech(loudVoice, sizeof(loudVoice), 1));
    assert(mixer.read(out, 1) == 1 && out[0] == 32767 && out[1] == 22937);
    assert(mixer.pushMedia(music, sizeof(music)));
    mixer.clearMedia();
    assert(mixer.read(out, 1) == 0);
    std::vector<uint8_t> overflow((evo::AudioMixer::kCapacityFrames + 1) * 4, 0);
    overflow[0] = 1;
    overflow[overflow.size() - 4] = 2;
    assert(mixer.pushMedia(overflow.data(), overflow.size()));
    assert(mixer.read(out, 1) == 1 && out[0] == 0);
    std::vector<int16_t> rest(evo::AudioMixer::kCapacityFrames * 2);
    assert(mixer.read(rest.data(), evo::AudioMixer::kCapacityFrames) == evo::AudioMixer::kCapacityFrames - 1);
    assert(rest[(evo::AudioMixer::kCapacityFrames - 2) * 2] == 2);
    puts("PASS audio mixer: bounds, formats, voice ducking, clipping and stream clear");
    return 0;
}
