// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The unit's restart after an update from a USB stick, and aa-supervisor --reboot: a clean restart asked of the
// unit's own processor (Cortex-M4) as a BMW diagnostic tool asks it, else today's restart.
//
// Today's restart is /sbin/reboot, a link to systemctl, without -f: systemd stops the services, so the BMW
// persistence is written, but the iDrive stays black about 3 minutes while systemd waits for the BMW services (after
// an update by USB stick: "update install: ended" at 04:51:51, the app loaded again at 04:55:02). It stays the way
// out of every case below: at worst, the restart is exactly today's. Never reboot -f (the services would not be
// stopped, the persistence not written), never a remount of / (refused after an update anyway: removed files stay
// open).
//
// Read in the car's system (a copy in WSL, ~/entrynav2/rootfs, software 202111011321):
//   - DiagProxyIC (usr/local/bin/DiagProxyIC, 0xc304) forwards the UDS request of a tester on the ENET cable to the
//     Cortex-M4 over vMOST: address 0x0100, FBlock 0xFD, instance 0x04, function 0xC00, operation 0 (Set), data =
//     the tester and unit addresses then the request. 0xF4 0x63 (tester 0xF4, the unit 0x63) is entry 4 of
//     EthDiagMM_TA2TAConnMap (usr/local/lib/libEthDiagMM.so.0, 0x110ac). Ours: ECUReset hardReset, F4 63 11 01.
//   - Its vMOST callback (0xaaf8, then 0xad90-0xae44) hands FD.04 and FD.41 function 0xC00 to 0xa9e8, looking at
//     neither the operation nor the address; 0xa9e8 reads data[1] == 0x63 and data[2] == 0x51 (the positive answer
//     to ECUReset) as "ECUReset", and closes the tester's connection 100 ms later.
//   - SystemManager.service gives SystemManager 5 s for the applications and 20 s for the persistence when the unit
//     shuts down (SYSM_T_USER_APP_SHUTDOWN, SYSM_T_WRITE_PERSISTENCE).
// Not proven: what the M4 does after a positive answer (a clean shutdown by SystemManager, then the whole unit reset,
// is what is expected), which operation and address its answer carries, whether vmostd hands that answer to a second
// client like us, and why it would refuse (UDS 0x22 says "conditions not correct": whether that means a moving car
// is the M4's business, not read anywhere). Nothing of this has run in a car yet.
//
// So, after the request (restartHeadUnit):
//   - accepted (51 01), or "response pending" (7F 11 78) without a final answer: the clean shutdown is left to run;
//     today's restart only if the unit still runs kCleanRestartWaitMs later;
//   - no answer within kEcuResetAnswerMs: ours may have been lost on the way while the M4 does restart; today's
//     restart kSilentM4WaitMs later;
//   - refused (7F 11 <code>), a vMOST error answer (operation 0x9 or 0xF), vmostd or its library missing, or the
//     request not sent: today's restart at once.
#ifndef ENTRY_AAD_PLATFORM_EVO_REBOOT_HPP
#define ENTRY_AAD_PLATFORM_EVO_REBOOT_HPP

#include <stddef.h>
#include <stdint.h>

#include "vmost_client.hpp"

namespace evo {

// Where DiagProxyIC sends a tester's request (above), and where it reads the answers: instance 0x04 or 0x41.
const uint16_t kM4Address = 0x0100;
const uint8_t kDiagFblock = 0xFD, kDiagInstance = 0x04, kDiagAnswerInstance = 0x41;
const uint16_t kDiagFunction = 0x0C00;
const uint8_t kDiagTester = 0xF4, kDiagUnit = 0x63;
// UDS (ISO 14229): ECUReset with its hardReset sub-function, its positive answer, a refusal, "response pending".
const uint8_t kUdsEcuReset = 0x11, kUdsHardReset = 0x01, kUdsEcuResetAccepted = 0x51, kUdsRefusal = 0x7F;
const uint8_t kUdsResponsePending = 0x78;
// vMOST's error answers to a request: ErrorAck and Error.
const uint32_t kDiagMostErrorAck = 0x9, kDiagMostError = 0xF;

// UDS asks a unit to answer within 50 ms (P2), or to say "response pending" first: 2 s is plenty for the first answer.
const unsigned kEcuResetAnswerMs = 2000;
// After a "response pending", UDS gives the final answer 5 s (P2*), counted from the first one: a refusal that comes
// then is still seen. Later ones do not lengthen the wait.
const unsigned kEcuResetPendingMs = 5000;
// Accepted: SystemManager's own budgets (5 s for the applications, 20 s for the persistence, then the unmounting)
// four times over, so that a slow persistence write is never cut short by today's restart.
const unsigned kCleanRestartWaitMs = 120000;
// No answer: SystemManager's 25 s and its unmounting, in case the M4 did take the request; then today's restart.
const unsigned kSilentM4WaitMs = 30000;

// The request: F4 63 11 01.
size_t buildEcuHardReset(uint8_t out[4]);

enum class EcuResetAnswer {
    kNone,        // not an answer to our request
    kPending,     // 7F 11 78: response pending
    kAccepted,    // 51 01
    kRefused,     // 7F 11 <code>, code != 78
    kVmostError,  // operation 0x9 or 0xF from the M4 on the diagnostic function: the transport refused the request
};

// What one vMOST message says about our request. Taken only from the M4 (kM4Address), on FD.04 or FD.41 function
// 0xC00. A UDS answer is told by its bytes whatever its operation (DiagProxyIC reads it so), and must name our tester
// and the unit (F4 63); a copy of a request is never one. code: the UDS refusal code, or the first byte of a vMOST
// error (-1 without one); untouched for the other answers.
EcuResetAnswer parseEcuResetAnswer(const VmostMsgView& msg, int* code = nullptr);

// The answers to one request, as vmostd hands them. The first final answer (accepted, refused, vMOST error) settles
// it; "response pending" before it is noted with its time.
class EcuResetWatch {
public:
    EcuResetWatch();
    // One message from vmostd at nowMs (a monotonic clock). True when it answered our request.
    bool take(const VmostMsgView& msg, uint32_t nowMs);
    bool settled() const;
    // The final answer, else kPending after a "response pending", else kNone.
    EcuResetAnswer answer() const { return answer_; }
    int code() const { return code_; }        // the refusal code or the vMOST error's first byte, -1 without one
    uint32_t errorOp() const { return op_; }  // the vMOST error's operation
    bool pendingSeen() const { return pendingSeen_; }
    uint32_t pendingAtMs() const { return pendingAtMs_; }
    unsigned answers() const { return answers_; }

private:
    EcuResetAnswer answer_;
    int code_;
    uint32_t op_;
    bool pendingSeen_;
    uint32_t pendingAtMs_;
    unsigned answers_;
};

// What the restart needs from the car, so that the tests stand in for it and can never restart the PC
// (update_car.cpp holds the car's; tests/test_evo_reboot.cpp links none of it).
class RestartHost {
public:
    virtual ~RestartHost() {}
    virtual uint32_t nowMs() = 0;  // a monotonic clock
    virtual void sleepMs(unsigned ms) = 0;
    // Opens a short vMOST session and hands the request to vmostd. False when it did not: vmostd or its library
    // missing, connection or sending refused (the host logs why).
    virtual bool sendEcuReset() = 0;
    // Gives watch what vmostd hands over, for ms at most, and returns as soon as watch is settled. False when the
    // session broke.
    virtual bool pump(unsigned ms, EcuResetWatch& watch) = 0;
    virtual void endSession() = 0;           // after sendEcuReset, whatever it returned
    virtual void log(const char* line) = 0;  // one [Reboot] line
    // Flushes the log to the flash and closes it for good: nothing of /var/local stays open while the unit shuts
    // down. Nothing is logged after.
    virtual void closeLog() = 0;
    // Today's restart: sync, then /sbin/reboot without -f. Returns only if it could not start.
    virtual void legacyReboot() = 0;
};

// The way restartHeadUnit took. The host's legacyReboot ends it each time; on the car it returns only if that failed.
enum class RestartWay {
    kLegacyNow,           // no clean restart to wait for: today's restart at once
    kLegacyAfterSilence,  // no answer: today's restart kSilentM4WaitMs later
    kLegacyAfterClean,    // accepted or pending: today's restart only if the unit still runs kCleanRestartWaitMs later
};

// Waits delayMs (the iDrive menu says "restarting" meanwhile), asks the M4 for its clean restart, and then (top of
// this file) leaves it to the M4 or goes for today's restart.
RestartWay restartHeadUnit(RestartHost& host, unsigned delayMs);

// aa-supervisor --reboot, by hand over SSH (docs/MAINTENANCE.md), in update_car.cpp (the car only). Refused while an
// update from a USB stick runs (its lock is taken) or was cut short (its journal, update-pending); otherwise
// restartHeadUnit at once, holding the update's lock, its lines on stdout. 1: refused; 127: no restart could start.
int rebootByHand();

// The car's vMOST session for the request (vmost_client.hpp). Used by the car's RestartHost; the tests run it on a
// stand-in library (tests/fake_vmostclient.c). Logs what vmostd answers ([Reboot]).
class EcuResetSession {
public:
    EcuResetSession();
    ~EcuResetSession();
    // vmostd, its library, a connection, the copies of FD.04 and FD.41 (the request still goes without them), then
    // the request handed to vmostd.
    bool send();
    // False once the session broke: vmostd hung up, or the library dropped the connection after a failed read.
    bool pump(unsigned ms, EcuResetWatch& watch);
    // Gives the copies back and disconnects; after a broken session, only disconnects, and only if the library has
    // not done it itself (vmost_client.hpp).
    void end();
    bool registered(uint8_t instance) const;

private:
    static void onMessage(void* msg, void* user);
    VmostClient vmost_;
    bool loaded_, connected_, registered04_, registered41_;
    // vmostd hung up or the library dropped the connection (a failed read or write): end() only lets it go, once.
    bool linkLost_;
    int fd_;
    EcuResetWatch* watch_;
};

// The [Reboot] lines of a restart. With a marker and a file: into that file (O_NOFOLLOW, ours, regular), as stdout
// and stderr, only while the marker exists (the maintenance profile), opened at the first line; else on stdout as it
// is. close() flushes, fsyncs and puts /dev/null in its place: the file is never opened again.
class RebootLog {
public:
    RebootLog(const char* maintenanceMarker, const char* logFile);
    void line(const char* text);
    void close();

private:
    const char* marker_;
    const char* file_;
    bool tried_, redirected_, closed_;
};

}  // namespace evo

#endif
