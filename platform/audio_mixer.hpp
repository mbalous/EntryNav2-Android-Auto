// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Bounded 48 kHz stereo output from the Android Auto media and mono speech streams.
#ifndef ENTRY_AAD_PLATFORM_AUDIO_MIXER_HPP
#define ENTRY_AAD_PLATFORM_AUDIO_MIXER_HPP

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace evo {

// The caller serializes access. No pointers to input data are retained.
class AudioMixer {
public:
    static const size_t kCapacityFrames = 24000;  // 500 ms at 48 kHz

    AudioMixer() : media_(kCapacityFrames, 2), speech_(kCapacityFrames, 1) {}

    void clearMedia() { media_.clear(); }
    void clearSpeech() { speech_.clear(); }
    size_t mediaFrames() const { return media_.size(); }
    size_t speechFrames() const { return speech_.size(); }

    // Returns false for malformed PCM or an unsupported speech configuration. A full buffer discards oldest frames.
    bool pushMedia(const uint8_t* pcm, size_t bytes) {
        if (!pcm || !bytes || bytes % 4) {
            return false;
        }
        if (bytes / 4 > kCapacityFrames) {
            pcm += bytes - kCapacityFrames * 4;
            bytes = kCapacityFrames * 4;
        }
        for (size_t i = 0; i < bytes; i += 4) {
            media_.push(sample(pcm + i), sample(pcm + i + 2));
        }
        return true;
    }

    bool pushSpeech(const uint8_t* pcm, size_t bytes, unsigned config) {
        if (!pcm || !bytes || bytes % 2 || config > 1) {
            return false;
        }
        const unsigned copies = config == 0 ? 3 : 1;
        const size_t maxInputFrames = kCapacityFrames / copies;
        if (bytes / 2 > maxInputFrames) {
            pcm += bytes - maxInputFrames * 2;
            bytes = maxInputFrames * 2;
        }
        for (size_t i = 0; i < bytes; i += 2) {
            const int16_t value = sample(pcm + i);
            for (unsigned n = 0; n < copies; ++n) {
                speech_.push(value, 0);
            }
        }
        return true;
    }

    // Stereo S16 output, at most maxFrames. Speech ducks media to 30%, with clipping after mixing.
    size_t read(int16_t* out, size_t maxFrames, bool media = true, bool speech = true) {
        if (!out) {
            return 0;
        }
        size_t written = 0;
        while (written < maxFrames && ((media && media_.size()) || (speech && speech_.size()))) {
            int16_t left = 0, right = 0, voice = 0, unused = 0;
            if (media) {
                media_.pop(left, right);
            }
            const bool speaking = speech && speech_.pop(voice, unused);
            const int gain = speaking ? 30 : 100;
            out[written * 2] = clamp((int(left) * gain) / 100 + voice);
            out[written * 2 + 1] = clamp((int(right) * gain) / 100 + voice);
            ++written;
        }
        return written;
    }

private:
    class Ring {
    public:
        Ring(size_t frames, size_t channels)
            : data_(frames * channels), channels_(channels), capacity_(frames), head_(0), size_(0) {}
        size_t size() const { return size_; }
        void clear() { head_ = size_ = 0; }
        void push(int16_t left, int16_t right) {
            if (size_ == capacity_) {
                head_ = (head_ + 1) % capacity_;
                --size_;
            }
            const size_t index = ((head_ + size_) % capacity_) * channels_;
            data_[index] = left;
            if (channels_ == 2) {
                data_[index + 1] = right;
            }
            ++size_;
        }
        bool pop(int16_t& left, int16_t& right) {
            if (!size_) {
                return false;
            }
            const size_t index = head_ * channels_;
            left = data_[index];
            right = channels_ == 2 ? data_[index + 1] : 0;
            head_ = (head_ + 1) % capacity_;
            --size_;
            return true;
        }

    private:
        std::vector<int16_t> data_;
        size_t channels_, capacity_, head_, size_;
    };

    static int16_t sample(const uint8_t* pcm) {
        const uint16_t u = static_cast<uint16_t>(pcm[0]) | (static_cast<uint16_t>(pcm[1]) << 8);
        const int32_t signedValue = u < 0x8000 ? u : int32_t(u) - 0x10000;
        return static_cast<int16_t>(signedValue);
    }
    static int16_t clamp(int value) {
        return static_cast<int16_t>(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
    }

    Ring media_, speech_;
};

}  // namespace evo

#endif
