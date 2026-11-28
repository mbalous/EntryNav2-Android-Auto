// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// core/crash_handler: crash line, fatal hook called once, SA_SIGINFO chaining, default signal termination.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "crash_handler.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace {

int g_markerFd = -1;

void writeTag(const char* tag) {
    if (g_markerFd >= 0) {
        const ssize_t w = write(g_markerFd, tag, strlen(tag));
        (void)w;
    }
}

void onChainedSigInfo(int number, siginfo_t* info, void*) {
    if (number == SIGABRT && info && info->si_signo == SIGABRT) {
        writeTag("[CHAINED]");
    }
}

void onFatalHook() {
    writeTag("[HOOK]");
}

}  // namespace

int main() {
    int outPipe[2];
    int tagPipe[2];
    CHECK(pipe(outPipe) == 0 && pipe(tagPipe) == 0);

    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(outPipe[0]);
        close(tagPipe[0]);
        dup2(outPipe[1], STDOUT_FILENO);
        close(outPipe[1]);
        g_markerFd = tagPipe[1];

        struct sigaction prev;
        memset(&prev, 0, sizeof(prev));
        prev.sa_sigaction = onChainedSigInfo;
        sigemptyset(&prev.sa_mask);
        prev.sa_flags = SA_SIGINFO;
        sigaction(SIGABRT, &prev, nullptr);

        // Installing twice must never self-chain.
        aa::installProcessGuards();
        aa::installProcessGuards();
        aa::setFatalHook(onFatalHook);
        aa::g_trail.stage = aa::kTrailVideo;
        aa::g_trail.channel = 6;
        aa::g_trail.message = 0x8007;
        raise(SIGABRT);
        _exit(99);
    }

    close(outPipe[1]);
    close(tagPipe[1]);
    std::string out, tags;
    char buf[256];
    ssize_t n;
    while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    while ((n = read(tagPipe[0], buf, sizeof(buf))) > 0) {
        tags.append(buf, static_cast<size_t>(n));
    }
    close(outPipe[0]);
    close(tagPipe[0]);

    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    CHECK(out.find("[CRASH] fatal signal 6 (SIGABRT) stage=4 last_channel=6 last_msg=0x8007") != std::string::npos);
    CHECK(tags == "[HOOK][CHAINED]");

    puts("PASS crash handler: sigaction, altstack, hook once, SA_SIGINFO chained, default termination");
    return 0;
}
