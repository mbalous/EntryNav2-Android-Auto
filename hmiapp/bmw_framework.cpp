// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The app's own bmw::Runnable and bmw::PluginApp vtable, and small wrappers around BMW's handlers (bmw_framework.hpp).
#include "bmw_framework.hpp"

#include <cxxabi.h>
#include <dlfcn.h>
#include <string.h>

#include <new>

#include "posted_task.hpp"
#include "vtable_check.hpp"

namespace evo {
namespace app {

namespace {

enum { kSlotOnError = 21, kSlotOnInit = 22, kSlotOnFinalize = 23, kVtblSlots = 34 };

void* g_vtbl[2 + kVtblSlots];  // the app's copy of bmw::PluginApp's primary vtable, with its offset and typeinfo

// The runnable's destructors (vtable slots 2 and 3): nothing to free. They return this, as ARM's C++ ABI has every
// destructor do.
void* runnableDestructor(void* self) {
    return self;
}

}  // namespace

void makeRunnable(AppRunnable& r, void (*run)(void*)) {
    void** vtbl = r.vtbl;
    void** base = reinterpret_cast<void**>(dlsym(RTLD_DEFAULT, "_ZTVN3bmw8RunnableE"));
    if (base) {
        memcpy(vtbl, base, 5 * sizeof(void*));
    } else {
        vtbl[0] = nullptr;
        vtbl[1] = nullptr;
    }
    vtbl[2] = reinterpret_cast<void*>(&runnableDestructor);
    vtbl[3] = reinterpret_cast<void*>(&runnableDestructor);
    vtbl[4] = reinterpret_cast<void*>(&runPostedTask<AppRunnable>);
    r.run = run;
    r.vptr = vtbl + 2;
}

void postRunnable(bmw::PluginApp* app, AppRunnable& r, unsigned delayMs) {
    app->post(reinterpret_cast<bmw::Runnable*>(&r), delayMs);
}

void addAction(bmw::PluginApp* app, unsigned actionId, ActionStub stub) {
    bmw::ActionDelegate d;
    d.object = app;
    d.stub = stub;
    app->addHandler(actionId, d);
}

void addStateHandler(bmw::PluginApp* app, unsigned state, unsigned kind, ActionStub stub) {
    bmw::ActionDelegate d;
    d.object = app;
    d.stub = stub;
    app->addHandler(state, kind, d);
}

void addCdsHandler(bmw::PluginApp* app, const char* property, CdsStub stub) {
    bmw::BasicString<char> name(property);
    bmw::CdsDelegate d;
    d.object = app;
    d.stub = stub;
    app->cdsAddPropertyChangedEventHandler(name, 0, d);
}

bool parcelBool(const bmw::Parcel& value) {
    bool b = false;
    value.readBool(b);
    return b;
}

void narrow(const bmw::BasicString<unsigned short>& value, char* text, size_t cap) {
    if (!text || cap == 0) {
        return;
    }
    size_t n = 0;
    const unsigned short* p = value.raw();
    while (p && p[n] && n < cap - 1) {
        text[n] = (p[n] < 0x80) ? static_cast<char>(p[n]) : '?';
        ++n;
    }
    text[n] = 0;
}

bmw::PluginApp* newPluginApp(bmw::IDispatcher& dispatcher, const char* name, const bmw::Bundle& params,
                             const bmw::Bundle& config, const PluginAppHooks& hooks, void** bmwVtable) {
    void* mem = ::operator new(bmw::kPluginAppStorage, std::nothrow);
    if (!mem) {
        return nullptr;
    }
    memset(mem, 0, bmw::kPluginAppStorage);
    bmw::PluginApp* app = nullptr;
    try {
        app = new (mem) bmw::PluginApp(dispatcher, bmw::BasicString<char>(name ? name : "EntryAad"), 1, 0,
                                       bmw::RHMI_INTERFACE_VERSION_1, params, config);
    } catch (abi::__forced_unwind&) {
        throw;  // a thread being cancelled
    } catch (...) {
        ::operator delete(mem, std::nothrow);
        return nullptr;
    }
    // Own copy of bmw::PluginApp's primary vtable with onInit / onError / onFinalize replaced (no BMW headers).
    void** base = *reinterpret_cast<void***>(app);
    *bmwVtable = base;
    // Only where BMW's onError, onInit and onFinalize are where this layout puts them: on a libAppFramework where they
    // moved, no app rather than a wrong function called inside HmiMain3D (the names tests/test_hmiapp_load.cpp checks).
    const VtableSlot expected[] = {
        {kSlotOnError, dlsym(RTLD_DEFAULT, "_ZN3bmw9PluginApp7onErrorEjRKNS_11BasicStringIcEE")},
        {kSlotOnInit, dlsym(RTLD_DEFAULT, "_ZN3bmw9PluginApp6onInitEv")},
        {kSlotOnFinalize, dlsym(RTLD_DEFAULT, "_ZN3bmw9PluginApp10onFinalizeEv")},
    };
    if (!vtableSlotsHold(base, expected, 3)) {
        deletePluginApp(app);
        return nullptr;
    }
    memcpy(g_vtbl, base - 2, sizeof(g_vtbl));
    g_vtbl[2 + kSlotOnInit] = reinterpret_cast<void*>(hooks.onInit);
    g_vtbl[2 + kSlotOnError] = reinterpret_cast<void*>(hooks.onError);
    g_vtbl[2 + kSlotOnFinalize] = reinterpret_cast<void*>(hooks.onFinalize);
    *reinterpret_cast<void***>(app) = g_vtbl + 2;
    return app;
}

void deletePluginApp(void* app) {
    if (!app) {
        return;
    }
    void** vt = *reinterpret_cast<void***>(app);
    reinterpret_cast<void (*)(void*)>(vt[1])(app);  // deleting destructor
}

}  // namespace app
}  // namespace evo
