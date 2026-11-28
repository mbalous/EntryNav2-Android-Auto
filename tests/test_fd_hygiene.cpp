// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// platform/fd_hygiene & platform/evo_process: inherited descriptors closed, stdio safe, SIGPIPE restored in children.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "evo_process.hpp"
#include "fd_hygiene.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

int main() {
    // 1. closeInheritedFds() drops every descriptor >= 3 while leaving 0..2 untouched.
    int p1[2], p2[2];
    CHECK(pipe(p1) == 0 && pipe(p2) == 0);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        evo::closeInheritedFds();
        const bool closed = fcntl(p1[0], F_GETFD) < 0 && errno == EBADF && fcntl(p1[1], F_GETFD) < 0 &&
                            errno == EBADF && fcntl(p2[0], F_GETFD) < 0 && errno == EBADF &&
                            fcntl(p2[1], F_GETFD) < 0 && errno == EBADF;
        const bool stdioOk = fcntl(STDOUT_FILENO, F_GETFD) >= 0;
        _exit(closed && stdioOk ? 0 : 1);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(p1[0]);
    close(p1[1]);
    close(p2[0]);
    close(p2[1]);

    // 2. ensureFdAboveStdio() moves a descriptor in 0..2 above 2 with FD_CLOEXEC set.
    const pid_t movedChild = fork();
    CHECK(movedChild >= 0);
    if (movedChild == 0) {
        close(STDIN_FILENO);
        int fd = open("/dev/null", O_RDONLY);
        const bool ok = fd == 0 && evo::ensureFdAboveStdio(fd) && fd >= 3 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
        if (fd >= 0) {
            close(fd);
        }
        _exit(ok ? 0 : 1);
    }
    CHECK(waitpid(movedChild, &status, 0) == movedChild && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // 3. runCommand() runs a child cleanly even when the parent ignores SIGPIPE, and kills a timed-out child.
    signal(SIGPIPE, SIG_IGN);
    std::string out;
    evo::CommandReport rep;
    CHECK(evo::runCommand("/bin/sh", std::vector<std::string>{"-c", "echo ok"}, out, 2000, &rep));
    CHECK(out == "ok\n" && !rep.timedOut && rep.exitCode == 0);

    CHECK(!evo::runCommand("/bin/sh", std::vector<std::string>{"-c", "sleep 5 & wait"}, out, 150, &rep));
    CHECK(rep.timedOut);

    puts("PASS fd hygiene: closeInheritedFds, ensureFdAboveStdio, runCommand timeout and SIGPIPE");
    return 0;
}
