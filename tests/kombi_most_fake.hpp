// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// A fake vmostd connection and an F52's MOST cluster for the cluster guidance tests: what MostHud sends, and the
// answers.
#ifndef ENTRY_AAD_TESTS_KOMBI_MOST_FAKE_HPP
#define ENTRY_AAD_TESTS_KOMBI_MOST_FAKE_HPP

#include <stddef.h>
#include <stdint.h>

#include <utility>
#include <vector>

#include "kombi_most.hpp"
#include "kombi_test_util.hpp"
#include "vmost_client.hpp"

namespace kombitest {

struct Message {
    evo::VmostMsgOut out;
    Bytes data;
};

class FakePort : public evo::VmostPort {
public:
    FakePort() : refuseSends(false), now(0), ackTarget(nullptr), ackSent(0) {}
    int send(evo::VmostMsgOut& m) override {
        if (refuseSends) {
            return -1;
        }
        Message copy;
        copy.out = m;
        copy.data.assign(m.data, m.data + m.len);
        sent.push_back(copy);
        at.push_back(now);
        return 0;
    }
    int shadow(uint8_t fblock, uint8_t inst, bool on) override {
        shadows.push_back((on ? 0x10000 : 0) | (fblock << 8) | inst);
        return 0;
    }
    void pump(int) override {
        if (!ackTarget) {
            return;
        }
        while (ackSent < sent.size()) {
            const Message& m = sent[ackSent++];
            evo::VmostMsgView v;
            v.addr = m.out.addr;
            v.fblock = m.out.fblock;
            v.inst = m.out.inst;
            v.fkt = m.out.fkt;
            v.op = evo::kombi::kMostOpResultAck;
            v.len = 0;
            v.data = nullptr;
            ackTarget->onMessage(v);
        }
    }
    std::vector<Message> sent;
    std::vector<uint32_t> at;  // when each was sent
    std::vector<int> shadows;
    bool refuseSends;
    uint32_t now;
    evo::kombi::MostHud* ackTarget;
    size_t ackSent;
};

inline evo::VmostMsgView answer(uint8_t fblock, uint16_t fkt, uint32_t op, const Bytes& data, uint16_t addr = 0x0101) {
    evo::VmostMsgView v;
    v.addr = addr;
    v.fblock = fblock;
    v.inst = 0;
    v.fkt = fkt;
    v.op = op;
    v.len = static_cast<uint16_t>(data.size());
    v.data = data.empty() ? nullptr : data.data();
    return v;
}

// An answer the car sends later.
struct Later {
    uint32_t at;
    uint16_t fkt;
    Bytes data;
};

// The cluster of the F52: registry with 82/00 at 0x0101, the 6WA fields; every call acknowledged unless refuseFkt.
struct Car {
    bool hud;
    bool silent;     // the registry does not answer
    bool hudSilent;  // the cluster does not answer
    uint16_t refuseFkt;
    uint16_t pictureW, pictureH;
    uint32_t sizeDelayMs;    // a cold cluster answers the size reads this late
    uint32_t answerDelayMs;  // and the drawing calls this late, each answer with its call's parameters
    bool drawSilent;         // the drawing calls are never answered
    bool onlyListed;         // another cluster: only the fields in `listed` answer a size read
    bool refuseSizes;        // and the others refuse it
    std::vector<std::pair<uint16_t, evo::kombi::FieldSize>> listed;
    std::vector<Later> later;
    Car()
        : hud(true),
          silent(false),
          hudSilent(false),
          refuseFkt(0),
          pictureW(176),
          pictureH(126),
          sizeDelayMs(0),
          answerDelayMs(0),
          drawSilent(false),
          onlyListed(false),
          refuseSizes(false) {}
    // The F15's fields as another project saw them: the picture buffers answer no size read.
    static Car f15() {
        Car c;
        c.onlyListed = true;
        return c;
    }
};

inline Bytes sizeAnswerOf(uint16_t field, uint16_t width, uint16_t height) {
    return Bytes({static_cast<uint8_t>(field >> 8), static_cast<uint8_t>(field & 0xFF), 0x01,
                  static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width & 0xFF),
                  static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height & 0xFF)});
}

// Ticks the hud for `ms`, answering each new call as the car would.
inline void run(evo::kombi::MostHud& hud, FakePort& port, Car& car, uint32_t& now, uint32_t ms) {
    using namespace evo;
    using namespace evo::kombi;
    size_t answered = port.sent.size();
    for (uint32_t end = now + ms; now < end; now += 5) {
        port.now = now;
        for (size_t i = 0; i < car.later.size();) {
            if (now >= car.later[i].at) {
                const Later l = car.later[i];
                car.later.erase(car.later.begin() + static_cast<long>(i));
                hud.onMessage(answer(kHudFblock, l.fkt, kMostOpResultAck, l.data));
            } else {
                ++i;
            }
        }
        hud.tick(now, port);
        while (answered < port.sent.size()) {
            const Message& m = port.sent[answered++];
            Bytes keep;
            if (m.out.fblock == kRegistryFblock ? car.silent : car.hudSilent) {
                continue;
            }
            if (m.out.fblock == kRegistryFblock) {
                const uint8_t entries[] = {0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x82, 0x00};
                keep.assign(entries, entries + (car.hud ? 8 : 4));
                hud.onMessage(answer(kRegistryFblock, kRegistryFkt, kMostOpStatus, keep, 0x0100));
            } else if (m.out.fkt == car.refuseFkt) {
                keep = Bytes({0x41, 0x02});
                hud.onMessage(answer(kHudFblock, m.out.fkt, kMostOpErrorAck, keep));
            } else if (m.out.fkt == kHudSize && car.onlyListed) {
                const uint16_t field = static_cast<uint16_t>((m.data[0] << 8) | m.data[1]);
                bool found = false;
                for (size_t k = 0; k < car.listed.size(); ++k) {
                    if (car.listed[k].first == field) {
                        found = true;
                        keep = sizeAnswerOf(field, car.listed[k].second.w, car.listed[k].second.h);
                        hud.onMessage(answer(kHudFblock, kHudSize, kMostOpResultAck, keep));
                    }
                }
                if (!found && car.refuseSizes) {
                    keep = Bytes({0x02, 0x00, 0x01});
                    hud.onMessage(answer(kHudFblock, kHudSize, kMostOpErrorAck, keep));
                }
            } else if (m.out.fkt == kHudSize) {
                static const uint16_t w[] = {396, 272, 80, 44, 176, 176}, h[] = {68, 33, 28, 28, 126, 126};
                const unsigned field = m.data[1];
                const uint16_t width = field >= 5 ? car.pictureW : w[field - 1];
                const uint16_t height = field >= 5 ? car.pictureH : h[field - 1];
                keep = Bytes({0x00, static_cast<uint8_t>(field), 0x01, static_cast<uint8_t>(width >> 8),
                              static_cast<uint8_t>(width & 0xFF), static_cast<uint8_t>(height >> 8),
                              static_cast<uint8_t>(height & 0xFF)});
                if (car.sizeDelayMs) {
                    Later l = {now + car.sizeDelayMs, kHudSize, keep};
                    car.later.push_back(l);
                } else {
                    hud.onMessage(answer(kHudFblock, kHudSize, kMostOpResultAck, keep));
                }
            } else if (car.drawSilent) {
                continue;
            } else if (car.answerDelayMs) {
                Later l = {now + car.answerDelayMs, m.out.fkt, m.data};
                car.later.push_back(l);
            } else {
                hud.onMessage(answer(kHudFblock, m.out.fkt, kMostOpResultAck, Bytes()));
            }
        }
    }
}

}  // namespace kombitest

#endif
