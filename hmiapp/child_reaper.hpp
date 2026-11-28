// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The iDrive app's children, collected without ever waiting on its thread (HmiMain3D's): waitpid(WNOHANG) at each
// status poll. A job stopped after its limit can stay in the kernel for minutes (a USB stick pulled out mid-write:
// SIGKILL takes effect only when the kernel lets go), and waiting for it there froze the whole iDrive meanwhile. The
// supervisor started for the service is collected the same way, with no thread of its own.
// tests/test_child_reaper.cpp.
#ifndef ENTRY_AAD_HMIAPP_CHILD_REAPER_HPP
#define ENTRY_AAD_HMIAPP_CHILD_REAPER_HPP

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace evo {
namespace app {

class ChildReaper {
public:
    static const int kSlots = 8;
    // Called for each child collected: its pid and waitpid's status, or -1 when it was no longer ours to collect.
    typedef void (*Ended)(pid_t pid, int status, void* user);

    ChildReaper() {
        for (int i = 0; i < kSlots; ++i) {
            slots_[i] = 0;
        }
    }

    // Collect pid at a later poll. False when every slot is taken: that child then stays a zombie until the iDrive
    // restarts, which only costs a process table entry.
    bool keep(pid_t pid) {
        if (pid <= 0) {
            return false;
        }
        for (int i = 0; i < kSlots; ++i) {
            if (slots_[i] == 0) {
                slots_[i] = pid;
                return true;
            }
        }
        return false;
    }

    // Collects every kept child that has ended; never waits. Returns how many were collected.
    int reap(Ended ended, void* user) {
        int collected = 0;
        for (int i = 0; i < kSlots; ++i) {
            const pid_t pid = slots_[i];
            if (pid == 0) {
                continue;
            }
            int status = 0;
            pid_t r;
            do {
                r = waitpid(pid, &status, WNOHANG);
            } while (r < 0 && errno == EINTR);
            if (r == pid || (r < 0 && errno == ECHILD)) {
                slots_[i] = 0;
                ++collected;
                if (ended) {
                    ended(pid, r == pid ? status : -1, user);
                }
            }
        }
        return collected;
    }

    int pending() const {
        int n = 0;
        for (int i = 0; i < kSlots; ++i) {
            n += slots_[i] != 0;
        }
        return n;
    }

private:
    pid_t slots_[kSlots];
};

}  // namespace app
}  // namespace evo

#endif
