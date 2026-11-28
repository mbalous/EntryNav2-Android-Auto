// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The update from a USB stick on the car itself (update_install.hpp): /proc/mounts, the system partition remounted as
// the installation by hand does it, the car's own loader, the restart, and the locks. What this file decides from
// the car's answers (loaderFoundEverything, systemPartitionWritable, lockOwnFile) is in update_install.cpp, tested.
// The restart's car side too (evo_reboot.hpp), also for aa-supervisor --reboot: the only place that runs /sbin/reboot,
// never linked into a test.
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "evo_log.hpp"
#include "evo_process.hpp"
#include "evo_reboot.hpp"
#include "service_contract.hpp"
#include "update_install.hpp"
#include "update_key.hpp"

namespace evo {

namespace {

// The check of docs/INSTALLATION.md, step 2: the loader lists what a program would miss, without running any of it.
const char* const kLoaderEnv[] = {"LD_TRACE_LOADED_OBJECTS=1", "LD_WARN=yes", "LD_BIND_NOW=yes",
                                  "LD_LIBRARY_PATH=/usr/local/lib:/opt/hmi/lib:/usr/lib:/lib", nullptr};
const unsigned kRestartDelayS = 5;  // the menu says "restarting" meanwhile
// The iDrive app tries the supervisor's lock once a second to know whether Android Auto runs: an install waits out
// such a moment rather than answer "busy".
const unsigned kLockWaitMs = 2000;

std::string procMounts() {
    std::string mounts;
    const int fd = open("/proc/mounts", O_RDONLY | O_CLOEXEC);
    char buf[4096];
    ssize_t n;
    while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0 && mounts.size() < 256 * 1024) {
        mounts.append(buf, static_cast<size_t>(n));
    }
    if (fd >= 0) {
        close(fd);
    }
    return mounts;
}

// The restart on the car (evo_reboot.hpp): the vMOST session, the [Reboot] lines, the clock, today's restart.
class CarRestartHost : public RestartHost {
public:
    CarRestartHost(const char* maintenanceMarker, const char* logFile) : log_(maintenanceMarker, logFile) {}
    uint32_t nowMs() override { return monoMs(); }
    void sleepMs(unsigned ms) override {
        const uint32_t end = monoMs() + ms;
        for (;;) {
            const int32_t left = static_cast<int32_t>(end - monoMs());
            if (left <= 0) {
                return;
            }
            struct timespec req;
            req.tv_sec = left / 1000;
            req.tv_nsec = static_cast<long>(left % 1000) * 1000000L;
            // SIGTERM, SIGINT and SIGHUP end this process as the unit shuts down; any other signal shortens one turn.
            nanosleep(&req, nullptr);
        }
    }
    bool sendEcuReset() override { return session_.send(); }
    bool pump(unsigned ms, EcuResetWatch& watch) override { return session_.pump(ms, watch); }
    void endSession() override { session_.end(); }
    void log(const char* line) override { log_.line(line); }
    void closeLog() override { log_.close(); }
    // The restart as it always was: systemd stops the services first (no -f).
    void legacyReboot() override {
        sync();
        execl("/sbin/reboot", "reboot", static_cast<char*>(nullptr));
        execl("/bin/systemctl", "systemctl", "reboot", static_cast<char*>(nullptr));
    }

private:
    EcuResetSession session_;
    RebootLog log_;
};

class CarUpdateHost : public UpdateHost {
public:
    std::string mounts() override { return procMounts(); }
    bool systemWritable() override { return systemPartitionWritable(procMounts()); }
    bool setSystemWritable(bool writable) override {
        std::vector<std::string> args;
        args.push_back("-o");
        args.push_back(writable ? "remount,rw" : "remount,ro");
        args.push_back("/");
        std::string output;
        CommandReport report;
        if (!runCommand("/bin/mount", args, output, 30000, &report)) {
            ELOG("Update", "mount -o %s / refused (exit %d): %.200s", args[1].c_str(), report.exitCode,
                 report.text.c_str());
        }
        return systemWritable() == writable;
    }
    bool loads(const std::string& path, std::string& why) override {
        std::vector<std::string> args(1, path);
        std::string output;
        CommandReport report;
        runCommand(kCarLoader, args, output, 30000, &report, const_cast<char* const*>(kLoaderEnv));
        if (report.spawnError || report.timedOut || report.signal || report.truncated) {
            why = "the loader did not run to its end";
            return false;
        }
        return loaderFoundEverything(report.text, report.exitCode, why);
    }
    long long systemFreeBytes() override {
        struct statvfs fs;
        if (statvfs("/", &fs) != 0) {
            return -1;
        }
        return static_cast<long long>(fs.f_bavail) * static_cast<long long>(fs.f_frsize);
    }
    // The unit restarts a few seconds later, from a child of its own: the iDrive app reads this program's result
    // meanwhile. The child keeps this program's locks until the restart. It asks the M4 for its clean restart, and
    // falls back on today's /sbin/reboot (evo_reboot.hpp); its [Reboot] lines go to the service's kept log in the
    // maintenance profile only.
    void restartUnit() override {
        const pid_t pid = fork();
        if (pid == 0) {
            setsid();
            const int nullFd = open("/dev/null", O_RDWR);
            if (nullFd >= 0) {
                dup2(nullFd, STDIN_FILENO);
                dup2(nullFd, STDOUT_FILENO);
                dup2(nullFd, STDERR_FILENO);
                if (nullFd > 2) {
                    close(nullFd);
                }
            }
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            signal(SIGPIPE, SIG_IGN);
            CarRestartHost host(contract::kMaintenance, contract::kServiceLogKept);
            restartHeadUnit(host, kRestartDelayS * 1000);
            _exit(127);  // not even today's restart could start
        }
        if (pid < 0) {
            ELOG("Update", "cannot start the restart: restart the iDrive by hand");
        } else {
            ELOG("Update", "the unit restarts in %u s", kRestartDelayS);
        }
    }
    std::string stamp() override {
        char text[32];
        const time_t now = time(nullptr);
        struct tm local;
        localtime_r(&now, &local);
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
        return text;
    }
};

}  // namespace

UpdatePaths carUpdatePaths() {
    UpdatePaths paths;
    paths.pending = contract::kUpdatePending;
    paths.report = contract::kUpdateReport;
    paths.checked = contract::kUpdateChecked;
    paths.staging = contract::kUpdateStaging;
    paths.mediaPrefix = "/media/";
    return paths;
}

int updateFromUsb(bool install, const char* installedVersion) {
    signal(SIGPIPE, SIG_IGN);
    mkdir(contract::kDataDir, 0755);
    // Both locks last as long as this program (and its restart child): their descriptors stay open.
    if (lockOwnFile(contract::kUpdateLock, install ? kLockWaitMs : 0) < 0) {
        ELOG("Update", "another update from a USB stick runs");
        return static_cast<int>(UpdateResult::kBusy);
    }
    // The supervisor's own lock: no Android Auto starts during an install, none runs when it begins.
    if (install && lockOwnFile(contract::kWirelessLock, kLockWaitMs) < 0) {
        ELOG("Update", "Android Auto runs: stop it before the install");
        return static_cast<int>(UpdateResult::kBusy);
    }
    if (install) {
        // A few seconds the unit's shutdown can wait for: a signal must not stop the install between two files.
        sigset_t block;
        sigemptyset(&block);
        sigaddset(&block, SIGTERM);
        sigaddset(&block, SIGINT);
        sigaddset(&block, SIGHUP);
        sigprocmask(SIG_BLOCK, &block, nullptr);
    }
    CarUpdateHost host;
    Updater updater(host, carUpdatePaths(), trustedUpdateKeys(), installedVersion);
    const UpdateResult result = install ? updater.install() : updater.check();
    fputs(updater.report().c_str(), stdout);
    ELOG("Update", "%s: %s (report in %s)", install ? "install" : "check", updateResultText(result),
         contract::kUpdateReport);
    return static_cast<int>(result);
}

int rebootByHand() {
    signal(SIGPIPE, SIG_IGN);
    // The update's own lock, held until the restart: no update starts meanwhile, and none runs now (an install
    // restarts the unit itself, a few seconds after its last file).
    if (lockOwnFile(contract::kUpdateLock, 0) < 0) {
        puts("aa-supervisor --reboot: an update from a USB stick runs, and restarts the unit itself. No restart.");
        return 1;
    }
    struct stat st;
    if (lstat(contract::kUpdatePending, &st) == 0) {
        printf(
            "aa-supervisor --reboot: an update from a USB stick was cut short (%s): run it again from the stick, it "
            "finishes its files and restarts the unit. No restart.\n",
            contract::kUpdatePending);
        return 1;
    }
    printf(
        "aa-supervisor --reboot: asking the unit's processor (Cortex-M4) for a clean restart, as a BMW diagnostic "
        "tool does. Accepted: the unit shuts down by itself (the usual restart after %u s if it does not). No "
        "answer: the usual restart (/sbin/reboot) after %u s. Refused or impossible: the usual restart now.\n",
        kCleanRestartWaitMs / 1000, kSilentM4WaitMs / 1000);
    fflush(stdout);
    CarRestartHost host(nullptr, nullptr);  // the lines on this terminal
    restartHeadUnit(host, 0);
    puts("aa-supervisor --reboot: /sbin/reboot could not start: restart the iDrive by hand.");
    return 127;
}

}  // namespace evo
