// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// core/wireless_setup: a Bluetooth link that comes back while Android Auto runs is only listened to.

// The link is a SOCK_SEQPACKET socketpair: like the car's RFCOMM device, it keeps each message whole.
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

#include "wireless_setup.hpp"

namespace {

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

long long monoMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<long long>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

struct TrickleArgs {
    int fd;
    std::atomic<bool> stop;
};

void* trickleSender(void* arg) {
    TrickleArgs* a = static_cast<TrickleArgs*>(arg);
    const uint8_t junk = 0xAA;
    while (!a->stop.load()) {
        if (send(a->fd, &junk, 1, MSG_NOSIGNAL) <= 0) {
            break;
        }
        usleep(40000);
    }
    return nullptr;
}

// What the head unit wrote to the phone, or -1 when it wrote nothing.
ssize_t written(int phone, uint8_t* to, size_t cap) {
    return recv(phone, to, cap, MSG_DONTWAIT);
}

}  // namespace

int main() {
    int link[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, link) != 0) {
        puts("FAIL socketpair");
        return 1;
    }
    const int car = link[0], phone = link[1];
    aa::ApInfo ap;
    ap.ssid = "car";
    ap.key = "secret";
    ap.bssid = "00:11:22:33:44:55";
    ap.ip = "172.16.222.49";
    ap.security = 8;
    aa::WirelessSetup setup(car, ap, 50000, 300);
    uint8_t got[64];

    // Nothing from the phone: nothing is sent first, the link stays.
    CHECK(setup.listen(50) && setup.heard() == 0);
    CHECK(written(phone, got, sizeof(got)) < 0);

    // A ping: echoed back as a pong (message 9), body kept.
    const uint8_t ping[] = {0x00, 0x02, 0x00, 0x08, 0x08, 0x2a};
    CHECK(send(phone, ping, sizeof(ping), 0) == static_cast<ssize_t>(sizeof(ping)));
    CHECK(setup.listen(200) && setup.heard() == 1);
    const uint8_t pong[] = {0x00, 0x02, 0x00, 0x09, 0x08, 0x2a};
    CHECK(written(phone, got, sizeof(got)) == static_cast<ssize_t>(sizeof(pong)) && !memcmp(got, pong, sizeof(pong)));

    // The phone's own start request, an info request: counted, written to the log, not answered. An answer would
    // send the phone to a new TCP connection while its session runs.
    const uint8_t start[] = {0x00, 0x00, 0x00, 0x01};
    const uint8_t info[] = {0x00, 0x00, 0x00, 0x02};
    CHECK(send(phone, start, sizeof(start), 0) == static_cast<ssize_t>(sizeof(start)));
    CHECK(setup.listen(200) && setup.heard() == 2);
    CHECK(send(phone, info, sizeof(info), 0) == static_cast<ssize_t>(sizeof(info)));
    CHECK(setup.listen(200) && setup.heard() == 3);
    CHECK(written(phone, got, sizeof(got)) < 0);

    // The phone closes the link.
    close(phone);
    CHECK(!setup.listen(200));
    close(car);

    // A trickling peer (sending 1 byte every 40 ms) must not bypass handshake()'s timeout.
    {
        int tlink[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, tlink) == 0);
        TrickleArgs args;
        args.fd = tlink[1];
        args.stop = false;
        pthread_t th;
        CHECK(pthread_create(&th, nullptr, trickleSender, &args) == 0);
        aa::WirelessSetup trickled(tlink[0], ap, 50000, 0);
        const long long since = monoMs();
        CHECK(!trickled.handshake(250));
        const long long elapsed = monoMs() - since;
        args.stop = true;
        shutdown(tlink[1], SHUT_RDWR);
        pthread_join(th, nullptr);
        close(tlink[0]);
        close(tlink[1]);
        CHECK(elapsed >= 200 && elapsed < 2000);
    }

    // A full non-blocking socket must time out write() instead of spinning forever on EAGAIN.
    {
        int blink[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, blink) == 0);
        const int smallBuf = 4096;
        setsockopt(blink[0], SOL_SOCKET, SO_SNDBUF, &smallBuf, sizeof(smallBuf));
        setsockopt(blink[1], SOL_SOCKET, SO_RCVBUF, &smallBuf, sizeof(smallBuf));
        fcntl(blink[0], F_SETFL, fcntl(blink[0], F_GETFL, 0) | O_NONBLOCK);
        char fill[1024];
        memset(fill, 'x', sizeof(fill));
        while (send(blink[0], fill, sizeof(fill), MSG_DONTWAIT) > 0) {
        }
        aa::WirelessSetup blocked(blink[0], ap, 50000, 0);
        const long long since = monoMs();
        CHECK(!blocked.handshake(250));
        const long long elapsed = monoMs() - since;
        close(blink[0]);
        close(blink[1]);
        CHECK(elapsed >= 1500 && elapsed < 4000);
    }

    if (g_failures) {
        printf("FAIL wireless listen: %d checks failed\n", g_failures);
        return 1;
    }
    puts(
        "PASS Bluetooth link back during a session: nothing sent first, ping answered, requests left alone, closing "
        "seen");
    return 0;
}
