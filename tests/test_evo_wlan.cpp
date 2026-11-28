// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Hotspot request (evo_wlan): exact MOST bytes, "only when off", never "off", and behaviour without vmostd/answers.
// Also the vMOST loader its users share (vmost_client): daemon socket, library, functions, and a connection the
// library drops itself, never closed twice.

// Uses tests/fake_vmostclient.c in place of the HU's libvmostclient.so.1 (no car, no network).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

#include "evo_wlan.hpp"
#include "vmost_client.hpp"

#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            return 1;                                            \
        }                                                        \
    } while (0)

static std::string readAll(const char* p) {
    std::string s;
    FILE* f = fopen(p, "r");
    if (!f) {
        return s;
    }
    char b[512];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) {
        s.append(b, n);
    }
    fclose(f);
    return s;
}

static bool has(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

static size_t count(const std::string& s, const char* part) {
    size_t n = 0;
    for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + 1)) {
        ++n;
    }
    return n;
}

static void noMessage(void*, void*) {}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test-evo-wlan <fake-libvmostclient.so> [the same without 'connected']\n");
        return 2;
    }
    uint8_t p[3];
    CHECK(evo::buildSelectWlanState3(p, 0x5741, true) == 3 && p[0] == 0x57 && p[1] == 0x41 && p[2] == 1);
    CHECK(evo::buildSelectWlanState3(p, 0x0102, false) == 3 && p[0] == 0x01 && p[1] == 0x02 && p[2] == 0);

    char dir[] = "/tmp/entry-wlan-test-XXXXXX";
    CHECK(mkdtemp(dir));
    std::string sock = std::string(dir) + "/vmost_sockstream", log = std::string(dir) + "/sent.log";
    setenv("FAKE_VMOST_LOG", log.c_str(), 1);  // read by the fake library

    // No vmostd socket: nothing is attempted.
    evo::setVmostForTest(sock.c_str(), argv[1]);
    evo::WlanRequestResult r;
    CHECK(!evo::requestWlanAccessPoint(500, &r) && !r.sent && readAll(log.c_str()).empty());
    evo::VmostClient none;
    CHECK(none.load() == evo::VmostClient::kNoDaemon);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(s >= 0);
    sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, sock.c_str(), sizeof(a.sun_path) - 1);
    CHECK(bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);

    {  // The loader: library missing, library without the vMOST functions, then every function resolved.
        evo::VmostClient v;
        evo::setVmostForTest(sock.c_str(), "/nonexistent/libvmostclient.so.1");
        CHECK(v.load() == evo::VmostClient::kNoLibrary && !v.error().empty() && !v.loaded());
        evo::setVmostForTest(sock.c_str(), "libc.so.6");
        CHECK(v.load() == evo::VmostClient::kNoSymbols && !v.loaded());
        evo::setVmostForTest(sock.c_str(), argv[1]);
        CHECK(v.load() == evo::VmostClient::kLoaded && v.loaded() && !v.linked());
        v.unload();
        CHECK(!v.loaded() && evo::kVmostFunctionCount == 7);
    }

    // The library drops the connection itself after a failed read (as the car's does): neither a deregistration nor
    // a disconnection follows it, which would free its queue twice (the fake aborts then, as glibc on the car).
    {
        unlink(log.c_str());
        setenv("FAKE_VMOST_GONE_AFTER", "1", 1);
        setenv("FAKE_VMOST_GONE_HOW", "broken", 1);
        evo::VmostClient v;
        CHECK(v.load() == evo::VmostClient::kLoaded && v.connect(noMessage, nullptr, 0) == 0 && v.linked());
        CHECK(v.shadowRegister(0xA0, 0x01) == 0);
        evo::VmostMsgOut m;
        static const uint8_t none[4] = {0, 0, 0, 0};
        evo::buildVmostGet(m, 0xA0, 0x01, 0xC10, none);
        CHECK(v.send(&m) == 0 && v.linked());  // taken; vmostd leaves right after
        CHECK(v.processMessages() != 0 && !v.linked());
        CHECK(v.shadowDeregister(0xA0, 0x01) == evo::VmostClient::kNotConnected);
        CHECK(v.shadowRegister(0xA0, 0x01) == evo::VmostClient::kNotConnected);
        CHECK(v.send(&m) == evo::VmostClient::kNotConnected);  // the library's own answer
        CHECK(v.disconnect() == 0 && !v.linked());
        v.unload();
        const std::string l = readAll(log.c_str());
        CHECK(count(l, "disconnect\n") == 1 && has(l, "the library disconnects itself (a read failed)\n"));
        CHECK(!has(l, "deregister") && count(l, "\nregister ") == 1);
        // Hung up: the library still holds its side, which is let go once; nothing asked of vmostd any more.
        unlink(log.c_str());
        setenv("FAKE_VMOST_GONE_HOW", "hangup", 1);
        CHECK(v.load() == evo::VmostClient::kLoaded && v.connect(noMessage, nullptr, 0) == 0);
        CHECK(v.send(&m) == 0 && v.linked() && v.disconnect() == 0 && !v.linked() && v.disconnect() == 0);
        CHECK(v.shadowDeregister(0xA0, 0x01) == evo::VmostClient::kNotConnected);  // after our own disconnection
        v.unload();
        CHECK(count(readAll(log.c_str()), "disconnect\n") == 1);
        unsetenv("FAKE_VMOST_GONE_AFTER");
        unsetenv("FAKE_VMOST_GONE_HOW");
    }
    if (argc > 2) {  // A build of the library without 'connected': VmostClient follows its own calls, as before.
        unlink(log.c_str());
        evo::setVmostForTest(sock.c_str(), argv[2]);
        evo::VmostClient v;
        CHECK(v.load() == evo::VmostClient::kLoaded && !v.linked() && v.connect(noMessage, nullptr, 0) == 0);
        CHECK(v.linked() && v.shadowRegister(0xA0, 0x01) == 0 && v.shadowDeregister(0xA0, 0x01) == 0);
        CHECK(v.disconnect() == 0 && !v.linked() && v.disconnect() == 0);
        v.unload();
        const std::string l = readAll(log.c_str());
        CHECK(count(l, "disconnect\n") == 1 && has(l, "deregister a0.01\n"));
        evo::setVmostForTest(sock.c_str(), argv[1]);
    }

    // vmostd leaves once it took the state read: the switch-on is not asked, nothing is deregistered, and the
    // connection is let go once (ours after a hang-up, the library's own after a broken read).
    {
        const char* const ways[] = {"hangup", "broken"};
        for (size_t i = 0; i < sizeof(ways) / sizeof(ways[0]); ++i) {
            unlink(log.c_str());
            setenv("FAKE_WLAN_STATE", "0", 1);
            setenv("FAKE_VMOST_GONE_AFTER", "1", 1);
            setenv("FAKE_VMOST_GONE_HOW", ways[i], 1);
            CHECK(!evo::requestWlanAccessPoint(1000, &r) && r.connected && !r.sent && r.stateBefore == -1);
            const std::string l = readAll(log.c_str());
            if (!has(l, "vmostd leaves") || has(l, "c11") || has(l, "deregister") || count(l, "disconnect\n") != 1) {
                fprintf(stderr, "FAIL hotspot, vmostd gone (%s):\n%s", ways[i], l.c_str());
                return 1;
            }
        }
        unsetenv("FAKE_VMOST_GONE_AFTER");
        unsetenv("FAKE_VMOST_GONE_HOW");
        unlink(log.c_str());
    }

    // Hotspot off: Get, then the CarPlay-switch request, ResultAck and state 1 afterwards.
    setenv("FAKE_WLAN_STATE", "0", 1);
    CHECK(evo::requestWlanAccessPoint(1000, &r));
    CHECK(r.connected && r.stateBefore == 0 && r.sent && r.replyOp == 0xD && r.stateAfter == 1);
    std::string l = readAll(log.c_str());
    CHECK(l.find("register a0.01\n") != std::string::npos);
    CHECK(l.find("send addr=ffff a0.01.c10 op=1 len=0 :\n") != std::string::npos);
    CHECK(l.find("send addr=ffff a0.01.c11 op=6 len=3 : 57 41 01\n") != std::string::npos);
    CHECK(l.find("deregister a0.01\n") != std::string::npos);
    CHECK(l.find(" 00\n") == std::string::npos);  // never an "off" payload
    unlink(log.c_str());

    // Hotspot already on: read only.
    setenv("FAKE_WLAN_STATE", "1", 1);
    CHECK(evo::requestWlanAccessPoint(1000, &r) && r.stateBefore == 1 && !r.sent);
    l = readAll(log.c_str());
    CHECK(l.find("c10 op=1") != std::string::npos && l.find("c11") == std::string::npos);
    unlink(log.c_str());

    // wland silent: state unknown after 1 s, the request is still sent once, no answer recorded.
    setenv("FAKE_WLAN_SILENT", "1", 1);
    CHECK(evo::requestWlanAccessPoint(300, &r) && r.stateBefore == -1 && r.sent && r.replyOp == -1);
    l = readAll(log.c_str());
    CHECK(l.find("c11 op=6 len=3 : 57 41 01") != std::string::npos);
    unsetenv("FAKE_WLAN_SILENT");

    close(s);
    unlink(sock.c_str());
    unlink(log.c_str());
    rmdir(dir);
    puts(
        "PASS hotspot : A0.01.C10 Get puis A0.01.C11 StartResultAck 57 41 01 seulement si eteint, jamais 'eteindre', "
        "rien sans vmostd, envoi unique si wland ne repond pas, connexion lachee une seule fois si vmostd part");
    return 0;
}
