// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Process guards: line-buffered stdout, SIGPIPE ignored, and a crash line that says where the session was.
#include "crash_handler.hpp"

#include <stdint.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>

namespace aa {

SessionTrail g_trail = {kTrailIdle, -1, -1};

namespace {

struct FatalSignal {
    int number;
    const char* name;
};
const FatalSignal kFatal[] = {
    {SIGSEGV, "SIGSEGV"}, {SIGBUS, "SIGBUS"}, {SIGILL, "SIGILL"}, {SIGFPE, "SIGFPE"}, {SIGABRT, "SIGABRT"}};
const size_t kFatalCount = sizeof(kFatal) / sizeof(kFatal[0]);

struct sigaction g_chained[kFatalCount];
bool g_chainedValid[kFatalCount];
FatalHook volatile g_hook = nullptr;
int g_hookDone = 0;
uint8_t g_altStack[16384];

// A line built without the C library: write(2) is all a signal handler may use.
class CrashLine {
public:
    CrashLine() : size_(0) {}
    CrashLine& text(const char* s) {
        while (s && *s && size_ < kTextRoom) {
            buffer_[size_++] = *s++;
        }
        return *this;
    }
    CrashLine& decimal(long value) {
        char digits[24];
        int count = 0;
        unsigned long rest = value < 0 ? 0ul - static_cast<unsigned long>(value) : static_cast<unsigned long>(value);
        do {
            digits[count++] = static_cast<char>('0' + rest % 10);
            rest /= 10;
        } while (rest && count < 22);
        if (value < 0 && size_ < kMaxLine) {
            buffer_[size_++] = '-';
        }
        while (count && size_ < kMaxLine) {
            buffer_[size_++] = digits[--count];
        }
        return *this;
    }
    // Upper case, no leading zero; "-1" for any negative value (no message yet).
    CrashLine& hex(int value) {
        if (value < 0) {
            return text("-1");
        }
        static const char kDigits[] = "0123456789ABCDEF";
        bool started = false;
        for (int shift = 28; shift >= 0; shift -= 4) {
            const int digit = (value >> shift) & 0xF;
            if ((digit || started || shift == 0) && size_ < kMaxLine) {
                buffer_[size_++] = kDigits[digit];
                started = true;
            }
        }
        return *this;
    }
    void writeTo(int fd) {
        if (size_ < sizeof(buffer_)) {
            buffer_[size_++] = '\n';
        }
        const ssize_t written = write(fd, buffer_, size_);
        (void)written;
    }

private:
    static const size_t kTextRoom = 200;  // leaves room for the numbers
    static const size_t kMaxLine = 255;   // leaves room for '\n'
    char buffer_[256];
    size_t size_;
};

void onFatalSignal(int number, siginfo_t* info, void* context) {
    const char* name = "";
    for (size_t i = 0; i < kFatalCount; ++i) {
        if (kFatal[i].number == number) {
            name = kFatal[i].name;
        }
    }
    CrashLine line;
    line.text("\n[CRASH] fatal signal ").decimal(number).text(" (").text(name).text(")");
    line.text(" stage=").decimal(g_trail.stage).text(" last_channel=").decimal(g_trail.channel);
    line.text(" last_msg=0x").hex(g_trail.message).writeTo(1);
    // The line is out first: the hook may wait on the network. Atomic test-and-set keeps two crashing threads from
    // running the hook concurrently, or re-entering it if the hook itself faults.
    const FatalHook hook = g_hook;
    if (hook && __sync_lock_test_and_set(&g_hookDone, 1) == 0) {
        hook();
    }
    for (size_t i = 0; i < kFatalCount; ++i) {
        if (kFatal[i].number == number && g_chainedValid[i]) {
            const struct sigaction& prev = g_chained[i];
            if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction) {
                prev.sa_sigaction(number, info, context);
            } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler != SIG_ERR) {
                prev.sa_handler(number);
            }
        }
    }
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(number, &dfl, nullptr);
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, number);
    sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
    raise(number);
}

}  // namespace

void installProcessGuards() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, nullptr);

    stack_t alt;
    alt.ss_sp = g_altStack;
    alt.ss_size = sizeof(g_altStack);
    alt.ss_flags = 0;
    const bool hasAltStack = sigaltstack(&alt, nullptr) == 0;

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_sigaction = onFatalSignal;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO | (hasAltStack ? SA_ONSTACK : 0);
    for (size_t i = 0; i < kFatalCount; ++i) {
        struct sigaction prev;
        if (sigaction(kFatal[i].number, &act, &prev) == 0) {
            const bool self = (prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction == onFatalSignal;
            if (!self) {  // called twice: never chain to itself
                g_chained[i] = prev;
                g_chainedValid[i] = true;
            }
        }
    }
}

void setFatalHook(FatalHook hook) {
    g_hook = hook;
}

}  // namespace aa
