// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The unit's restart (evo_reboot): the request's bytes, the answers told apart, every way out on a stand-in car with a
// fake clock, the vMOST session on a stand-in library (tests/fake_vmostclient.c), and the maintenance-only log.
// Nothing here can restart the PC or remount anything: the car's host (platform/update_car.cpp) is not linked.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "evo_log.hpp"
#include "evo_reboot.hpp"
#include "vmost_client.hpp"

#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            return 1;                                            \
        }                                                        \
    } while (0)

namespace {

std::string readAll(const std::string& path) {
    std::string s;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        return s;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        s.append(buf, n);
    }
    fclose(f);
    return s;
}

bool has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

size_t count(const std::string& text, const char* part) {
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) {
        ++n;
    }
    return n;
}

evo::VmostMsgView makeMsg(uint8_t inst, uint32_t op, const uint8_t* data, uint16_t len, uint16_t addr = 0x0100,
                          uint8_t fblock = 0xFD, uint16_t fkt = 0x0C00) {
    evo::VmostMsgView m;
    m.addr = addr;
    m.fblock = fblock;
    m.inst = inst;
    m.fkt = fkt;
    m.op = op;
    m.len = len;
    m.data = data;
    return m;
}

// ---- a stand-in car on a fake clock ------------------------------------------------------------------------------

// What vmostd hands over, afterMs after the request was sent. breaks: the session breaks there instead.
struct Event {
    uint32_t afterMs;
    uint16_t addr;
    uint8_t inst;
    uint32_t op;
    std::vector<uint8_t> data;
    bool breaks;
};

Event answer(uint32_t afterMs, std::vector<uint8_t> data, uint8_t inst = 0x04, uint32_t op = 0xC,
             uint16_t addr = 0x0100) {
    Event e;
    e.afterMs = afterMs;
    e.addr = addr;
    e.inst = inst;
    e.op = op;
    e.data = data;
    e.breaks = false;
    return e;
}

Event breakAt(uint32_t afterMs) {
    Event e = answer(afterMs, std::vector<uint8_t>());
    e.breaks = true;
    return e;
}

std::vector<uint8_t> bytes(const uint8_t* b, size_t n) {
    return std::vector<uint8_t>(b, b + n);
}

const uint8_t kAccepted[] = {0xF4, 0x63, 0x51, 0x01};
const uint8_t kPending[] = {0xF4, 0x63, 0x7F, 0x11, 0x78};
const uint8_t kRefused22[] = {0xF4, 0x63, 0x7F, 0x11, 0x22};
const uint8_t kRefused7F[] = {0xF4, 0x63, 0x7F, 0x11, 0x7F};
const uint8_t kRefused33[] = {0xF4, 0x63, 0x7F, 0x11, 0x33};
const uint8_t kOtherTester[] = {0xF1, 0x63, 0x51, 0x01};
const uint8_t kRequest[] = {0xF4, 0x63, 0x11, 0x01};
const uint8_t kMostError[] = {0x02, 0x01};

class ScriptHost : public evo::RestartHost {
public:
    explicit ScriptHost(const std::vector<Event>& events, bool sendOk = true)
        : script(events),
          sendOk_(sendOk),
          now(5000000),
          start(5000000),
          sentAt(0),
          rebootAt(0),
          reboots(0),
          ends(0),
          logClosed(false),
          loggedAfterClose(false),
          next_(0) {}

    uint32_t nowMs() override { return now; }
    void sleepMs(unsigned ms) override {
        char text[32];
        snprintf(text, sizeof(text), "sleep %u", ms);
        calls.push_back(text);
        now += ms;
    }
    bool sendEcuReset() override {
        calls.push_back("send");
        sentAt = now;
        return sendOk_;
    }
    bool pump(unsigned ms, evo::EcuResetWatch& watch) override {
        calls.push_back("pump");
        const uint32_t end = now + ms;
        while (next_ < script.size()) {
            const Event& e = script[next_];
            const uint32_t at = sentAt + e.afterMs;
            if (static_cast<int32_t>(at - end) > 0) {
                break;
            }
            if (static_cast<int32_t>(at - now) > 0) {
                now = at;
            }
            ++next_;
            if (e.breaks) {
                return false;
            }
            watch.take(makeMsg(e.inst, e.op, e.data.empty() ? nullptr : &e.data[0],
                               static_cast<uint16_t>(e.data.size()), e.addr),
                       now);
            if (watch.settled()) {
                return true;
            }
        }
        now = end;
        return true;
    }
    void endSession() override {
        calls.push_back("end");
        ++ends;
    }
    void log(const char* line) override {
        loggedAfterClose |= logClosed;
        lines.push_back(line);
    }
    void closeLog() override {
        calls.push_back("close-log");
        logClosed = true;
    }
    void legacyReboot() override {
        calls.push_back("reboot");
        ++reboots;
        rebootAt = now;
    }
    std::string allLines() const {
        std::string s;
        for (size_t i = 0; i < lines.size(); ++i) {
            s += lines[i] + "\n";
        }
        return s;
    }

    std::vector<Event> script;
    bool sendOk_;
    uint32_t now, start, sentAt, rebootAt;
    unsigned reboots, ends;
    bool logClosed, loggedAfterClose;
    std::vector<std::string> calls, lines;

private:
    size_t next_;
};

// One restart on the stand-in car: the way taken, today's restart once, after afterSendMs counted from the request
// (from the start when nothing could be sent), the log closed before any wait and never written after, and the line
// that says why.
int runCase(const char* name, const std::vector<Event>& script, bool sendOk, evo::RestartWay way, uint32_t afterSendMs,
            const char* why) {
    ScriptHost host(script, sendOk);
    const evo::RestartWay got = evo::restartHeadUnit(host, 5000);
    const std::string lines = host.allLines();
    if (got != way || host.reboots != 1 || host.ends != 1 || host.loggedAfterClose || !has(lines, why) ||
        host.calls.empty() || host.calls[0] != "sleep 5000" || host.calls.back() != "reboot") {
        fprintf(stderr, "FAIL %s: way %d reboots %u ends %u logged after close %d\n%s", name, static_cast<int>(got),
                host.reboots, host.ends, host.loggedAfterClose, lines.c_str());
        return 1;
    }
    const uint32_t from = sendOk ? host.sentAt : host.start;
    const uint32_t after = host.rebootAt - from;
    if (after != afterSendMs || (sendOk && host.sentAt != host.start + 5000)) {
        fprintf(stderr, "FAIL %s: today's restart %u ms after the request, expected %u\n", name, after, afterSendMs);
        return 1;
    }
    // The log is closed before the wait, and nothing but that wait comes between it and the restart.
    size_t closed = 0;
    for (size_t i = 0; i < host.calls.size(); ++i) {
        if (host.calls[i] == "close-log") {
            closed = i;
        }
    }
    const size_t rest = host.calls.size() - closed;
    const bool waits = way != evo::RestartWay::kLegacyNow;
    if (host.calls[closed] != "close-log" || rest != (waits ? 3u : 2u)) {
        fprintf(stderr, "FAIL %s: the log is not closed right before the wait and the restart\n", name);
        return 1;
    }
    if (!sendOk && host.calls.size() != 5) {  // sleep, send, end, close-log, reboot: no pump
        fprintf(stderr, "FAIL %s: something ran after a request that could not be sent\n", name);
        return 1;
    }
    printf("ok %s: %s", name, lines.substr(lines.rfind('\n', lines.size() - 2) + 1).c_str());
    return 0;
}

// ---- the real session on the stand-in library, with a stand-in for the rest of the car -------------------------

class SessionHost : public evo::RestartHost {
public:
    SessionHost() : reboots(0) {}
    uint32_t nowMs() override { return evo::monoMs(); }
    void sleepMs(unsigned ms) override { sleeps.push_back(ms); }
    bool sendEcuReset() override { return session.send(); }
    bool pump(unsigned ms, evo::EcuResetWatch& watch) override { return session.pump(ms, watch); }
    void endSession() override { session.end(); }
    void log(const char* line) override { lines += std::string(line) + "\n"; }
    void closeLog() override {}
    void legacyReboot() override { ++reboots; }
    evo::EcuResetSession session;
    std::vector<unsigned> sleeps;
    std::string lines;
    unsigned reboots;
};

int restoreStdio(int savedOut, int savedErr) {
    fflush(stdout);
    fflush(stderr);
    return dup2(savedOut, STDOUT_FILENO) == STDOUT_FILENO && dup2(savedErr, STDERR_FILENO) == STDERR_FILENO;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test-evo-reboot <fake-libvmostclient.so>\n");
        return 2;
    }

    // 1. The request: tester 0xF4 to the unit 0x63, ECUReset 0x11, hardReset 0x01.
    uint8_t req[4] = {0, 0, 0, 0};
    CHECK(evo::buildEcuHardReset(req) == 4);
    CHECK(req[0] == 0xF4 && req[1] == 0x63 && req[2] == 0x11 && req[3] == 0x01);

    // 2. Each message told apart.
    const uint8_t pos3[] = {0xF4, 0x63, 0x51};
    const uint8_t pos5[] = {0xF4, 0x63, 0x51, 0x01, 0x02};
    const uint8_t posOtherSub[] = {0xF4, 0x63, 0x51, 0x03};
    const uint8_t otherService[] = {0xF4, 0x63, 0x7F, 0x22, 0x31};
    const uint8_t otherUnit[] = {0xF4, 0x40, 0x51, 0x01};
    const uint8_t shortRefusal[] = {0xF4, 0x63, 0x7F, 0x11};
    int code = -1;
    using evo::EcuResetAnswer;
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, kAccepted, 4), &code) == EcuResetAnswer::kAccepted);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x41, 0xC, kAccepted, 4)) == EcuResetAnswer::kAccepted);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0x0, kAccepted, 4)) == EcuResetAnswer::kAccepted);  // any operation
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, pos3, 3)) == EcuResetAnswer::kAccepted);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, pos5, 5)) == EcuResetAnswer::kAccepted);
    CHECK(code == -1);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, posOtherSub, 4)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, kPending, 5), &code) == EcuResetAnswer::kPending && code == -1);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, kRefused22, 5), &code) == EcuResetAnswer::kRefused &&
          code == 0x22);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x41, 0xC, kRefused33, 5), &code) == EcuResetAnswer::kRefused &&
          code == 0x33);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xF, kMostError, 2), &code) == EcuResetAnswer::kVmostError &&
          code == 0x02);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x41, 0x9, kMostError, 2), &code) == EcuResetAnswer::kVmostError);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xF, nullptr, 0), &code) == EcuResetAnswer::kVmostError && code == -1);
    // A vMOST error answer carrying UDS bytes is still the transport refusing.
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xF, kAccepted, 4)) == EcuResetAnswer::kVmostError);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, otherService, 5)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, kOtherTester, 4)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, otherUnit, 4)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, shortRefusal, 4)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0x0, kRequest, 4)) == EcuResetAnswer::kNone);  // a copy of ours
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, kAccepted, 4, 0x0200)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xF, kMostError, 2, 0x0200)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x05, 0xC, kAccepted, 4)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, kAccepted, 4, 0x0100, 0xA0)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, kAccepted, 4, 0x0100, 0xFD, 0x0C04)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, kAccepted, 2)) == EcuResetAnswer::kNone);
    CHECK(evo::parseEcuResetAnswer(makeMsg(0x04, 0xC, nullptr, 4)) == EcuResetAnswer::kNone);

    // 3. The watch: the first final answer stands; "response pending" is noted once, with its time.
    {
        evo::EcuResetWatch w;
        CHECK(!w.settled() && w.answer() == EcuResetAnswer::kNone && !w.pendingSeen());
        CHECK(!w.take(makeMsg(0x04, 0xC, kOtherTester, 4), 10));
        CHECK(w.take(makeMsg(0x04, 0xC, kPending, 5), 20) && w.pendingSeen() && w.pendingAtMs() == 20);
        CHECK(w.take(makeMsg(0x04, 0xC, kPending, 5), 30) && w.pendingAtMs() == 20);
        CHECK(!w.settled() && w.answer() == EcuResetAnswer::kPending);
        CHECK(w.take(makeMsg(0x04, 0xC, kAccepted, 4), 40) && w.settled());
        CHECK(w.take(makeMsg(0x04, 0xC, kRefused22, 5), 50));
        CHECK(w.answer() == EcuResetAnswer::kAccepted && w.code() == -1 && w.answers() == 4);
        evo::EcuResetWatch e;
        CHECK(e.take(makeMsg(0x41, 0x9, kMostError, 2), 5) && e.settled());
        CHECK(e.answer() == EcuResetAnswer::kVmostError && e.errorOp() == 0x9 && e.code() == 0x02);
    }

    // 4. Every way out, on the stand-in car (fake clock: the 30 s and 120 s waits take no time).
    using evo::RestartWay;
    const uint32_t clean = evo::kCleanRestartWaitMs, silent = evo::kSilentM4WaitMs;
    std::vector<Event> s;
    int failed = 0;
    s.assign(1, answer(100, bytes(kAccepted, 4)));
    failed |= runCase("accepted", s, true, RestartWay::kLegacyAfterClean, 100 + clean, "took the request (51 01)");
    s.assign(1, answer(100, bytes(kAccepted, 4), 0x41));
    failed |= runCase("accepted on FD.41", s, true, RestartWay::kLegacyAfterClean, 100 + clean, "took the request");
    // 78 then 51 after the first 2 s: the window is 5 s from the 78.
    s.clear();
    s.push_back(answer(100, bytes(kPending, 5)));
    s.push_back(answer(3000, bytes(kAccepted, 4)));
    failed |= runCase("78 then 51", s, true, RestartWay::kLegacyAfterClean, 3000 + clean, "took the request");
    // 78 then a refusal: today's restart at once, the clean one is not waited for.
    s.clear();
    s.push_back(answer(100, bytes(kPending, 5)));
    s.push_back(answer(4000, bytes(kRefused22, 5)));
    failed |= runCase("78 then refused", s, true, RestartWay::kLegacyNow, 4000, "refused the request (7f 11 22)");
    // 78 and nothing final: waited for as accepted, from the end of the 5 s.
    s.assign(1, answer(100, bytes(kPending, 5)));
    failed |= runCase("78 alone", s, true, RestartWay::kLegacyAfterClean, 5100 + clean, "response pending");
    // A second 78 does not lengthen the window: the 51 after it comes too late and is not seen.
    s.clear();
    s.push_back(answer(100, bytes(kPending, 5)));
    s.push_back(answer(4000, bytes(kPending, 5)));
    s.push_back(answer(5500, bytes(kAccepted, 4)));
    failed |= runCase("78 twice", s, true, RestartWay::kLegacyAfterClean, 5100 + clean, "response pending");
    s.assign(1, answer(50, bytes(kRefused22, 5)));
    failed |= runCase("refused 22", s, true, RestartWay::kLegacyNow, 50, "refused the request (7f 11 22)");
    s.assign(1, answer(50, bytes(kRefused7F, 5)));
    failed |= runCase("refused 7F", s, true, RestartWay::kLegacyNow, 50, "refused the request (7f 11 7f)");
    s.assign(1, answer(50, bytes(kRefused33, 5), 0x41));
    failed |= runCase("refused 33", s, true, RestartWay::kLegacyNow, 50, "refused the request (7f 11 33)");
    s.assign(1, answer(50, bytes(kMostError, 2), 0x04, 0xF));
    failed |= runCase("vMOST error 0xF", s, true, RestartWay::kLegacyNow, 50, "error (operation 0xf, code 2)");
    s.assign(1, answer(50, bytes(kMostError, 2), 0x04, 0x9));
    failed |= runCase("vMOST error 0x9", s, true, RestartWay::kLegacyNow, 50, "error (operation 0x9, code 2)");
    s.clear();
    failed |= runCase("silent", s, true, RestartWay::kLegacyAfterSilence, 2000 + silent, "no answer from the M4 in");
    failed |= runCase("not sent", s, false, RestartWay::kLegacyNow, 5000, "not sent");
    s.assign(1, answer(100, bytes(kOtherTester, 4)));
    failed |= runCase("other tester", s, true, RestartWay::kLegacyAfterSilence, 2000 + silent, "no answer");
    s.assign(1, answer(100, bytes(kAccepted, 4), 0x04, 0xC, 0x0200));
    failed |= runCase("other address", s, true, RestartWay::kLegacyAfterSilence, 2000 + silent, "no answer");
    s.assign(1, answer(10, bytes(kRequest, 4), 0x04, 0x0));
    failed |= runCase("copy of ours", s, true, RestartWay::kLegacyAfterSilence, 2000 + silent, "no answer");
    s.assign(1, breakAt(500));
    failed |= runCase("session broke", s, true, RestartWay::kLegacyAfterSilence, 500 + silent, "session broke");
    s.clear();
    s.push_back(answer(100, bytes(kPending, 5)));
    s.push_back(breakAt(300));
    failed |= runCase("78 then broke", s, true, RestartWay::kLegacyAfterClean, 300 + clean, "response pending");
    CHECK(failed == 0);

    // 5. The vMOST session on the stand-in library.
    char dir[] = "/tmp/entry-reboot-test-XXXXXX";
    CHECK(mkdtemp(dir));
    const std::string sock = std::string(dir) + "/vmost_sockstream";
    const std::string sent = std::string(dir) + "/sent.log";
    setenv("FAKE_VMOST_LOG", sent.c_str(), 1);
    unsetenv("FAKE_WLAN_SILENT");
    unsetenv("FAKE_VMOST_FAIL_REGISTER");
    unsetenv("FAKE_ECURESET_MODE");
    evo::setVmostForTest(sock.c_str(), argv[1]);

    // 5a. No vmostd: nothing sent, today's restart at once.
    {
        evo::EcuResetSession session;
        CHECK(!session.send());
        session.end();
        CHECK(readAll(sent).empty());
        SessionHost host;
        CHECK(evo::restartHeadUnit(host, 0) == RestartWay::kLegacyNow && host.reboots == 1);
        CHECK(host.sleeps.size() == 1 && host.sleeps[0] == 0 && has(host.lines, "not sent"));
    }

    const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    struct Mode {
        const char* name;
        bool sends;
        EcuResetAnswer answer;
        int code;
    };
    const Mode modes[] = {
        {"ok", true, EcuResetAnswer::kAccepted, -1},          {"pending_ok", true, EcuResetAnswer::kAccepted, -1},
        {"alt_inst", true, EcuResetAnswer::kAccepted, -1},    {"nrc22", true, EcuResetAnswer::kRefused, 0x22},
        {"most_error", true, EcuResetAnswer::kVmostError, 2}, {"other_tester", true, EcuResetAnswer::kNone, -1},
        {"other_address", true, EcuResetAnswer::kNone, -1},   {"echo", true, EcuResetAnswer::kNone, -1},
        {"send_refused", false, EcuResetAnswer::kNone, -1},
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        const Mode& m = modes[i];
        unlink(sent.c_str());
        setenv("FAKE_ECURESET_MODE", m.name, 1);
        evo::EcuResetSession session;
        evo::EcuResetWatch watch;
        const bool sends = session.send();
        CHECK(sends == m.sends);
        CHECK(session.registered(0x04) && session.registered(0x41));
        if (sends) {
            CHECK(session.pump(m.answer == EcuResetAnswer::kNone ? 150 : 1000, watch));
        }
        session.end();
        const std::string log = readAll(sent);
        if (watch.answer() != m.answer || watch.code() != m.code) {
            fprintf(stderr, "FAIL mode %s: answer %d code %d\n", m.name, static_cast<int>(watch.answer()),
                    watch.code());
            return 1;
        }
        CHECK(has(log, "register fd.04\nregister fd.41\nsend addr=0100 fd.04.c00 op=0 len=4 : f4 63 11 01\n"));
        if (m.sends) {
            CHECK(has(log, "deregister fd.04\nderegister fd.41\ndisconnect\n") && count(log, "disconnect\n") == 1);
        } else {
            // The write failed: the library dropped the connection itself, and nothing follows (vmost_client.hpp).
            CHECK(has(log, "the library disconnects itself (a write failed)\ndisconnect\n"));
            CHECK(!has(log, "deregister") && count(log, "disconnect\n") == 1);
        }
        if (!strcmp(m.name, "pending_ok")) {
            CHECK(watch.pendingSeen() && watch.answers() == 2);
        }
        if (!strcmp(m.name, "most_error")) {
            CHECK(watch.errorOp() == 0xF);
        }
        if (!strcmp(m.name, "echo") || !strcmp(m.name, "other_tester") || !strcmp(m.name, "other_address")) {
            CHECK(watch.answers() == 0);
        }
    }

    // 5b. Copies refused: the request still goes, nothing to give back.
    unlink(sent.c_str());
    setenv("FAKE_VMOST_FAIL_REGISTER", "1", 1);
    setenv("FAKE_ECURESET_MODE", "ok", 1);
    {
        evo::EcuResetSession session;
        evo::EcuResetWatch watch;
        CHECK(session.send() && !session.registered(0x04) && !session.registered(0x41));
        CHECK(session.pump(1000, watch) && watch.answer() == EcuResetAnswer::kAccepted);
        session.end();
        const std::string log = readAll(sent);
        CHECK(has(log, "send addr=0100 fd.04.c00 op=0 len=4 : f4 63 11 01\n") && !has(log, "deregister"));
    }
    unsetenv("FAKE_VMOST_FAIL_REGISTER");

    // 5b'. vmostd leaves once it took the request: it hangs up, or the read breaks. The session gives nothing back and
    // lets the connection go once: ours after a hang-up, the library's own after the broken read. A deregistration
    // there, or a second disconnection, freed the library's queue twice on the car (free(): invalid pointer).
    {
        const char* const ways[] = {"hangup", "broken"};
        for (size_t i = 0; i < sizeof(ways) / sizeof(ways[0]); ++i) {
            unlink(sent.c_str());
            setenv("FAKE_ECURESET_MODE", "silent", 1);
            setenv("FAKE_VMOST_GONE_AFTER", "1", 1);
            setenv("FAKE_VMOST_GONE_HOW", ways[i], 1);
            evo::EcuResetSession session;
            evo::EcuResetWatch watch;
            CHECK(session.send() && session.registered(0x04) && session.registered(0x41));
            CHECK(!session.pump(1000, watch) && watch.answer() == EcuResetAnswer::kNone);
            session.end();
            const std::string log = readAll(sent);
            if (!has(log, "f4 63 11 01\nvmostd leaves") || has(log, "deregister") || count(log, "disconnect\n") != 1) {
                fprintf(stderr, "FAIL session, vmostd gone (%s):\n%s", ways[i], log.c_str());
                return 1;
            }
            // The whole restart: no answer before the session broke, today's restart 30 s later.
            SessionHost host;
            if (evo::restartHeadUnit(host, 0) != RestartWay::kLegacyAfterSilence || host.reboots != 1 ||
                !has(host.lines, "before the vMOST session broke")) {
                fprintf(stderr, "FAIL whole restart, vmostd gone (%s):\n%s", ways[i], host.lines.c_str());
                return 1;
            }
        }
        unsetenv("FAKE_VMOST_GONE_AFTER");
        unsetenv("FAKE_VMOST_GONE_HOW");
        unsetenv("FAKE_ECURESET_MODE");
    }

    // 5c. The whole restart on the real session: accepted, refused, vMOST error, not sent, silent.
    struct Whole {
        const char* mode;
        RestartWay way;
        unsigned wait;
    };
    const Whole wholes[] = {
        {"ok", RestartWay::kLegacyAfterClean, clean},        {"nrc22", RestartWay::kLegacyNow, 0},
        {"most_error", RestartWay::kLegacyNow, 0},           {"send_refused", RestartWay::kLegacyNow, 0},
        {"silent", RestartWay::kLegacyAfterSilence, silent},
    };
    for (size_t i = 0; i < sizeof(wholes) / sizeof(wholes[0]); ++i) {
        setenv("FAKE_ECURESET_MODE", wholes[i].mode, 1);
        SessionHost host;
        const RestartWay way = evo::restartHeadUnit(host, 0);
        const size_t sleeps = wholes[i].wait ? 2 : 1;
        if (way != wholes[i].way || host.reboots != 1 || host.sleeps.size() != sleeps ||
            (wholes[i].wait && host.sleeps[1] != wholes[i].wait)) {
            fprintf(stderr, "FAIL whole %s: way %d reboots %u\n%s", wholes[i].mode, static_cast<int>(way), host.reboots,
                    host.lines.c_str());
            return 1;
        }
    }
    unsetenv("FAKE_ECURESET_MODE");

    // 6. The log: maintenance profile only, never through a link, never opened again once closed.
    const std::string marker = std::string(dir) + "/maintenance";
    const std::string kept = std::string(dir) + "/service.log";
    const std::string link = std::string(dir) + "/link.log";
    const std::string target = std::string(dir) + "/target.log";
    const int savedOut = dup(STDOUT_FILENO), savedErr = dup(STDERR_FILENO);
    CHECK(savedOut >= 0 && savedErr >= 0);
    {
        evo::RebootLog log(marker.c_str(), kept.c_str());  // production: no marker
        log.line("production line");
        log.close();
        CHECK(restoreStdio(savedOut, savedErr));
        CHECK(access(kept.c_str(), F_OK) != 0);
    }
    const int mfd = open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(mfd >= 0 && close(mfd) == 0);
    {
        evo::RebootLog log(marker.c_str(), kept.c_str());
        log.line("first line");
        log.line("second line");
        log.close();
        log.line("after the close");
        CHECK(restoreStdio(savedOut, savedErr));
        const std::string text = readAll(kept);
        CHECK(has(text, "[Reboot] first line\n") && has(text, "[Reboot] second line\n"));
        CHECK(!has(text, "after the close"));
    }
    {
        // The restart's lines through the log: closed before the wait, nothing after.
        unlink(kept.c_str());
        setenv("FAKE_ECURESET_MODE", "nrc22", 1);
        class LoggedHost : public SessionHost {
        public:
            LoggedHost(const char* m, const char* f) : file(m, f) {}
            void log(const char* line) override { file.line(line); }
            void closeLog() override { file.close(); }
            evo::RebootLog file;
        };
        LoggedHost host(marker.c_str(), kept.c_str());
        CHECK(evo::restartHeadUnit(host, 0) == RestartWay::kLegacyNow && host.reboots == 1);
        CHECK(restoreStdio(savedOut, savedErr));
        const std::string text = readAll(kept);
        CHECK(has(text, "ECUReset hardReset 0100.fd.04.c00 Set f4 63 11 01: sent (0)"));
        CHECK(has(text, "operation 0xc: f4 63 7f 11 22"));
        CHECK(has(text, "refused the request (7f 11 22): the usual restart (/sbin/reboot) now"));
        unsetenv("FAKE_ECURESET_MODE");
    }
    {
        CHECK(symlink(target.c_str(), link.c_str()) == 0);
        evo::RebootLog log(marker.c_str(), link.c_str());
        log.line("through a link");
        log.close();
        CHECK(restoreStdio(savedOut, savedErr));
        CHECK(access(target.c_str(), F_OK) != 0);
    }
    close(savedOut);
    close(savedErr);

    unsetenv("FAKE_VMOST_LOG");
    close(listener);
    unlink(link.c_str());
    unlink(kept.c_str());
    unlink(marker.c_str());
    unlink(sock.c_str());
    unlink(sent.c_str());
    rmdir(dir);

    puts(
        "PASS reboot: request F4 63 11 01 on 0100.FD.04.C00; accepted or pending = clean restart waited 120 s, silent "
        "= 30 s, refused / vMOST error / not sent = usual restart at once; copies of FD.04 and FD.41; log in "
        "maintenance only, closed before the wait; vmostd gone or a failed write: the connection let go once, nothing "
        "deregistered");
    return 0;
}
