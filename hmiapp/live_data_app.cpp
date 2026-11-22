// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "bmw_framework.hpp"
#include "live_data.hpp"
#include "mono_clock.hpp"

namespace evo {
namespace app {

namespace {

const char kTemperatures[] = "engine.temperature";
const char kUnits[] = "vehicle.units";
const char kDisableFile[] = "/var/local/data/EntryLiveData.disable";
const unsigned kOpenAction = 1;
const unsigned kOpenEvent = 8;
const unsigned kVisible = 3;
bmw::PluginApp* g_app = nullptr;
AppRunnable g_subscribe;
live_data::Model g_model;
long g_lastSample = -5;

struct Row {
    unsigned model, value, unavailable, gaugeModel, gauge;
    std::wstring shown;
    int shownGauge;

    void render(bmw::PluginApp* app, const live_data::Reading& reading, bool fahrenheit) {
        const std::wstring next = reading.text(fahrenheit);
        const int position = reading.gaugePosition();
        if (!next.empty() && shown != next) {
            app->updateModel(model, next.c_str());
        }
        if (position >= 0 && shownGauge != position) {
            app->updateModel(gaugeModel, position);
        }
        if (shown.empty() != next.empty()) {
            app->setProperty(value, kVisible, bmw::Parcel(!next.empty()));
            app->setProperty(unavailable, kVisible, bmw::Parcel(next.empty()));
            app->setProperty(gauge, kVisible, bmw::Parcel(!next.empty()));
        }
        shown = next;
        shownGauge = position;
    }
};

Row g_oil{4, 12, 13, 30, 32, {}, -1};
Row g_coolant{6, 15, 16, 31, 33, {}, -1};

void render(bmw::PluginApp* app) {
    g_oil.render(app, g_model.oil, g_model.fahrenheit);
    g_coolant.render(app, g_model.coolant, g_model.fahrenheit);
}

bool onTemperatures(void* app, const bmw::BasicString<unsigned short>& value) {
    if (g_app && app == g_app) {
        char text[512];
        narrow(value, text, sizeof(text));
        const long now = monoSeconds();
        if (now - g_lastSample >= 5) {
            g_lastSample = now;
            if (FILE* log = fopen("/tmp/entry-live-data.log", "w")) {
                fprintf(log, "CDS %s: %s\n", kTemperatures, text);
                fclose(log);
            }
        }
        g_model.updateTemperatures(text);
        render(g_app);
    }
    return true;
}

bool onUnits(void* app, const bmw::BasicString<unsigned short>& value) {
    if (g_app && app == g_app) {
        char text[512];
        narrow(value, text, sizeof(text));
        g_model.updateUnits(text);
        render(g_app);
    }
    return true;
}

void subscribe(void*) {
    if (!g_app) {
        return;
    }
    addCdsHandler(g_app, kTemperatures, &onTemperatures);
    addCdsHandler(g_app, kUnits, &onUnits);
    bmw::BasicString<char> temperatures(kTemperatures), units(kUnits);
    g_app->cdsGetPropertyAsync(temperatures, bmw::CdsDelegate{g_app, &onTemperatures});
    g_app->cdsGetPropertyAsync(units, bmw::CdsDelegate{g_app, &onUnits});
}

bool open(void* app, const bmw::Parcel&) {
    static_cast<bmw::PluginApp*>(app)->triggerEvent(kOpenEvent);
    return true;
}

bool init(bmw::PluginApp* app) {
    addAction(app, kOpenAction, &open);
    if (!app->onInit()) {
        return false;
    }
    g_app = app;
    g_model = live_data::Model();
    g_lastSample = -5;
    g_oil.shown.clear();
    g_coolant.shown.clear();
    g_oil.shownGauge = -1;
    g_coolant.shownGauge = -1;
    makeRunnable(g_subscribe, &subscribe);
    postRunnable(app, g_subscribe, 1500);
    return true;
}

void finalize(bmw::PluginApp* app) {
    g_app = nullptr;
    app->onFinalize();
}

void onError(bmw::PluginApp*, unsigned code, const bmw::BasicString<char>& text) {
    fprintf(stderr, "[EntryLiveData] RHMI error %u: %s\n", code, text.raw() ? text.raw() : "(null)");
}

bmw::PluginApp* create(bmw::IDispatcher& dispatcher, const char*, const bmw::Bundle& params,
                       const bmw::Bundle& config) {
    if (access(kDisableFile, F_OK) == 0) {
        fprintf(stderr, "[EntryLiveData] disabled by %s\n", kDisableFile);
        return nullptr;
    }
    const PluginAppHooks hooks = {&init, &onError, &finalize};
    void* vtable = nullptr;
    return newPluginApp(dispatcher, "EntryLiveData", params, config, hooks, &vtable);
}

}  // namespace
}  // namespace app
}  // namespace evo

extern "C" {
struct LiveDataPluginAppInfo {
    uint16_t versionMajor;
    uint16_t versionMinor;
    const char* name;
    const char* vendor;
    uint32_t rhmiInterfaceVersion;
    bmw::PluginApp* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
__attribute__((visibility("default"))) LiveDataPluginAppInfo pluginAppInfo = {
    1, 0, "Live Data", "EntryLiveData", 1, &evo::app::create, &evo::app::deletePluginApp};
}
