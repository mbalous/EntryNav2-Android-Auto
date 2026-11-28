// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// hmiapp/child_reaper.hpp with real children: a running one is never waited for, an ended or killed one is collected
// with its status, a pid that is not a child is let go, and a full table refuses without harm.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../hmiapp/child_reaper.hpp"

namespace {

using evo::app::ChildReaper;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

struct Seen {
    pid_t pid;
    int status;
    int calls;
};

void record(pid_t pid, int status, void* user) {
    Seen* seen = static_cast<Seen*>(user);
    seen->pid = pid;
    seen->status = status;
    ++seen->calls;
}

long nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

pid_t child(int exitCode, int sleepSeconds) {
    const pid_t pid = fork();
    if (pid == 0) {
        if (sleepSeconds) {
            sleep(sleepSeconds);
        }
        _exit(exitCode);
    }
    return pid;
}

// Polls the way the app's status poll does, every 20 ms, for at most 2 s.
int reapWithin(ChildReaper& reaper, Seen* seen) {
    for (int i = 0; i < 100; ++i) {
        const int n = reaper.reap(record, seen);
        if (n) {
            return n;
        }
        usleep(20000);
    }
    return 0;
}

void runningChildIsNeverWaitedFor() {
    ChildReaper reaper;
    Seen seen = {0, 0, 0};
    const pid_t pid = child(0, 30);
    CHECK(reaper.keep(pid));
    const long start = nowMs();
    CHECK(reaper.reap(record, &seen) == 0);
    CHECK(nowMs() - start < 100);  // the old code waited here for as long as the child lived
    CHECK(seen.calls == 0 && reaper.pending() == 1);
    kill(pid, SIGKILL);  // what jobEnded does past the limit
    CHECK(reapWithin(reaper, &seen) == 1);
    CHECK(seen.pid == pid && seen.status >= 0 && WIFSIGNALED(seen.status) && WTERMSIG(seen.status) == SIGKILL);
    CHECK(reaper.pending() == 0);
}

void endedChildKeepsItsExitCode() {
    ChildReaper reaper;
    Seen seen = {0, 0, 0};
    const pid_t pid = child(127, 0);  // the supervisor refused at exec
    CHECK(reaper.keep(pid));
    CHECK(reapWithin(reaper, &seen) == 1);
    CHECK(seen.pid == pid && WIFEXITED(seen.status) && WEXITSTATUS(seen.status) == 127);
    CHECK(reaper.reap(record, &seen) == 0 && seen.calls == 1);  // collected once only
}

void notAChildIsLetGo() {
    ChildReaper reaper;
    Seen seen = {0, 0, 0};
    CHECK(reaper.keep(getppid()));  // alive, but not ours: waitpid says ECHILD
    CHECK(reaper.reap(record, &seen) == 1);
    CHECK(seen.status == -1 && reaper.pending() == 0);
    CHECK(!reaper.keep(0) && !reaper.keep(-5));
}

void fullTableRefuses() {
    ChildReaper reaper;
    pid_t kids[ChildReaper::kSlots];
    for (int i = 0; i < ChildReaper::kSlots; ++i) {
        kids[i] = child(0, 30);
        CHECK(reaper.keep(kids[i]));
    }
    const pid_t extra = child(0, 30);
    CHECK(!reaper.keep(extra));
    CHECK(reaper.pending() == ChildReaper::kSlots);
    for (int i = 0; i < ChildReaper::kSlots; ++i) {
        kill(kids[i], SIGKILL);
    }
    kill(extra, SIGKILL);
    Seen seen = {0, 0, 0};
    for (int i = 0; i < 100 && reaper.pending(); ++i) {
        reaper.reap(record, &seen);
        usleep(20000);
    }
    CHECK(reaper.pending() == 0 && seen.calls == ChildReaper::kSlots);
    int status = 0;
    waitpid(extra, &status, 0);
}

}  // namespace

int main() {
    runningChildIsNeverWaitedFor();
    endedChildKeepsItsExitCode();
    notAChildIsLetGo();
    fullTableRefuses();
    if (g_failures) {
        printf("FAIL child reaper: %d checks failed\n", g_failures);
        return 1;
    }
    puts(
        "PASS child reaper: running child not waited for, killed and ended children collected once, foreign pid let "
        "go");
    return 0;
}
