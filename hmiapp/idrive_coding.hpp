// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What the service needs of the car's coding, read inside iDrive and written for it: which key each controller
// button is (platform/controller_layout.hpp), the brand (platform/car_brand.hpp), the driver's side
// (platform/driver_side.hpp) and the series (platform/car_series.hpp).
//
// iDrive keeps its coding behind a variant server: hmiplugin::get<IHmiVariantServer>(), then isVariantSet(id)
// (vtable slot 3) and getRole() (slot 21), as CHmiCCSController::pushbuttonToKeycode does in HmiMain3D. The app runs
// inside HmiMain3D, so it asks the same server; both functions are found with dlsym, nothing is linked. If one is
// missing or the server is not there yet, nothing is written: the service keeps its G-style keys and a BMW.
// Called from onInit and iDrive's handlers. The lookups go through iDrive's per-thread instance cache: inside one of
// the app's posted tasks they are refused, logged once, and nothing changes (posted_task.hpp).
#ifndef ENTRY_AAD_HMIAPP_IDRIVE_CODING_HPP
#define ENTRY_AAD_HMIAPP_IDRIVE_CODING_HPP

namespace evo {
namespace app {

// Reads the coding and writes contract::kControllerFile, contract::kBrandFile, contract::kDriverSideFile and
// contract::kSeriesFile. False when one was not written (logged); a file not written is removed, except when refused
// inside a posted task: the files stay as they were.
bool writeIdriveCoding();

// iDrive's own driving mode popup, read for the app itself (popup_peek.hpp, driveModePopupExpected).
//  - setting: the driving mode entry of iDrive's popup settings (per driver profile), as its driving mode device
//    IDevCarFES::isPopupAvailable() gives it: on and the device coded. Reached as HmiMain3D's CCarFesModeService::init
//    does; the function is called only when the device's vtable holds this very one.
//  - forced: coding variant 0x519d, with which CDevCarFES35 shows the popup even with the setting off.
// Each 1, 0, or -1 when it could not be read (both -1 inside a posted task). Logged. Called like writeIdriveCoding().
struct DriveModePopup {
    int setting, forced;
};
DriveModePopup readDriveModePopup();

}  // namespace app
}  // namespace evo

#endif
