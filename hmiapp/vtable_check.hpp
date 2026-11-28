// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Before the app replaces slots of bmw::PluginApp's vtable, each must hold BMW's own function there, the one its
// exported symbol names. On another libAppFramework where a virtual function moved, the replaced slot would be called
// for something else at the first event and bring HmiMain3D down; the app is then not created at all (iDrive starts
// without it, as when the app is disabled). The car's library passes: tests/test_hmiapp_load.cpp checks the same slots
// under QEMU; tests/test_vtable_check.cpp checks this rule.
#ifndef ENTRY_AAD_HMIAPP_VTABLE_CHECK_HPP
#define ENTRY_AAD_HMIAPP_VTABLE_CHECK_HPP

namespace evo {
namespace app {

struct VtableSlot {
    int index;       // in the vtable, from its address point
    void* function;  // the exported symbol's address (dlsym), nullptr when the library has none
};

// True when every slot holds its function; false at the first one that does not, or whose symbol is missing.
inline bool vtableSlotsHold(void* const* vtable, const VtableSlot* slots, int count) {
    if (!vtable) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (!slots[i].function || vtable[slots[i].index] != slots[i].function) {
            return false;
        }
    }
    return true;
}

}  // namespace app
}  // namespace evo

#endif
