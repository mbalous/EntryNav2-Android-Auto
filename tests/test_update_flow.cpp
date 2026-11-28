// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The menu's "Update from USB stick" button (hmiapp/update_flow.hpp): what each press and each outcome does.
#include <stdio.h>
#include <stdlib.h>

#include "../hmiapp/update_flow.hpp"

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

namespace {

using evo::UpdateResult;
using evo::app::PendingRestart;
using evo::app::UpdateFlow;
namespace app = evo::app;

int code(UpdateResult r) {
    return static_cast<int>(r);
}

UpdateFlow::Car car(bool androidAuto, bool mayRestart = true) {
    UpdateFlow::Car c;
    c.androidAuto = androidAuto;
    c.mayRestart = mayRestart;
    return c;
}
const UpdateFlow::Car kStopped = car(false), kRunning = car(true);

// A check that found what is given, from a fresh button at time 100.
UpdateFlow checked(UpdateResult found) {
    UpdateFlow f;
    CHECK(f.press(100, kStopped) == UpdateFlow::kStartCheck && f.checking() && f.text() == app::kTextUpdateChecking);
    f.ended(code(found), 101);
    return f;
}

void firstPress() {
    UpdateFlow f;
    CHECK(f.text() == app::kTextUpdate && !f.holdsAndroidAuto() && !f.needsAndroidAutoState());
    CHECK(f.press(10, kRunning) == UpdateFlow::kStartCheck);  // reading the stick needs nothing stopped
    CHECK(!f.holdsAndroidAuto() && !f.needsAndroidAutoState());
    CHECK(f.press(11, kRunning) == UpdateFlow::kNothing);                  // pressed again while it reads the stick
    CHECK(f.poll(200, kRunning) == UpdateFlow::kNothing && f.checking());  // a check ends when its program says so
    UpdateFlow notStarted;
    notStarted.press(10, kStopped);
    notStarted.startFailed(10);
    CHECK(notStarted.text() == app::kTextUpdateFailed && !notStarted.takeRestart());
    printf("PASS first press\n");
}

void readyToInstall() {
    const struct {
        UpdateResult found;
        unsigned text;
    } ready[] = {{UpdateResult::kNewer, app::kTextUpdateInstall},
                 {UpdateResult::kOlder, app::kTextUpdateOlder},
                 {UpdateResult::kRepair, app::kTextUpdateReinstall},
                 {UpdateResult::kPending, app::kTextUpdateFinish}};
    for (size_t i = 0; i < sizeof(ready) / sizeof(ready[0]); ++i) {
        UpdateFlow f = checked(ready[i].found);
        CHECK(f.text() == ready[i].text && !f.checking() && !f.holdsAndroidAuto());
        // Android Auto stopped: the second press installs at once.
        CHECK(f.press(110, kStopped) == UpdateFlow::kStartInstall && f.installing() && f.holdsAndroidAuto());
        CHECK(f.text() == app::kTextUpdateInstalling);
        CHECK(f.press(111, kStopped) == UpdateFlow::kNothing);
        f.ended(code(UpdateResult::kInstalled), 120);
        CHECK(f.text() == app::kTextUpdateRestarting && f.holdsAndroidAuto() && !f.takeRestart());
        CHECK(f.press(121, kStopped) == UpdateFlow::kNothing);
        CHECK(f.poll(120 + app::kUpdateRestartS - 1, kStopped) == UpdateFlow::kNothing);
        CHECK(f.text() == app::kTextUpdateRestarting);
        f.poll(120 + app::kUpdateRestartS, kStopped);  // no restart came: the driver restarts the iDrive
        CHECK(f.text() == app::kTextUpdateRestartByHand && f.holdsAndroidAuto());
        f.poll(10000, kStopped);
        CHECK(f.text() == app::kTextUpdateRestartByHand);
    }
    // The second press too late: the button reads "Update from USB stick" again, the next press checks again.
    UpdateFlow late = checked(UpdateResult::kNewer);
    late.poll(101 + app::kUpdateReadyS - 1, kStopped);
    CHECK(late.text() == app::kTextUpdateInstall);
    late.poll(101 + app::kUpdateReadyS, kStopped);
    CHECK(late.text() == app::kTextUpdate);
    CHECK(late.press(200, kStopped) == UpdateFlow::kStartCheck);
    printf("PASS ready to install\n");
}

void notDuringACallOrInReverse() {
    UpdateFlow f = checked(UpdateResult::kNewer);
    CHECK(f.press(110, car(false, false)) == UpdateFlow::kNothing);
    CHECK(f.text() == app::kTextUpdateNotNow && !f.holdsAndroidAuto() && !f.takeRestart());
    CHECK(f.press(112, kStopped) == UpdateFlow::kStartCheck);  // checked again once the call is over
    // A call or reverse gear while Android Auto stops for the install: given up, Android Auto started again.
    UpdateFlow stopping = checked(UpdateResult::kNewer);
    CHECK(stopping.press(110, kRunning) == UpdateFlow::kStopAndroidAuto);
    CHECK(stopping.poll(112, car(false, false)) == UpdateFlow::kNothing);
    CHECK(stopping.text() == app::kTextUpdateNotNow && !stopping.holdsAndroidAuto());
    CHECK(stopping.takeRestart() == PendingRestart::kStart);  // already stopped: at once
    CHECK(!stopping.takeRestart());                           // once
    printf("PASS not during a call or in reverse\n");
}

void androidAutoStoppedFirst() {
    UpdateFlow f = checked(UpdateResult::kNewer);
    CHECK(f.press(110, kRunning) == UpdateFlow::kStopAndroidAuto && f.text() == app::kTextUpdateStopping);
    CHECK(f.holdsAndroidAuto() && f.needsAndroidAutoState() &&
          !f.restartAsked());                                   // nothing may start it again meanwhile
    CHECK(f.press(111, kRunning) == UpdateFlow::kNothing);      // pressed again: one stop is enough
    CHECK(f.poll(115, kRunning) == UpdateFlow::kNothing);       // still stopping
    CHECK(f.poll(116, kStopped) == UpdateFlow::kStartInstall);  // stopped, Bluetooth given back: the install starts
    CHECK(f.installing() && f.holdsAndroidAuto());
    // Installed: Android Auto stays stopped, the unit restarts.
    f.ended(code(UpdateResult::kInstalled), 120);
    CHECK(!f.takeRestart());
    // Refused before any change: Android Auto, stopped for it, starts again at the status poll that reads the outcome
    // (the app's poll hands it the end of the install, then polls).
    const int refusals[] = {code(UpdateResult::kFailed),
                            code(UpdateResult::kIncompatible),
                            code(UpdateResult::kInvalid),
                            code(UpdateResult::kNoStick),
                            code(UpdateResult::kBusy),
                            code(UpdateResult::kUpToDate),
                            1,
                            127};
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); ++i) {
        UpdateFlow r = checked(UpdateResult::kNewer);
        r.press(110, kRunning);
        r.poll(111, kStopped);
        r.ended(refusals[i], 115);
        CHECK(!r.holdsAndroidAuto());
        r.poll(115, kStopped);
        CHECK(r.takeRestart() == PendingRestart::kStart && !r.takeRestart());
    }
    // Part of it may be in place: Android Auto is left stopped, the button says so.
    const int partial[] = {code(UpdateResult::kIncomplete), -2};
    for (size_t i = 0; i < sizeof(partial) / sizeof(partial[0]); ++i) {
        UpdateFlow r = checked(UpdateResult::kNewer);
        r.press(110, kRunning);
        r.poll(111, kStopped);
        r.ended(partial[i], 115);
        CHECK(r.text() == app::kTextUpdateIncomplete && !r.holdsAndroidAuto() && !r.takeRestart());
    }
    // The install could not even start: nothing ran, Android Auto starts again.
    UpdateFlow notStarted = checked(UpdateResult::kNewer);
    notStarted.press(110, kRunning);
    notStarted.poll(111, kStopped);
    notStarted.startFailed(111);
    CHECK(notStarted.text() == app::kTextUpdateFailed);
    notStarted.poll(112, kStopped);
    CHECK(notStarted.takeRestart() == PendingRestart::kStart);
    // Android Auto never stops: given up, nothing installed, starts allowed again.
    UpdateFlow stuck = checked(UpdateResult::kNewer);
    stuck.press(110, kRunning);
    CHECK(stuck.poll(110 + app::kUpdateStopS - 1, kRunning) == UpdateFlow::kNothing && stuck.holdsAndroidAuto());
    CHECK(stuck.poll(110 + app::kUpdateStopS, kRunning) == UpdateFlow::kNothing);
    CHECK(stuck.text() == app::kTextUpdateBusy && !stuck.holdsAndroidAuto());
    printf("PASS Android Auto stopped first\n");
}

// Given up while Android Auto was still stopping (code review of 25/11, M12.3): the start was asked for at once, the
// supervisor still giving the Bluetooth back refused it, and Android Auto stayed stopped without a word. It is started
// once nothing of it is left, the way the USB checkbox's restart waits (pending_restart.hpp).
UpdateFlow givenUpWhileStopping() {
    UpdateFlow f = checked(UpdateResult::kNewer);
    CHECK(f.press(110, kRunning) == UpdateFlow::kStopAndroidAuto);
    CHECK(f.poll(112, car(true, false)) == UpdateFlow::kNothing);  // a call: given up, Android Auto still stopping
    CHECK(f.text() == app::kTextUpdateNotNow && !f.holdsAndroidAuto() && f.needsAndroidAutoState() && f.restartAsked());
    return f;
}
void restartOnceStopped() {
    {  // started again once it has stopped, call or not, once
        UpdateFlow f = givenUpWhileStopping();
        CHECK(!f.takeRestart());
        for (long t = 113; t < 125; ++t) {
            f.poll(t, car(true, false));
            CHECK(!f.takeRestart());  // its supervisor still gives the Bluetooth back
        }
        f.poll(125, car(false, false));
        CHECK(f.takeRestart() == PendingRestart::kStart && !f.takeRestart());
        f.poll(126, kStopped);
        CHECK(!f.takeRestart());
    }
    {  // never gone: left as it is after kRestartGiveUpS, said once, and not started later behind the driver's back
        UpdateFlow f = givenUpWhileStopping();
        f.poll(112 + app::kRestartGiveUpS - 1, kRunning);
        CHECK(!f.takeRestart());
        f.poll(112 + app::kRestartGiveUpS, kRunning);
        CHECK(f.takeRestart() == PendingRestart::kGiveUp && !f.takeRestart());
        f.poll(112 + app::kRestartGiveUpS + 1, kStopped);
        CHECK(!f.takeRestart());
    }
    {  // Start or Stop by hand meanwhile: the driver decides, nothing is started for the update
        UpdateFlow f = givenUpWhileStopping();
        f.poll(113, kRunning);
        f.cancelRestart();
        f.poll(114, kStopped);
        CHECK(!f.takeRestart());
    }
    {  // the update pressed again meanwhile, up to its install: that install needs Android Auto stopped
        UpdateFlow f = givenUpWhileStopping();
        CHECK(f.press(113, kRunning) == UpdateFlow::kStartCheck);  // reading the stick: the restart still waits
        f.ended(code(UpdateResult::kNewer), 114);
        CHECK(f.press(115, kRunning) == UpdateFlow::kStopAndroidAuto);
        CHECK(f.poll(116, kStopped) == UpdateFlow::kStartInstall && !f.takeRestart());
    }
    printf("PASS restarted once Android Auto has stopped\n");
}

void outcomes() {
    const struct {
        int code;
        unsigned text;
    } checks[] = {{code(UpdateResult::kUpToDate), app::kTextUpToDate},
                  {code(UpdateResult::kNoStick), app::kTextNoStick},
                  {code(UpdateResult::kNoPackage), app::kTextNoUpdate},
                  {code(UpdateResult::kInvalid), app::kTextUpdateInvalid},
                  {code(UpdateResult::kIncompatible), app::kTextUpdateIncompatible},
                  {code(UpdateResult::kBusy), app::kTextUpdateBusy},
                  {code(UpdateResult::kNotInstalled), app::kTextUpdateNotOurs},
                  {code(UpdateResult::kFailed), app::kTextUpdateFailed},
                  {code(UpdateResult::kIncomplete), app::kTextUpdateIncomplete},
                  {code(UpdateResult::kInstalled), app::kTextUpdateFailed},  // a check never installs
                  {-2, app::kTextUpdateFailed},                              // it was killed, or crashed
                  {1, app::kTextUpdateFailed},
                  {127, app::kTextUpdateFailed}};
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        UpdateFlow f;
        f.press(100, kStopped);
        f.ended(checks[i].code, 101);
        CHECK(f.text() == checks[i].text && !f.holdsAndroidAuto() && !f.takeRestart());
        f.poll(101 + app::kUpdateResultS - 1, kStopped);
        CHECK(f.text() == checks[i].text);
        f.poll(101 + app::kUpdateResultS, kStopped);
        CHECK(f.text() == app::kTextUpdate);
        f.ended(checks[i].code, 120);  // nothing runs: ignored
        CHECK(f.text() == app::kTextUpdate);
    }
    UpdateFlow shown = checked(UpdateResult::kNoStick);
    CHECK(shown.press(105, kStopped) == UpdateFlow::kStartCheck);  // the stick plugged in meanwhile
    printf("PASS outcomes\n");
}

}  // namespace

int main() {
    firstPress();
    readyToInstall();
    notDuringACallOrInReverse();
    androidAutoStoppedFirst();
    restartOnceStopped();
    outcomes();
    printf("PASS update flow\n");
    return 0;
}
