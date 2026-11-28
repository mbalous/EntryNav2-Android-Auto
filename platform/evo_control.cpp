// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Control channel between the iDrive app and the Android Auto service (see evo_control.hpp).
#include "evo_control.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "atomic_file.hpp"
#include "control_address.hpp"
#include "evo_log.hpp"

namespace evo {

bool parseControlCommand(const char* data, size_t len, ControlCommand& out) {
    if (!data || !len || len > 64 || memchr(data, '\0', len) != nullptr) {
        return false;
    }
    char buf[65];
    memcpy(buf, data, len);
    buf[len] = 0;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = 0;
    }
    if (!len || buf[0] == ' ') {
        return false;
    }
    char* sep = strchr(buf, ' ');
    const char* verb = buf;
    const char* argStr = nullptr;
    if (sep) {
        *sep = 0;
        argStr = sep + 1;
        while (*argStr == ' ') {
            ++argStr;
        }
        if (!*argStr) {
            return false;
        }
    }
    static const char* const known[] = {"ping", "show",     "hide",      "quit", "gear", "await",
                                        "pdc",  "daynight", "drivemode", "call", "popup"};
    bool ok = false;
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (!strcmp(verb, known[i])) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        return false;
    }
    const bool needsArg = !strcmp(verb, "gear") || !strcmp(verb, "pdc") || !strcmp(verb, "daynight") ||
                          !strcmp(verb, "call") || !strcmp(verb, "popup");
    const bool optionalArg = !strcmp(verb, "drivemode");
    if (argStr && !needsArg && !optionalArg) {
        return false;
    }
    if (!argStr && needsArg) {
        return false;
    }
    int arg = 0;
    if (argStr) {
        const char* p = argStr;
        if (*p == '-' || *p == '+') {
            ++p;
        }
        if (*p < '0' || *p > '9') {
            return false;
        }
        char* end = nullptr;
        errno = 0;
        const long parsed = strtol(argStr, &end, 10);
        if (errno != 0 || end == argStr || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
            return false;
        }
        arg = static_cast<int>(parsed);
    }
    out.verb = verb;
    out.arg = arg;
    out.hasArg = argStr != nullptr;
    return true;
}

EvoControl::EvoControl()
    : fd_(-1), started_(false), running_(false), lastWriteMs_(0), ignoredLogs_(0), cb_(nullptr), user_(nullptr) {}
EvoControl::~EvoControl() {
    stop();
}

bool EvoControl::start(const char* name, const char* stateFile, Callback cb, void* user) {
    if (started_) {
        return true;
    }
    fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        ELOG("Control", "no control channel: socket: %s", strerror(errno));
        return false;
    }
    int one = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) != 0) {
        ELOG("Control", "no control channel: SO_PASSCRED refused (%s)", strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }
    sockaddr_un a;
    const socklen_t length = controlAddress(name, a);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&a), length) != 0) {
        // Only a program that asks for this very name can hold it: "in use" is a service of ours, still running.
        ELOG("Control", "control channel \"%s\" not available (%s)%s", name, strerror(errno),
             errno == EADDRINUSE ? ": a service already runs" : "");
        close(fd_);
        fd_ = -1;
        return false;
    }
    stateFile_ = stateFile ? stateFile : "";
    cb_ = cb;
    user_ = user;
    running_ = true;
    if (pthread_create(&thread_, nullptr, threadMain, this) != 0) {
        running_ = false;
        close(fd_);
        fd_ = -1;
        return false;
    }
    started_ = true;
    ELOG("Control", "iDrive app commands on the control channel \"%s\"; state in %s", name, stateFile_.c_str());
    return true;
}

void EvoControl::stop() {
    if (!started_) {
        return;
    }
    running_ = false;
    pthread_join(thread_, nullptr);
    started_ = false;
    close(fd_);
    fd_ = -1;
    if (!stateFile_.empty()) {
        unlink(stateFile_.c_str());
    }
}

void EvoControl::writeState(const char* state, int overlay) {
    if (stateFile_.empty()) {
        return;
    }
    char text[64];
    snprintf(text, sizeof(text), "%s %d %d", state, static_cast<int>(getpid()), overlay);
    const uint32_t now = monoMs();
    if (last_ == text && now - lastWriteMs_ < 5000) {
        return;
    }
    char line[66];
    const int n = snprintf(line, sizeof(line), "%s\n", text);
    if (n > 0 && n < static_cast<int>(sizeof(line)) &&
        writeAtomicFile(stateFile_.c_str(), line, static_cast<size_t>(n), 0644)) {
        if (last_ != text) {
            ELOG("Control", "state -> %s", text);
        }
        last_ = text;
        lastWriteMs_ = now;
    }
}

void* EvoControl::threadMain(void* self) {
    static_cast<EvoControl*>(self)->run();
    return nullptr;
}

void EvoControl::run() {
    char buf[96];
    const uid_t ownUid = geteuid();
    while (running_) {
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 200) <= 0) {
            continue;
        }
        sockaddr_un from;
        memset(&from, 0, sizeof(from));
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        union {
            char bytes[CMSG_SPACE(sizeof(struct ucred))];
            struct cmsghdr align;
        } control;
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = &from;
        msg.msg_namelen = sizeof(from);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.bytes;
        msg.msg_controllen = sizeof(control.bytes);
        const ssize_t n = recvmsg(fd_, &msg, MSG_DONTWAIT);
        if (n <= 0) {
            continue;
        }
        bool hasCred = false;
        struct ucred cred;
        memset(&cred, 0, sizeof(cred));
        for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_CREDENTIALS &&
                cm->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
                memcpy(&cred, CMSG_DATA(cm), sizeof(cred));
                hasCred = true;
                break;
            }
        }
        if (!hasCred || !controlPeerAllowed(cred.uid, ownUid)) {
            if (++ignoredLogs_ <= 20 || ignoredLogs_ % 200 == 0) {
                ELOG("Control", "command refused: unauthorised peer uid=%ld (total %u)",
                     hasCred ? static_cast<long>(cred.uid) : -1L, ignoredLogs_);
            }
            continue;
        }
        ControlCommand c;
        if (!parseControlCommand(buf, static_cast<size_t>(n), c)) {
            if (++ignoredLogs_ <= 20 || ignoredLogs_ % 200 == 0) {
                ELOG("Control", "command ignored (%d B, total %u)", static_cast<int>(n), ignoredLogs_);
            }
            continue;
        }
        if (c.verb == "ping") {
            // A sender with no address of its own cannot be answered; never wait on a sender that does not read.
            const socklen_t fl = msg.msg_namelen;
            if (fl > sizeof(sa_family_t) && fl <= sizeof(from)) {
                sendto(fd_, "pong", 4, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), fl);
            }
            continue;
        }
        if (c.hasArg) {
            ELOG("Control", "app command: %s %d", c.verb.c_str(), c.arg);
        } else {
            ELOG("Control", "app command: %s", c.verb.c_str());
        }
        if (cb_) {
            cb_(c, user_);
        }
    }
}

}  // namespace evo
