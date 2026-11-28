// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include <vector>

#include "audio_player.hpp"
#include "audio_route.hpp"
#include "evo_log.hpp"

struct Message {
    uint16_t function;
    uint32_t op;
    std::vector<uint8_t> data;
};

class Port : public evo::VmostPort {
public:
    std::vector<Message> messages;
    int send(evo::VmostMsgOut& m) override {
        Message message = {m.fkt, m.op, std::vector<uint8_t>(m.data, m.data + m.len)};
        messages.push_back(message);
        return 0;
    }
    int shadow(uint8_t, uint8_t, bool) override { return 0; }
    void pump(int) override {}
};

static void table(evo::AudioRoute& route, std::initializer_list<uint16_t> active) {
    std::vector<uint8_t> bytes(2, 0);
    for (uint16_t id : active) {
        bytes.push_back(static_cast<uint8_t>(id >> 8));
        bytes.push_back(static_cast<uint8_t>(id));
        bytes.push_back(0);
    }
    evo::VmostMsgView message = {0, 3, 0, 0xC03, 0xC, static_cast<uint16_t>(bytes.size()), &bytes[0]};
    route.onMessage(message);
}

static void answer(evo::AudioRoute& route, const Message& sent) {
    assert(sent.op == 6 && sent.data.size() == 4);
    evo::VmostMsgView message = {0, 3, 0, sent.function, 0xD, 2, &sent.data[0]};
    route.onMessage(message);
}

static void waitRequest(evo::AudioRoute& route, Port& port, size_t count) {
    for (unsigned i = 0; i < 200 && port.messages.size() < count; ++i) {
        route.tick(evo::monoMs(), port);
        usleep(10000);
    }
    assert(port.messages.size() == count);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(lib);
    typedef int (*Get)();
    typedef void (*Set)(int);
    Get written = nullptr, promptWritten = nullptr;
    Set error = nullptr;
    *reinterpret_cast<void**>(&written) = dlsym(lib, "fake_audio_written");
    *reinterpret_cast<void**>(&promptWritten) = dlsym(lib, "fake_audio_prompt_written");
    *reinterpret_cast<void**>(&error) = dlsym(lib, "fake_audio_error");
    assert(written && promptWritten && error);

    evo::AudioRoute route;
    Port port;
    route.enable(true);
    route.tick(evo::monoMs(), port);
    table(route, {1});
    route.tick(evo::monoMs(), port);
    assert(route.ready());
    evo::AudioPlayer player;
    assert(player.startRouted(route, "test", "prompt", argv[1]));
    const uint8_t voice[] = {1, 0, 2, 0};
    evo::AudioPlayer::stream(8, 1, 0, true, &player);
    evo::AudioPlayer::data(8, 1, voice, sizeof(voice), 0, &player);
    waitRequest(route, port, 2);
    assert(port.messages.back().function == 0xC03);
    table(route, {1});
    route.tick(evo::monoMs(), port);
    assert(port.messages.back().function == 0x53 && port.messages.back().data[3] == 0x1A);
    answer(route, port.messages.back());
    route.tick(evo::monoMs(), port);
    table(route, {1, 0x31A});
    route.tick(evo::monoMs(), port);
    for (unsigned i = 0; i < 200 && promptWritten() < 6; ++i) {
        usleep(10000);
    }
    assert(promptWritten() == 6 && written() == 0);

    player.setCall(true);
    evo::AudioPlayer::data(8, 1, voice, sizeof(voice), 0, &player);
    usleep(50000);
    assert(promptWritten() == 6);
    route.tick(evo::monoMs() + 1100, port);
    assert(port.messages.back().function == 0xC03);
    table(route, {1, 0x31A});
    route.tick(evo::monoMs() + 1101, port);
    assert(port.messages.back().function == 0x54);
    answer(route, port.messages.back());
    route.tick(evo::monoMs() + 1102, port);
    table(route, {1});
    route.tick(evo::monoMs() + 1103, port);

    player.setCall(false);
    const uint8_t music[] = {1, 0, 2, 0, 3, 0, 4, 0};
    evo::AudioPlayer::stream(8, 1, 0, false, &player);
    evo::AudioPlayer::stream(9, 2, 0, true, &player);
    evo::AudioPlayer::data(9, 2, music, sizeof(music), 0, &player);
    const size_t next = port.messages.size() + 1;
    waitRequest(route, port, next);
    table(route, {1});
    route.tick(evo::monoMs(), port);
    assert(port.messages.back().function == 0x53 && port.messages.back().data[3] == 0x15);
    answer(route, port.messages.back());
    route.tick(evo::monoMs(), port);
    table(route, {0x315});
    route.tick(evo::monoMs(), port);
    for (unsigned i = 0; i < 200 && written() < 2; ++i) {
        usleep(10000);
    }
    assert(written() == 2 && player.failure() == 0);
    error(-EIO);
    evo::AudioPlayer::data(9, 2, music, sizeof(music), 0, &player);
    for (unsigned i = 0; i < 200 && !player.failure(); ++i) {
        usleep(10000);
    }
    assert(player.failure() == -EIO);
    player.stop();

    route.tick(evo::monoMs() + 1200, port);
    table(route, {0x315});
    route.tick(evo::monoMs() + 1201, port);
    assert(port.messages.back().function == 0x54);
    answer(route, port.messages.back());
    route.tick(evo::monoMs() + 1202, port);
    table(route, {});
    route.tick(evo::monoMs() + 1203, port);
    assert(port.messages.back().function == 0x53);  // the FM source was displaced by music
    answer(route, port.messages.back());
    route.tick(evo::monoMs() + 1204, port);
    route.finish(port);
    dlclose(lib);
    puts("PASS routed audio: FM prompt, call interruption, media and playback failure");
    return 0;
}
