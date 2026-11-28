// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Linux /proc process identity checks: verify executable name and start ticks before signalling a PID from a file.
#ifndef ENTRY_AAD_PLATFORM_PROC_IDENTITY_HPP
#define ENTRY_AAD_PLATFORM_PROC_IDENTITY_HPP

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "atomic_file.hpp"

namespace evo {

// Field 22 (starttime in clock ticks since boot) of /proc/<pid>/stat; survives spaces and parentheses in comm.
inline bool readProcStartTicks(pid_t pid, char* out, size_t cap) {
    if (pid <= 1 || !out || cap < 2) {
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", static_cast<long>(pid));
    char statBuf[512];
    if (readRegularTextFile(path, statBuf, sizeof(statBuf)) <= 0) {
        return false;
    }
    const char* closeParen = strrchr(statBuf, ')');
    if (!closeParen) {
        return false;
    }
    const char* p = closeParen + 1;
    // After ')': field 3 is state, field 22 is starttime (20th space-delimited token after ')').
    for (int field = 3; field <= 22; ++field) {
        while (*p == ' ') {
            ++p;
        }
        if (!*p) {
            return false;
        }
        const char* start = p;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r') {
            ++p;
        }
        if (field == 22) {
            const size_t len = static_cast<size_t>(p - start);
            if (len == 0 || len >= cap) {
                return false;
            }
            memcpy(out, start, len);
            out[len] = '\0';
            return true;
        }
    }
    return false;
}

// Basename of /proc/<pid>/exe (stripping any trailing " (deleted)"), falling back to /proc/<pid>/comm.
inline bool readProcExeName(pid_t pid, char* out, size_t cap) {
    if (pid <= 1 || !out || cap < 2) {
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/exe", static_cast<long>(pid));
    char target[256];
    const ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = '\0';
        const size_t delLen = 10;  // " (deleted)"
        if (static_cast<size_t>(n) > delLen && strcmp(target + n - delLen, " (deleted)") == 0) {
            target[n - delLen] = '\0';
        }
        const char* slash = strrchr(target, '/');
        const char* base = slash ? slash + 1 : target;
        const size_t len = strlen(base);
        if (len > 0 && len < cap) {
            memcpy(out, base, len + 1);
            return true;
        }
    }
    snprintf(path, sizeof(path), "/proc/%ld/comm", static_cast<long>(pid));
    char comm[64];
    if (readRegularTextFile(path, comm, sizeof(comm)) <= 0) {
        return false;
    }
    size_t len = strlen(comm);
    while (len > 0 && (comm[len - 1] == '\n' || comm[len - 1] == '\r')) {
        comm[--len] = '\0';
    }
    if (len == 0 || len >= cap) {
        return false;
    }
    memcpy(out, comm, len + 1);
    return true;
}

// True when `pid` is a distinct live process (> 1, != getpid()) matching `expectedName` (if given) and `expectedTicks`
// (if given). Guards against PID reuse and spoofed state files before sending SIGTERM / SIGKILL.
inline bool processMatches(pid_t pid, const char* expectedName, const char* expectedTicks = nullptr) {
    if (pid <= 1 || pid == getpid()) {
        return false;
    }
    char ticks[32];
    if (!readProcStartTicks(pid, ticks, sizeof(ticks))) {
        return false;
    }
    if (expectedTicks && *expectedTicks && strcmp(ticks, expectedTicks) != 0) {
        return false;
    }
    if (expectedName && *expectedName) {
        char exe[64];
        if (!readProcExeName(pid, exe, sizeof(exe)) || strcmp(exe, expectedName) != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_PROC_IDENTITY_HPP
