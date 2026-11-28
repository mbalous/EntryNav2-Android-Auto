// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Descriptor hygiene at process start and around posix_spawn: close anything inherited from HmiMain3D (>= 3) and keep
// temporary descriptors out of the 0..2 range before dup2 file actions.
#ifndef ENTRY_AAD_PLATFORM_FD_HYGIENE_HPP
#define ENTRY_AAD_PLATFORM_FD_HYGIENE_HPP

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <vector>

namespace evo {

// Closes every open file descriptor >= firstFd, walking /proc/self/fd when available and falling back to a bounded
// range otherwise. Called at the start of aa-supervisor and entry-aad so descriptors inherited from HmiMain3D
// (Wayland, D-Bus, Etch, DRM, audio) are dropped immediately.
inline void closeInheritedFds(int firstFd = 3) {
    if (firstFd < 0) {
        firstFd = 0;
    }
    DIR* dir = opendir("/proc/self/fd");
    if (dir) {
        const int dirFd = dirfd(dir);
        std::vector<int> toClose;
        while (struct dirent* entry = readdir(dir)) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
                continue;
            }
            char* end = nullptr;
            const long fd = strtol(entry->d_name, &end, 10);
            if (!*end && fd >= firstFd && fd != dirFd && fd <= INT_MAX) {
                toClose.push_back(static_cast<int>(fd));
            }
        }
        closedir(dir);
        for (size_t i = 0; i < toClose.size(); ++i) {
            close(toClose[i]);
        }
        return;
    }
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < firstFd) {
        limit = 1024;
    } else if (limit > 65536) {
        limit = 65536;
    }
    for (int fd = firstFd; fd < static_cast<int>(limit); ++fd) {
        close(fd);
    }
}

// Moves a temporary descriptor out of 0..2 so posix_spawn_file_actions_adddup2/addclose cannot collide with stdio.
inline bool ensureFdAboveStdio(int& fd) {
    if (fd < 0 || fd > 2) {
        return fd >= 0;
    }
    const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (moved < 0) {
        return false;
    }
    close(fd);
    fd = moved;
    return true;
}

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_FD_HYGIENE_HPP
