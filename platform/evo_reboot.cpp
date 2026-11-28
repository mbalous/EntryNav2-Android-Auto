// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The unit's restart: the M4's clean restart asked over vMOST, today's restart otherwise (evo_reboot.hpp).
#include "evo_reboot.hpp"

#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "evo_log.hpp"

namespace evo {

size_t buildEcuHardReset(uint8_t out[4]) {
    out[0] = kDiagTester;
    out[1] = kDiagUnit;
    out[2] = kUdsEcuReset;
    out[3] = kUdsHardReset;
    return 4;
}

EcuResetAnswer parseEcuResetAnswer(const VmostMsgView& msg, int* code) {
    if (msg.addr != kM4Address || msg.fblock != kDiagFblock ||
        (msg.inst != kDiagInstance && msg.inst != kDiagAnswerInstance) || msg.fkt != kDiagFunction) {
        return EcuResetAnswer::kNone;
    }
    if (msg.op == kDiagMostErrorAck || msg.op == kDiagMostError) {
        if (code) {
            *code = msg.len >= 1 && msg.data ? msg.data[0] : -1;
        }
        return EcuResetAnswer::kVmostError;
    }
    if (!msg.data || msg.len < 3 || msg.data[0] != kDiagTester || msg.data[1] != kDiagUnit) {
        return EcuResetAnswer::kNone;
    }
    const uint8_t sid = msg.data[2];
    if (sid == kUdsEcuResetAccepted && (msg.len == 3 || msg.data[3] == kUdsHardReset)) {
        return EcuResetAnswer::kAccepted;
    }
    if (sid == kUdsRefusal && msg.len >= 5 && msg.data[3] == kUdsEcuReset) {
        if (msg.data[4] == kUdsResponsePending) {
            return EcuResetAnswer::kPending;
        }
        if (code) {
            *code = msg.data[4];
        }
        return EcuResetAnswer::kRefused;
    }
    return EcuResetAnswer::kNone;
}

EcuResetWatch::EcuResetWatch()
    : answer_(EcuResetAnswer::kNone), code_(-1), op_(0), pendingSeen_(false), pendingAtMs_(0), answers_(0) {}

bool EcuResetWatch::settled() const {
    return answer_ == EcuResetAnswer::kAccepted || answer_ == EcuResetAnswer::kRefused ||
           answer_ == EcuResetAnswer::kVmostError;
}

bool EcuResetWatch::take(const VmostMsgView& msg, uint32_t nowMs) {
    int code = -1;
    const EcuResetAnswer a = parseEcuResetAnswer(msg, &code);
    if (a == EcuResetAnswer::kNone) {
        return false;
    }
    ++answers_;
    if (settled()) {
        return true;  // the first final answer stands
    }
    if (a == EcuResetAnswer::kPending) {
        if (!pendingSeen_) {
            pendingSeen_ = true;
            pendingAtMs_ = nowMs;
        }
        answer_ = EcuResetAnswer::kPending;
        return true;
    }
    answer_ = a;
    if (a == EcuResetAnswer::kRefused || a == EcuResetAnswer::kVmostError) {
        code_ = code;
    }
    if (a == EcuResetAnswer::kVmostError) {
        op_ = msg.op;
    }
    return true;
}

namespace {

void __attribute__((format(printf, 2, 3))) say(RestartHost& host, const char* fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    host.log(text);
}

}  // namespace

RestartWay restartHeadUnit(RestartHost& host, unsigned delayMs) {
    host.sleepMs(delayMs);
    say(host,
        "asking the unit's processor (Cortex-M4) for a clean restart: ECUReset hardReset %02x %02x %02x %02x on "
        "%04x.%02x.%02x.%03x, as DiagProxyIC forwards a BMW tester's request",
        kDiagTester, kDiagUnit, kUdsEcuReset, kUdsHardReset, kM4Address, kDiagFblock, kDiagInstance, kDiagFunction);
    if (!host.sendEcuReset()) {
        host.endSession();
        say(host, "the request was not sent: the usual restart (/sbin/reboot) now");
        host.closeLog();
        host.legacyReboot();
        return RestartWay::kLegacyNow;
    }
    EcuResetWatch watch;
    uint32_t deadline = host.nowMs() + kEcuResetAnswerMs;
    bool lengthened = false, broken = false;
    while (!watch.settled()) {
        if (!lengthened && watch.pendingSeen()) {
            lengthened = true;
            const uint32_t later = watch.pendingAtMs() + kEcuResetPendingMs;
            if (static_cast<int32_t>(later - deadline) > 0) {
                deadline = later;
            }
        }
        const int32_t left = static_cast<int32_t>(deadline - host.nowMs());
        if (left <= 0) {
            break;
        }
        if (!host.pump(static_cast<unsigned>(left), watch)) {
            broken = true;
            break;
        }
    }
    host.endSession();

    RestartWay way = RestartWay::kLegacyNow;
    unsigned waitMs = 0;
    switch (watch.answer()) {
        case EcuResetAnswer::kAccepted:
            say(host, "the M4 took the request (%02x %02x): the usual restart only if the unit still runs in %u s",
                kUdsEcuResetAccepted, kUdsHardReset, kCleanRestartWaitMs / 1000);
            way = RestartWay::kLegacyAfterClean;
            waitMs = kCleanRestartWaitMs;
            break;
        case EcuResetAnswer::kPending:
            say(host,
                "the M4 answered 'response pending' (%02x %02x %02x) and nothing final%s: the usual restart only if "
                "the unit still runs in %u s",
                kUdsRefusal, kUdsEcuReset, kUdsResponsePending, broken ? " before the vMOST session broke" : "",
                kCleanRestartWaitMs / 1000);
            way = RestartWay::kLegacyAfterClean;
            waitMs = kCleanRestartWaitMs;
            break;
        case EcuResetAnswer::kRefused:
            say(host, "the M4 refused the request (%02x %02x %02x): the usual restart (/sbin/reboot) now", kUdsRefusal,
                kUdsEcuReset, static_cast<unsigned>(watch.code()));
            break;
        case EcuResetAnswer::kVmostError:
            say(host, "vMOST answered with an error (operation 0x%x, code %d): the usual restart (/sbin/reboot) now",
                static_cast<unsigned>(watch.errorOp()), watch.code());
            break;
        case EcuResetAnswer::kNone:
            if (broken) {
                say(host,
                    "no answer from the M4 before the vMOST session broke: the usual restart (/sbin/reboot) in "
                    "%u s, unless the unit restarts first",
                    kSilentM4WaitMs / 1000);
            } else {
                say(host,
                    "no answer from the M4 in %u ms: the usual restart (/sbin/reboot) in %u s, unless the unit "
                    "restarts first",
                    kEcuResetAnswerMs, kSilentM4WaitMs / 1000);
            }
            way = RestartWay::kLegacyAfterSilence;
            waitMs = kSilentM4WaitMs;
            break;
    }
    host.closeLog();
    if (waitMs) {
        host.sleepMs(waitMs);
    }
    host.legacyReboot();
    return way;
}

// ---- the vMOST session ------------------------------------------------------------------------------------------

EcuResetSession::EcuResetSession()
    : loaded_(false),
      connected_(false),
      registered04_(false),
      registered41_(false),
      linkLost_(false),
      fd_(-1),
      watch_(nullptr) {}

EcuResetSession::~EcuResetSession() {
    end();
}

bool EcuResetSession::registered(uint8_t instance) const {
    return instance == kDiagInstance ? registered04_ : instance == kDiagAnswerInstance ? registered41_ : false;
}

void EcuResetSession::onMessage(void* msg, void* user) {
    EcuResetSession* s = static_cast<EcuResetSession*>(user);
    VmostMsgView v;
    if (!s || !parseVmostMsg(msg, v) || v.fblock != kDiagFblock) {
        return;
    }
    char hex[3 * 16 + 1];
    size_t o = 0;
    hex[0] = 0;
    for (size_t i = 0; i < v.len && i < 16; ++i) {
        o += snprintf(hex + o, sizeof(hex) - o, i ? " %02x" : "%02x", v.data[i]);
    }
    const bool ours = s->watch_ && s->watch_->take(v, monoMs());
    ELOG("Reboot", "vMOST %04x.%02x.%02x.%03x operation 0x%x: %s%s", v.addr, v.fblock, v.inst, v.fkt,
         static_cast<unsigned>(v.op), hex, ours ? "" : " (not an answer to ours)");
}

bool EcuResetSession::send() {
    switch (vmost_.load()) {
        case VmostClient::kLoaded:
            break;
        case VmostClient::kNoDaemon:
            ELOG("Reboot", "vmostd socket missing");
            return false;
        case VmostClient::kNoLibrary:
            ELOG("Reboot", "libvmostclient.so.1: %s", vmost_.error().c_str());
            return false;
        case VmostClient::kNoSymbols:
            ELOG("Reboot", "vMOST functions missing");
            return false;
    }
    loaded_ = true;
    if (vmost_.connect(onMessage, this, 0) != 0) {
        ELOG("Reboot", "vmostd refused the connection");
        return false;
    }
    connected_ = true;
    // The copies of both instances DiagProxyIC reads answers on; the request still goes without them.
    int rc = vmost_.shadowRegister(kDiagFblock, kDiagInstance);
    registered04_ = rc == 0;
    const int rc41 = vmost_.shadowRegister(kDiagFblock, kDiagAnswerInstance);
    registered41_ = rc41 == 0;
    if (!registered04_ || !registered41_) {
        ELOG("Reboot", "copies of FD.04 (%d) and FD.41 (%d) not all granted: the request goes anyway", rc, rc41);
    }
    if (vmost_.fileDescriptorGet(&fd_) != 0) {
        fd_ = -1;
    }
    uint8_t payload[4];
    VmostMsgOut m;
    memset(&m, 0, sizeof(m));
    m.addr = kM4Address;
    m.fblock = kDiagFblock;
    m.inst = kDiagInstance;
    m.fkt = kDiagFunction;
    m.op = kMostOpSet;
    m.len = static_cast<uint16_t>(buildEcuHardReset(payload));
    m.data = payload;
    m.channel = 0;
    rc = vmost_.send(&m);
    linkLost_ = !vmost_.linked();  // a failed write: the library dropped the connection itself
    ELOG("Reboot", "ECUReset hardReset %04x.%02x.%02x.%03x Set %02x %02x %02x %02x: %s (%d)%s", m.addr, m.fblock,
         m.inst, m.fkt, payload[0], payload[1], payload[2], payload[3], rc == 0 ? "sent" : "refused", rc,
         linkLost_ ? ", the vMOST library closed the connection" : "");
    return rc == 0;
}

bool EcuResetSession::pump(unsigned ms, EcuResetWatch& watch) {
    if (!connected_ || linkLost_ || fd_ < 0) {
        return false;
    }
    watch_ = &watch;
    const uint32_t end = monoMs() + ms;
    bool ok = true;
    while (!watch.settled() && static_cast<int32_t>(end - monoMs()) > 0) {
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLIN;
        p.revents = 0;
        const int32_t left = static_cast<int32_t>(end - monoMs());
        const int r = poll(&p, 1, left > 50 ? 50 : left);
        if (r < 0 && errno != EINTR) {
            ok = false;
            break;
        }
        if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            ELOG("Reboot", "vmostd closed the connection");
            linkLost_ = true;
            ok = false;
            break;
        }
        if (r > 0 && (p.revents & POLLIN) && vmost_.processMessages() != 0) {
            ELOG("Reboot", "vMOST reading interrupted: the library closed the connection");
            linkLost_ = true;
            ok = false;
            break;
        }
    }
    watch_ = nullptr;
    return ok;
}

void EcuResetSession::end() {
    if (connected_) {
        // Nothing more to ask of a lost connection: a deregistration would only fail, and make the library disconnect.
        const bool lost = linkLost_ || !vmost_.linked();
        if (registered04_ && !lost) {
            vmost_.shadowDeregister(kDiagFblock, kDiagInstance);
        }
        if (registered41_ && !lost) {
            vmost_.shadowDeregister(kDiagFblock, kDiagAnswerInstance);
        }
        vmost_.disconnect();  // once, and not after the library's own (vmost_client.hpp)
    }
    if (loaded_) {
        vmost_.unload();
    }
    loaded_ = connected_ = registered04_ = registered41_ = linkLost_ = false;
    fd_ = -1;
    watch_ = nullptr;
}

// ---- the log ------------------------------------------------------------------------------------------------------

RebootLog::RebootLog(const char* maintenanceMarker, const char* logFile)
    : marker_(maintenanceMarker), file_(logFile), tried_(false), redirected_(false), closed_(false) {}

void RebootLog::line(const char* text) {
    if (closed_) {
        return;
    }
    if (!tried_ && marker_ && file_) {
        tried_ = true;
        if (access(marker_, F_OK) == 0) {
            const int fd = open(file_, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
            struct stat st;
            if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid()) {
                fflush(stdout);
                fflush(stderr);
                redirected_ = dup2(fd, STDOUT_FILENO) == STDOUT_FILENO;
                if (redirected_) {
                    dup2(fd, STDERR_FILENO);
                }
            }
            if (fd > STDERR_FILENO) {
                ::close(fd);
            }
        }
    }
    ELOG("Reboot", "%s", text);
}

void RebootLog::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    fflush(stdout);
    fflush(stderr);
    if (!redirected_) {
        return;
    }
    fsync(STDOUT_FILENO);
    const int nullFd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nullFd >= 0) {
        dup2(nullFd, STDOUT_FILENO);
        dup2(nullFd, STDERR_FILENO);
        if (nullFd > STDERR_FILENO) {
            ::close(nullFd);
        }
    }
    redirected_ = false;
}

}  // namespace evo
