// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Symlink-safe atomic file writes and non-blocking regular-file reads for runtime state in /tmp (no heap allocation).
#ifndef ENTRY_AAD_PLATFORM_ATOMIC_FILE_HPP
#define ENTRY_AAD_PLATFORM_ATOMIC_FILE_HPP

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace evo {

// Writes `len` bytes into a private temporary file next to `path` (never following symlinks) and renames it into
// place. Returns false and removes the temporary file on any write/close/rename error.
inline bool writeAtomicFile(const char* path, const void* data, size_t len, mode_t mode = 0644) {
    if (!path || !*path || (!data && len > 0)) {
        return false;
    }
    char tmp[256];
    const int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, static_cast<long>(getpid()));
    if (n <= 0 || n >= static_cast<int>(sizeof(tmp))) {
        return false;
    }
    unlink(tmp);  // drop any stale temp file or symlink left under our own PID name
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) {
        return false;
    }
    const char* p = static_cast<const char*>(data);
    size_t left = len;
    bool ok = true;
    while (left > 0) {
        const ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            ok = false;
            break;
        }
        p += w;
        left -= static_cast<size_t>(w);
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

// Reads a small regular file without following symlinks or blocking on FIFOs. When requireOwnUid is true, rejects
// files not owned by root or the effective UID. Returns bytes read (>= 0) or -1 on error.
inline ssize_t readRegularTextFile(const char* path, char* out, size_t cap, bool requireOwnUid = false) {
    if (!path || !*path || !out || cap < 2) {
        return -1;
    }
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (requireOwnUid && st.st_uid != 0 && st.st_uid != geteuid())) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got + 1 < cap) {
        const ssize_t n = read(fd, out + got, cap - 1 - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        got += static_cast<size_t>(n);
    }
    close(fd);
    out[got] = '\0';
    return static_cast<ssize_t>(got);
}

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_ATOMIC_FILE_HPP
