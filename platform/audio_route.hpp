// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// ConnectionMaster route ownership on the controller's existing vMOST connection.
#ifndef ENTRY_AAD_PLATFORM_AUDIO_ROUTE_HPP
#define ENTRY_AAD_PLATFORM_AUDIO_ROUTE_HPP

#include <atomic>
#include <vector>

#include "vmost_client.hpp"

namespace evo {

class AudioRoute : public VmostPeer {
public:
    AudioRoute();
    // Called from other threads. A zero route releases the current one. Repeated calls are harmless.
    void enable(bool on) {
        if (!on) {
            quiescent_.store(false);
        }
        enabled_.store(on);
    }
    void request(uint16_t route) { desired_.store(route); }
    // A native call or other externally selected source must never be displaced by restoration.
    void prohibitRestore() { ++restoreEpoch_; }
    bool ready() const { return ready_.load(); }
    bool quiescent() const { return quiescent_.load(); }
    bool owns(uint16_t route) const { return usable_.load() == route && route != 0; }
    int failure() const { return failure_.load(); }

    bool wants(const VmostMsgView& message) const override;
    void onMessage(const VmostMsgView& message) override;
    int dueInMs(uint32_t nowMs) const override;
    void tick(uint32_t nowMs, VmostPort& port) override;
    void finish(VmostPort& port) override;
    void connectionLost() override;

private:
    enum Phase {
        kIdle,
        kPreflight,
        kBefore,
        kBuild,
        kAfter,
        kPlaying,
        kMonitor,
        kReleaseTable,
        kRemove,
        kRestoreTable,
        kRestore
    };
    void query(Phase phase, uint32_t now, VmostPort& port);
    void call(Phase phase, uint16_t route, uint32_t now, VmostPort& port);
    void fail(const char* reason);
    bool parseTable(const VmostMsgView& message, std::vector<uint16_t>& active) const;

    std::atomic<bool> enabled_, ready_, quiescent_;
    std::atomic<uint16_t> desired_, owned_, usable_;
    std::atomic<uint32_t> restoreEpoch_;
    std::atomic<int> failure_;
    Phase phase_;
    bool shadowed_, received_, tableOk_, responseOk_, timedOut_, foreign_;
    uint16_t route_, handle_, nextHandle_;
    uint32_t sentAt_, nextPoll_, claimEpoch_;
    std::vector<uint16_t> before_, playing_, displaced_, observed_;
};

}  // namespace evo

#endif
