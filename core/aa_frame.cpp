// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto frame header and fragment reassembly (tests/test_aa_frame.cpp).
#include "aa_frame.hpp"

namespace aa {

FrameHeader decodeFrameHeader(const uint8_t* bytes) {
    FrameHeader header;
    header.channel = bytes[0];
    header.flags = bytes[1];
    header.length = static_cast<uint16_t>((bytes[2] << 8) | bytes[3]);
    header.totalSize = 0;
    return header;
}

uint32_t decodeTotalSize(const uint8_t* bytes) {
    uint32_t size = 0;
    for (int i = 0; i < 4; ++i) {
        size = (size << 8) | bytes[i];
    }
    return size;
}

bool encodeFrame(uint8_t channel, uint8_t flags, const uint8_t* payload, size_t length, std::vector<uint8_t>& out) {
    if (length > frame::kMaxPayload || (length > 0 && !payload)) {
        return false;
    }
    out.clear();
    out.reserve(frame::kHeaderSize + length);
    out.push_back(channel);
    out.push_back(flags);
    out.push_back(static_cast<uint8_t>(length >> 8));
    out.push_back(static_cast<uint8_t>(length & 0xFF));
    if (length > 0) {
        out.insert(out.end(), payload, payload + length);
    }
    return true;
}

MessageAssembler::Outcome MessageAssembler::add(const FrameHeader& header, const std::vector<uint8_t>& payload,
                                                std::vector<uint8_t>& message) {
    const uint8_t position = header.position();
    if (position == frame::kWhole) {
        // A message of its own, one frame long: what is being assembled or skipped on the channel stays as it is.
        if (payload.size() > frame::kMaxMessage) {
            return kOverflow;
        }
        message = payload;
        return kComplete;
    }
    if (position == frame::kFirst) {
        // Replaces an unfinished message, or ends the tail of a lost one whose LAST never came: freed before this one
        // is weighed. One announced larger than a message may be is dropped before any of it is kept.
        release(header.channel);
        if (header.totalSize > frame::kMaxMessage || header.totalSize > maxPending_ || !fits(payload.size(), 0)) {
            forget(header.channel, position);
            return kOverflow;
        }
        Pending& pending = pending_[header.channel];
        pending.data.assign(payload.begin(), payload.end());
        pending.lostTail = false;
        pending.encrypted = header.encrypted();
        pending.size = header.totalSize;
        pending.onWire = header.length;
        held_ += payload.size();
        return kPending;
    }
    const std::map<uint8_t, Pending>::iterator it = pending_.find(header.channel);
    if (it == pending_.end()) {
        forget(header.channel, position);
        return kNoFirst;
    }
    Pending& pending = it->second;
    if (pending.lostTail) {
        if (position == frame::kLast) {
            pending_.erase(it);
        }
        return kRestOfLost;
    }
    if (!fits(payload.size(), pending.data.size())) {
        forget(header.channel, position);
        return kOverflow;
    }
    // A phone encrypts all of a message or none of it: a fragment that differs from its FIRST is not part of it.
    if (header.encrypted() != pending.encrypted) {
        forget(header.channel, position);
        return kMixed;
    }
    if (position == frame::kMiddle) {
        pending.data.insert(pending.data.end(), payload.begin(), payload.end());
        pending.onWire += header.length;
        held_ += payload.size();
        return kPending;
    }
    // LAST closes the message started by FIRST, which then leaves the pending ones.
    const uint64_t onWire = pending.onWire + header.length;
    const uint32_t size = pending.size;
    held_ -= pending.data.size();
    pending.data.insert(pending.data.end(), payload.begin(), payload.end());
    message.swap(pending.data);
    pending_.erase(it);
    // The announced size counts plain bytes and the length fields count TLS records, which are larger: only a
    // shortfall tells that this LAST closed an incomplete message.
    if (size != 0 && onWire < size) {
        received_ = static_cast<uint32_t>(onWire);
        announced_ = size;
        return kShortOfSize;
    }
    return kComplete;
}

void MessageAssembler::lose(const FrameHeader& header) {
    if (header.position() != frame::kWhole) {
        forget(header.channel, header.position());
    }
}

// The bound of the message itself, and that of all channels together. Both hold the bytes kept so far (held_ may
// exceed maxPending_ only if tightened later), so the subtractions cannot wrap.
bool MessageAssembler::fits(size_t more, size_t holding) const {
    return holding <= frame::kMaxMessage && more <= frame::kMaxMessage - holding && held_ <= maxPending_ &&
           more <= maxPending_ - held_;
}

void MessageAssembler::release(uint8_t channel) {
    const std::map<uint8_t, Pending>::iterator it = pending_.find(channel);
    if (it != pending_.end()) {
        held_ -= it->second.data.size();
        pending_.erase(it);
    }
}

// Released first, so that what was gathered is freed; a LAST leaves nothing of its message to skip.
void MessageAssembler::forget(uint8_t channel, uint8_t position) {
    release(channel);
    if (position != frame::kLast) {
        pending_[channel].lostTail = true;
    }
}

}  // namespace aa
