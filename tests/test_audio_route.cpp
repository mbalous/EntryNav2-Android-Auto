// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <assert.h>
#include <stdio.h>

#include <vector>

#include "audio_route.hpp"

struct Sent {
    uint16_t function;
    uint32_t op;
    std::vector<uint8_t> data;
};

class Port : public evo::VmostPort {
public:
    std::vector<Sent> sent;
    unsigned registrations, removals;
    Port() : registrations(0), removals(0) {}
    int send(evo::VmostMsgOut& m) override {
        assert(m.addr == 0xFFFF && m.fblock == 3 && m.inst == 0);
        Sent s = {m.fkt, m.op, std::vector<uint8_t>(m.data, m.data + m.len)};
        sent.push_back(s);
        return 0;
    }
    int shadow(uint8_t block, uint8_t instance, bool on) override {
        assert(block == 3 && instance == 0);
        on ? ++registrations : ++removals;
        return 0;
    }
    void pump(int) override {}
};

static void table(evo::AudioRoute& route, std::initializer_list<uint16_t> active) {
    std::vector<uint8_t> bytes(2, 0);
    for (uint16_t id : active) {
        bytes.push_back(static_cast<uint8_t>(id >> 8));
        bytes.push_back(static_cast<uint8_t>(id));
        bytes.push_back(0);
    }
    evo::VmostMsgView msg = {0, 3, 0, 0xC03, 0xC, static_cast<uint16_t>(bytes.size()), &bytes[0]};
    route.onMessage(msg);
}

static void result(evo::AudioRoute& route, const Sent& sent, uint32_t op = 0xD) {
    assert(sent.data.size() == 4 && sent.op == 6);
    evo::VmostMsgView msg = {0, 3, 0, sent.function, op, 2, &sent.data[0]};
    route.onMessage(msg);
}

static void expect(const Port& port, uint16_t function, uint32_t op, uint16_t id = 0) {
    assert(!port.sent.empty());
    const Sent& s = port.sent.back();
    if (s.function != function || s.op != op) {
        fprintf(stderr, "expected %03x/%u, got %03x/%u\n", function, op, s.function, s.op);
    }
    assert(s.function == function && s.op == op);
    if (op == 1) {
        assert(s.data.size() == 2 && s.data[0] == 0 && s.data[1] == 0);
    } else {
        assert(s.data.size() == 4 && ((s.data[2] << 8) | s.data[3]) == id);
    }
}

static void preflight(evo::AudioRoute& route, Port& port) {
    route.enable(true);
    route.tick(1, port);
    expect(port, 0xC03, 1);
    table(route, {1});
    route.tick(2, port);
    assert(route.ready());
}

static void take(evo::AudioRoute& route, Port& port) {
    route.request(0x315);
    route.tick(3, port);
    expect(port, 0xC03, 1);
    table(route, {1});
    route.tick(4, port);
    expect(port, 0x53, 6, 0x315);
    result(route, port.sent.back());
    route.tick(5, port);
    expect(port, 0xC03, 1);
    table(route, {0x315});
    route.tick(6, port);
    assert(route.owns(0x315));
}

int main() {
    {
        evo::AudioRoute route;
        Port port;
        preflight(route, port);
        take(route, port);
        route.enable(false);
        assert(!route.quiescent());
        route.request(0);
        route.tick(1006, port);
        expect(port, 0xC03, 1);
        table(route, {0x315});
        route.tick(1007, port);
        expect(port, 0x54, 6, 0x315);
        result(route, port.sent.back());
        route.tick(1008, port);
        table(route, {});
        route.tick(1009, port);
        assert(!route.quiescent());
        expect(port, 0x53, 6, 1);
        result(route, port.sent.back());
        route.tick(1010, port);
        assert(!route.owns(0x315) && !route.failure() && route.quiescent());
        route.finish(port);
        assert(port.registrations == 1 && port.removals == 1);
    }
    {
        evo::AudioRoute route;
        Port port;
        preflight(route, port);
        take(route, port);
        route.tick(1006, port);
        table(route, {2});
        route.tick(1007, port);
        assert(route.failure() && !route.owns(0x315));
        const size_t count = port.sent.size();
        route.request(0);
        route.tick(2000, port);
        assert(port.sent.size() == count);  // the driver's new source is never removed or replaced
        route.finish(port);
    }
    {
        evo::AudioRoute route;
        Port port;
        preflight(route, port);
        route.request(0x315);
        route.tick(3, port);
        route.enable(false);
        route.request(0);
        table(route, {1});
        route.tick(4, port);
        assert(route.quiescent() && port.sent.back().function == 0xC03);  // no build after quiescing
        route.finish(port);
    }
    {
        evo::AudioRoute route;
        Port port;
        preflight(route, port);
        route.request(0x315);
        route.tick(3, port);
        table(route, {0x315});
        route.tick(4, port);
        assert(route.failure() && !route.owns(0x315));
        assert(port.sent.back().op == 1);  // never claim someone else's route
        route.finish(port);
    }
    {
        evo::AudioRoute route;
        Port port;
        preflight(route, port);
        route.request(0x315);
        route.tick(3, port);
        table(route, {1});
        route.tick(4, port);
        const Sent build = port.sent.back();
        route.tick(10004, port);
        assert(route.failure() && !route.owns(0x315));
        route.enable(false);
        route.tick(10005, port);
        assert(!route.quiescent());  // a timed-out build may still be granted
        result(route, build);        // a late grant still has to be relinquished
        route.tick(10005, port);
        table(route, {0x315});
        route.tick(10006, port);
        route.tick(11006, port);
        table(route, {0x315});
        route.tick(11007, port);
        expect(port, 0x54, 6, 0x315);
        result(route, port.sent.back());
        route.tick(11008, port);
        table(route, {});
        route.tick(11009, port);
        assert(route.quiescent());
        assert(port.sent.back().function == 0xC03);  // no source restoration after an uncertain result
        route.finish(port);
    }
    puts("PASS audio routes: preflight, ownership, restoration, driver selection and late grant");
    return 0;
}
