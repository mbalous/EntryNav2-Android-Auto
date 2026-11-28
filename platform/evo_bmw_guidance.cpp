// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Asks the BMW navigation over D-Bus whether it is guiding (one read-only method call).
#include "evo_bmw_guidance.hpp"

#include <dbus/dbus.h>
#include <pthread.h>

#include <cstdlib>
#include <string>

namespace evo {
namespace kombi {

namespace {
// Names, path and the "b" reply as observed on the car's session bus (traffic and introspection).
const char* const kDest = "com.bmw.navigation.guiding.Guiding.MainInstance";
const char* const kPath = "/com/bmw/navigation/guiding/Guiding/MainInstance";
const char* const kIface = "com.bmw.navigation.guiding.Guiding";
const char* const kMember = "getGuidanceActiveAttribute";
const char* const kSessionBus = "unix:path=/tmp/session_bus_socket";
const int kTimeoutMs = 1500;
std::string g_bus;  // empty on the car: its session bus

pthread_once_t g_threadsOnce = PTHREAD_ONCE_INIT;
void initThreadsOnce() {
    dbus_threads_init_default();
}

DBusConnection* g_conn = nullptr;

void closeBus() {
    if (!g_conn) {
        return;
    }
    dbus_connection_close(g_conn);
    dbus_connection_unref(g_conn);
    g_conn = nullptr;
}

void drainQueued() {
    if (!g_conn) {
        return;
    }
    while (DBusMessage* queued = dbus_connection_pop_message(g_conn)) {
        dbus_message_unref(queued);
    }
}

bool openBus() {
    if (g_conn && dbus_connection_get_is_connected(g_conn)) {
        return true;
    }
    closeBus();
    DBusError e;
    dbus_error_init(&e);
    g_conn = dbus_connection_open_private(g_bus.empty() ? kSessionBus : g_bus.c_str(), &e);
    if (g_conn) {
        // Set before dbus_bus_register so an immediate disconnect never terminates the daemon.
        dbus_connection_set_exit_on_disconnect(g_conn, FALSE);
        if (!dbus_bus_register(g_conn, &e)) {
            closeBus();
        }
    }
    dbus_error_free(&e);
    if (!g_conn) {
        return false;
    }
    drainQueued();
    return true;
}
}  // namespace

void initBmwGuidanceThreading() {
    pthread_once(&g_threadsOnce, initThreadsOnce);
}

void closeBmwGuidance() {
    closeBus();
}

void setGuidanceBusForTest(const char* address) {
    closeBus();
    g_bus = address ? address : "";
}

int probeBmwGuidance() {
    initBmwGuidanceThreading();
    if (!openBus()) {
        return -1;
    }
    DBusMessage* m = dbus_message_new_method_call(kDest, kPath, kIface, kMember);
    if (!m) {
        return -1;
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(g_conn, m, kTimeoutMs, &e);
    dbus_message_unref(m);
    dbus_error_free(&e);
    drainQueued();
    if (!r) {
        return -1;
    }
    int result = -1;
    DBusMessageIter it;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t v = FALSE;
        dbus_message_iter_get_basic(&it, &v);
        result = v ? 1 : 0;
    }
    dbus_message_unref(r);
    return result;
}

}  // namespace kombi
}  // namespace evo
