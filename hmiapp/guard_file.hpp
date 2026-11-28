// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The guards' files (app_guards.hpp): one date per line, the app's loads or its automatic starts. Only the recent
// ones count, and a file is replaced whole, never left half written. Pure file logic: tests/test_guard_file.cpp.
#ifndef ENTRY_AAD_HMIAPP_GUARD_FILE_HPP
#define ENTRY_AAD_HMIAPP_GUARD_FILE_HPP

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace evo {
namespace app {

const int kGuardDatesMax = 8;  // dates kept in a guard file: more than any guard counts

// A date within window seconds of now, on either side. The car's date can step back a little between two starts (a
// line just ahead still counts), but a line hours ahead (a date set back since, or never set) would count for ever.
// Differences in 64 bits: long is 32 bits on the car.
inline bool guardDateRecent(long date, long now, long window) {
    const long long d = static_cast<long long>(now) - static_cast<long long>(date);
    return d < window && -d < window;
}

// The recent dates of path, at most kGuardDatesMax, into dates; how many. A missing file counts nothing, and so does a
// line that is not a date (one cut by a power loss while an older version appended it).
inline int readGuardDates(const char* path, long now, long window, long* dates) {
    FILE* f = fopen(path, "re");
    if (!f) {
        return 0;
    }
    int n = 0;
    char line[64];
    while (n < kGuardDatesMax && fgets(line, sizeof(line), f)) {
        char* end = nullptr;
        errno = 0;
        const long date = strtol(line, &end, 10);
        if (end == line || errno != 0 || (*end != '\n' && *end != '\0')) {
            continue;
        }
        if (guardDateRecent(date, now, window)) {
            dates[n++] = date;
        }
    }
    fclose(f);
    return n;
}

// Replaces path with dates, one per line: written beside it, synced, then renamed over it, so a power loss leaves
// either the old file or the new one. False when it could not be written (the old one then stays as it was).
inline bool writeGuardDates(const char* path, const long* dates, int n) {
    char tmp[256];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= static_cast<int>(sizeof(tmp))) {
        return false;
    }
    char text[kGuardDatesMax * 24];
    int len = 0;
    for (int i = 0; i < n && i < kGuardDatesMax; ++i) {
        len += snprintf(text + len, sizeof(text) - len, "%ld\n", dates[i]);
    }
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    for (int done = 0; ok && done < len;) {
        const ssize_t w = write(fd, text + done, static_cast<size_t>(len - done));
        if (w > 0) {
            done += static_cast<int>(w);
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            ok = false;
        }
    }
    ok = fsync(fd) == 0 && ok;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

// One turn of a guard (max: kGuardDatesMax at most): true while fewer than max dates of path are recent, now then
// added to them and the old ones dropped; false at max, the file left as it is. *recent: how many were. A file that
// cannot be written does not stop the start: it only cannot be counted.
inline bool guardTurn(const char* path, long now, long window, int max, int* recent) {
    long dates[kGuardDatesMax];
    int n = readGuardDates(path, now, window, dates);
    *recent = n;
    if (n >= max || n >= kGuardDatesMax) {
        return false;
    }
    dates[n++] = now;
    writeGuardDates(path, dates, n);
    return true;
}

}  // namespace app
}  // namespace evo

#endif
