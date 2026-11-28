// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Symlink-safe atomic file helpers, non-blocking regular-file reads, media recall safety, and /proc identity checks.
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "../platform/atomic_file.hpp"
#include "../platform/media_recall.hpp"
#include "../platform/proc_identity.hpp"

int main() {
    char dir[] = "/tmp/entry-aad-secfs-XXXXXX";
    assert(mkdtemp(dir));
    const std::string base = dir;

    // 1. writeAtomicFile writes regular files and never follows a pre-planted symlink at the temporary name.
    const std::string target = base + "/state";
    const std::string victim = base + "/victim";
    assert(evo::writeAtomicFile(victim.c_str(), "keep\n", 5, 0644));
    char tmpName[256];
    snprintf(tmpName, sizeof(tmpName), "%s.tmp.%ld", target.c_str(), static_cast<long>(getpid()));
    assert(symlink(victim.c_str(), tmpName) == 0);
    assert(evo::writeAtomicFile(target.c_str(), "fresh\n", 6, 0644));

    char buf[64];
    assert(evo::readRegularTextFile(victim.c_str(), buf, sizeof(buf), true) == 5 && strcmp(buf, "keep\n") == 0);
    assert(evo::readRegularTextFile(target.c_str(), buf, sizeof(buf), true) == 6 && strcmp(buf, "fresh\n") == 0);

    // 2. readRegularTextFile rejects symlinks and FIFOs (without blocking).
    const std::string link = base + "/link";
    assert(symlink(target.c_str(), link.c_str()) == 0);
    assert(evo::readRegularTextFile(link.c_str(), buf, sizeof(buf), true) == -1);

    const std::string fifo = base + "/fifo";
    assert(mkfifo(fifo.c_str(), 0600) == 0);
    assert(evo::readRegularTextFile(fifo.c_str(), buf, sizeof(buf), true) == -1);

    // 3. Media recall consumes and rejects symlinks/FIFOs planted at its path.
    const std::string recall = base + "/recall";
    assert(evo::writeMediaRecall(recall.c_str(), 12345, 5000));
    assert(evo::takeMediaRecall(recall.c_str(), 12345, 5500));
    assert(!evo::takeMediaRecall(recall.c_str(), 12345, 5500));  // consumed
    assert(symlink(target.c_str(), recall.c_str()) == 0);
    assert(!evo::takeMediaRecall(recall.c_str(), 12345, 5500));
    assert(mkfifo(recall.c_str(), 0600) == 0);
    assert(!evo::takeMediaRecall(recall.c_str(), 12345, 5500));

    // 4. /proc identity verification: rejects self, PID <= 1, wrong executable, wrong start ticks, and exited child.
    assert(!evo::processMatches(0, "sleep"));
    assert(!evo::processMatches(1, "init"));
    assert(!evo::processMatches(getpid(), nullptr));

    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execlp("sleep", "sleep", "30", static_cast<char*>(nullptr));
        _exit(127);
    }
    // Wait briefly for exec to complete.
    char exe[64] = "";
    for (int i = 0; i < 100; ++i) {
        if (evo::readProcExeName(child, exe, sizeof(exe)) && strcmp(exe, "sleep") == 0) {
            break;
        }
        usleep(10000);
    }
    char ticks[32] = "";
    assert(evo::readProcStartTicks(child, ticks, sizeof(ticks)));
    assert(evo::processMatches(child, "sleep", ticks));
    assert(!evo::processMatches(child, "entry-aad", ticks));
    assert(!evo::processMatches(child, "sleep", "999999999999"));

    kill(child, SIGKILL);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(!evo::processMatches(child, "sleep", ticks));

    const std::string clean = "rm -rf '" + base + "'";
    assert(system(clean.c_str()) == 0);
    puts("PASS security fs & proc identity: atomic write, no symlink/FIFO follow, /proc exe + start ticks");
    return 0;
}
