// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The off switch, the crash guard, the boot-loop guard and the automatic start (app_guards.hpp).
#include "app_guards.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../platform/service_contract.hpp"
#include "app_log.hpp"
#include "guard_file.hpp"
#include "mono_clock.hpp"
#include "service_link.hpp"

namespace evo {
namespace app {

namespace {

const long kAutostartDelayS = 30;
const int kBootGuardMax = 3;
const long kBootGuardWindowS = 15 * 60;
const int kCrashGuardMax = 3;  // starts without 2 min of uptime before the app turns itself off
// Only loads this recent count: three short trips over days are not a crash loop, and turned the app off until someone
// removed the file over SSH (#4).
const long kCrashGuardWindowS = 15 * 60;
const long kStableAfterS = 120;

bool g_autostartDone = false, g_stableMarked = false;

bool fileExists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// A setting that is a file: there or not. content: what a new file holds.
void setSwitch(const char* what, const char* path, bool on, const char* content) {
    if (!on) {
        if (unlink(path) != 0 && errno != ENOENT) {
            alog("%s: cannot remove %s (errno %d)", what, path, errno);
        }
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        alog("%s: cannot create %s (errno %d)", what, path, errno);
        return;
    }
    ssize_t w = write(fd, content, strlen(content));
    (void)w;
    fsync(fd);
    close(fd);
}

// Both guards count on the date, not on the monotonic clock (mono_clock.hpp): the starts they count can be in
// different runs of the unit, and that clock starts again at each.
bool bootGuardAllows() {
    int n = 0;
    if (!guardTurn(contract::kBootGuard, static_cast<long>(time(nullptr)), kBootGuardWindowS, kBootGuardMax, &n)) {
        alog("autostart SKIPPED by the bootloop guard (%d starts within %ld min): use Start in the menu", n,
             kBootGuardWindowS / 60);
        return false;
    }
    return true;
}

}  // namespace

bool appDisabled() {
    if (fileExists(contract::kAppDisable)) {
        alog("disabled by %s", contract::kAppDisable);
        return true;
    }
    return false;
}

// ---- crash guard (HMI crash loop protection) ------------------------------------------------------------------------
// create() adds one line; 2 minutes later the status poll deletes the file. kCrashGuardMax lines of the last
// kCrashGuardWindowS = the HMI went down that many times shortly after loading us: create() then refuses (iDrive starts
// without the app), until those loads are older.
bool crashGuardAllows() {
    int n = 0;
    if (!guardTurn(contract::kAppGuard, static_cast<long>(time(nullptr)), kCrashGuardWindowS, kCrashGuardMax, &n)) {
        alog(
            "crash guard: the HMI restarted %d times within %ld min, each time shortly after loading this app: app "
            "turned off. It loads again at an iDrive start once these are %ld min old; to load it at once, remove %s "
            "after checking the logs, then restart the unit.",
            n, kCrashGuardWindowS / 60, kCrashGuardWindowS / 60, contract::kAppGuard);
        return false;
    }
    return true;
}

void resetGuardsWhenStable(long initTime) {
    if (!g_stableMarked && initTime && monoSeconds() - initTime >= kStableAfterS) {
        g_stableMarked = true;
        unlink(contract::kAppGuard);  // the HMI has been up 2 min with us: not a crash loop
        unlink(contract::kBootGuard);
        alog("stable for %ld s: crash and boot guards reset", kStableAfterS);
    }
}

// ---- start automatically (with a bootloop guard) --------------------------------------------------------------------
void serviceAutostart(long initTime) {
    if (g_autostartDone || !initTime || monoSeconds() - initTime < kAutostartDelayS) {
        return;
    }
    g_autostartDone = true;
    if (!autostartOn()) {
        return;
    }
    ServiceState d = readServiceState();
    if (d.pid != 0) {
        alog("autostart: service already running (pid %d)", d.pid);
        return;
    }
    if (!bootGuardAllows()) {
        return;
    }
    alog("autostart: launching the service");
    launchService();
}

void forgetBootGuard() {
    unlink(contract::kBootGuard);
}

bool autostartOn() {
    return fileExists(contract::kAutostart);
}

void toggleAutostart() {
    bool on = autostartOn();
    setSwitch("autostart", contract::kAutostart, !on, "1\n");
    alog("menu: start automatically %s -> %s", on ? "on" : "off", autostartOn() ? "on" : "off");
}

bool usbCableOn() {
    return fileExists(contract::kUsb);
}

void toggleUsbCable() {
    bool on = usbCableOn();
    setSwitch("USB cable", contract::kUsb, !on, "");  // empty: every USB port, as `touch` leaves it
    alog("menu: USB cable %s -> %s", on ? "on" : "off", usbCableOn() ? "on" : "off");
}

bool keepLogsOn() {
    return fileExists(contract::kMaintenance);
}

void toggleKeepLogs() {
    const bool on = keepLogsOn();
    if (on) {
        alog("menu: keep logs off: the app's log stops here");  // the last line, written while still on
    }
    setSwitch("keep logs", contract::kMaintenance, !on, "");
    if (!on) {
        alog("menu: keep logs on: the service's log starts with its next start");
    }
}

}  // namespace app
}  // namespace evo
