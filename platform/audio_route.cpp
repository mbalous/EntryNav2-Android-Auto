// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "audio_route.hpp"

#include <algorithm>
#include <cstring>

#include "evo_log.hpp"

namespace evo {
namespace {
const uint8_t kBlock = 0x03;
const uint16_t kTable = 0xC03, kBuildFunction = 0x053, kRemoveFunction = 0x054;
const uint32_t kTimeoutMs = 10000, kPollMs = 1000;

bool has(const std::vector<uint16_t>& entries, uint16_t value) {
    return std::find(entries.begin(), entries.end(), value) != entries.end();
}
}  // namespace

AudioRoute::AudioRoute()
    : enabled_(false),
      ready_(false),
      quiescent_(false),
      desired_(0),
      owned_(0),
      usable_(0),
      restoreEpoch_(0),
      failure_(0),
      phase_(kIdle),
      shadowed_(false),
      received_(false),
      tableOk_(false),
      responseOk_(false),
      timedOut_(false),
      foreign_(false),
      route_(0),
      handle_(0),
      nextHandle_(0xA600),
      sentAt_(0),
      nextPoll_(0),
      claimEpoch_(0) {}

bool AudioRoute::wants(const VmostMsgView& m) const {
    return m.fblock == kBlock && m.inst == 0 &&
           (m.fkt == kTable || m.fkt == kBuildFunction || m.fkt == kRemoveFunction);
}

bool AudioRoute::parseTable(const VmostMsgView& m, std::vector<uint16_t>& active) const {
    active.clear();
    if (m.len < 2 || (m.len - 2) % 3) {
        return false;
    }
    for (size_t i = 2; i < m.len; i += 3) {
        if (m.data[i + 2] == 0) {
            active.push_back(static_cast<uint16_t>((m.data[i] << 8) | m.data[i + 1]));
        }
    }
    std::sort(active.begin(), active.end());
    active.erase(std::unique(active.begin(), active.end()), active.end());
    return true;
}

void AudioRoute::onMessage(const VmostMsgView& m) {
    if (!wants(m)) {
        return;
    }
    if (m.fkt == kTable && m.op == kMostOpStatus &&
        (phase_ == kPreflight || phase_ == kBefore || phase_ == kAfter || phase_ == kReleaseTable ||
         phase_ == kRestoreTable || phase_ == kMonitor)) {
        tableOk_ = parseTable(m, observed_);
        received_ = true;
    } else if ((phase_ == kBuild || phase_ == kRemove || phase_ == kRestore) &&
               m.fkt == (phase_ == kRemove ? kRemoveFunction : kBuildFunction) && m.len >= 2 &&
               static_cast<uint16_t>((m.data[0] << 8) | m.data[1]) == handle_) {
        if (m.op == 0xD || m.op == 0x9 || m.op == 0xF) {
            responseOk_ = m.op == 0xD;
            received_ = true;
        }
    }
}

void AudioRoute::fail(const char* reason) {
    ELOG("AudioRoute", "%s", reason);
    failure_.store(1);
    ready_.store(false);
    usable_.store(0);
    desired_.store(0);
    ++restoreEpoch_;
}

void AudioRoute::query(Phase phase, uint32_t now, VmostPort& port) {
    static const uint8_t position[] = {0, 0};
    VmostMsgOut m;
    buildVmostGet(m, kBlock, 0, kTable, position, sizeof(position));
    phase_ = phase;
    received_ = false;
    sentAt_ = now;
    timedOut_ = false;
    if (port.send(m) != 0) {
        fail("connection table Get refused");
        phase_ = owned_.load() ? kPlaying : kIdle;
        nextPoll_ = now + kPollMs;
    }
}

void AudioRoute::call(Phase phase, uint16_t route, uint32_t now, VmostPort& port) {
    handle_ = ++nextHandle_;
    const uint8_t data[] = {static_cast<uint8_t>(handle_ >> 8), static_cast<uint8_t>(handle_),
                            static_cast<uint8_t>(route >> 8), static_cast<uint8_t>(route)};
    VmostMsgOut m;
    memset(&m, 0, sizeof(m));
    m.addr = 0xFFFF;
    m.fblock = kBlock;
    m.fkt = phase == kRemove ? kRemoveFunction : kBuildFunction;
    m.op = kMostOpStartResultAck;
    m.len = sizeof(data);
    m.data = data;
    phase_ = phase;
    received_ = false;
    timedOut_ = false;
    sentAt_ = now;
    if (port.send(m) != 0) {
        fail("route request refused by vMOST");
        phase_ = owned_.load() ? kPlaying : kIdle;
        nextPoll_ = now + kPollMs;
    }
}

int AudioRoute::dueInMs(uint32_t) const {
    return 50;
}

void AudioRoute::tick(uint32_t now, VmostPort& port) {
    if (!shadowed_) {
        if (!enabled_.load() || failure_.load()) {
            return;
        }
        if (port.shadow(kBlock, 0, true) != 0) {
            fail("ConnectionMaster shadow registration refused");
            return;
        }
        shadowed_ = true;
        query(kPreflight, now, port);
        return;
    }
    if (phase_ != kIdle && phase_ != kPlaying && !received_ && now - sentAt_ >= kTimeoutMs && !timedOut_) {
        timedOut_ = true;
        fail("connection table or route request timed out; ownership is uncertain");
        if (phase_ != kBuild && phase_ != kRemove && phase_ != kRestore) {
            phase_ = owned_.load() ? kPlaying : kIdle;
            nextPoll_ = now + kPollMs;
        }
    }
    if (received_) {
        received_ = false;
        if (phase_ == kBuild || phase_ == kRemove || phase_ == kRestore) {
            if (!responseOk_) {
                fail("ConnectionMaster rejected a route request");
                phase_ = owned_.load() ? kPlaying : kIdle;
                nextPoll_ = now + kPollMs;
            } else if (phase_ == kBuild) {
                owned_.store(route_);
                query(kAfter, now, port);
            } else if (phase_ == kRemove) {
                owned_.store(0);
                usable_.store(0);
                query(kRestoreTable, now, port);
            } else {
                phase_ = kIdle;
            }
        } else if (!tableOk_) {
            fail("malformed connection table");
            phase_ = owned_.load() ? kPlaying : kIdle;
            nextPoll_ = now + kPollMs;
        } else if (phase_ == kPreflight) {
            ready_.store(true);
            phase_ = kIdle;
        } else if (phase_ == kBefore) {
            if (!enabled_.load() || desired_.load() != route_ || failure_.load()) {
                phase_ = kIdle;
            } else if (has(observed_, route_)) {
                fail("candidate route is already in use");
                phase_ = kIdle;
            } else {
                before_ = observed_;
                call(kBuild, route_, now, port);
            }
        } else if (phase_ == kAfter) {
            displaced_.clear();
            for (size_t i = 0; i < before_.size(); ++i) {
                if (!has(observed_, before_[i])) {
                    displaced_.push_back(before_[i]);
                }
            }
            playing_ = observed_;
            foreign_ = !has(observed_, route_);
            if (foreign_) {
                fail("built route is not active in the table");
            } else if (!failure_.load() && enabled_.load() && desired_.load() == route_) {
                usable_.store(route_);
            }
            nextPoll_ = now + kPollMs;
            phase_ = kPlaying;
        } else if (phase_ == kMonitor) {
            if (observed_ != playing_) {
                foreign_ = true;
            }
            if (!has(observed_, route_)) {
                owned_.store(0);
                usable_.store(0);
                fail("another source replaced the audio route");
                phase_ = kIdle;
            } else {
                phase_ = kPlaying;
            }
            nextPoll_ = now + kPollMs;
        } else if (phase_ == kReleaseTable) {
            if (observed_ != playing_) {
                foreign_ = true;
            }
            if (has(observed_, route_)) {
                call(kRemove, route_, now, port);
            } else {
                owned_.store(0);
                phase_ = kIdle;
            }
        } else if (phase_ == kRestoreTable) {
            if (!foreign_ && restoreEpoch_.load() == claimEpoch_ && displaced_.size() == 1 && observed_.empty()) {
                call(kRestore, displaced_[0], now, port);
            } else {
                phase_ = kIdle;
            }
        }
    }
    const uint16_t desired = desired_.load();
    if (phase_ == kIdle && enabled_.load() && ready_.load() && desired && !failure_.load()) {
        route_ = desired;
        claimEpoch_ = restoreEpoch_.load();
        foreign_ = false;
        query(kBefore, now, port);
    } else if (phase_ == kPlaying) {
        if ((!enabled_.load() || desired_.load() != route_ || failure_.load()) &&
            static_cast<int32_t>(now - nextPoll_) >= 0) {
            usable_.store(0);
            query(kReleaseTable, now, port);
        } else if (static_cast<int32_t>(now - nextPoll_) >= 0) {
            query(kMonitor, now, port);
            nextPoll_ = now + kPollMs;
        }
    }
    if (!enabled_.load() && phase_ == kIdle && !owned_.load()) {
        quiescent_.store(true);
    }
}

void AudioRoute::finish(VmostPort& port) {
    enabled_.store(false);
    desired_.store(0);
    const uint32_t start = monoMs();
    while ((owned_.load() || phase_ == kBuild || phase_ == kRemove || phase_ == kRestore || phase_ == kReleaseTable ||
            phase_ == kRestoreTable || phase_ == kAfter) &&
           monoMs() - start < 12000) {
        tick(monoMs(), port);
        port.pump(50);
    }
    if (owned_.load() || phase_ == kBuild || phase_ == kRemove) {
        ELOG("AudioRoute", "shutdown with unresolved route ownership");
    }
    if (shadowed_ && port.shadow(kBlock, 0, false) != 0) {
        ELOG("AudioRoute", "shadow deregistration failed");
    }
    shadowed_ = false;
    ready_.store(false);
}

void AudioRoute::connectionLost() {
    if (enabled_.load() || owned_.load()) {
        fail("vMOST connection lost; route cleanup cannot be confirmed");
    }
}

}  // namespace evo
