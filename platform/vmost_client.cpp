// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's vMOST client library loaded at run time, and the message layout (see vmost_client.hpp).
#include "vmost_client.hpp"

#include <dlfcn.h>
#include <sys/stat.h>

#include <cstring>

#include "evo_log.hpp"

namespace evo {

namespace {
std::string g_testSocket, g_testLibrary;  // empty on the car

bool isSocket(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

bool daemonPresent() {
    if (!g_testSocket.empty()) {
        return isSocket(g_testSocket.c_str());
    }
    return isSocket("/var/run/vmost_sockstream") || isSocket("/tmp/vmost_sockstream");
}

// dlsym gives an object pointer; a function pointer of the same size is copied from it.
template <typename Fn>
bool resolve(void* lib, const char* name, Fn& fn) {
    static_assert(sizeof(Fn) == sizeof(void*), "function and object pointers of one size");
    void* p = dlsym(lib, name);
    memcpy(&fn, &p, sizeof(p));
    return p != nullptr;
}
}  // namespace

void setVmostForTest(const char* socketPath, const char* library) {
    g_testSocket = socketPath ? socketPath : "";
    g_testLibrary = library ? library : "";
}

bool parseVmostMsg(const void* msg, VmostMsgView& out) {
    if (!msg) {
        return false;
    }
    const uint8_t* b = static_cast<const uint8_t*>(msg);
    memcpy(&out.addr, b + 0, 2);
    out.fblock = b[2];
    out.inst = b[3];
    memcpy(&out.fkt, b + 4, 2);
    memcpy(&out.op, b + 8, 4);
    memcpy(&out.len, b + 12, 2);
    memcpy(&out.data, b + 16, sizeof(out.data));
    if (out.len > 4096 || (out.len && !out.data) || out.op > 0xF) {
        return false;
    }
    return true;
}

void buildVmostGet(VmostMsgOut& m, uint8_t fblock, uint8_t inst, uint16_t fkt, const uint8_t* data, uint16_t len) {
    memset(&m, 0, sizeof(m));
    m.addr = 0xFFFF;
    m.fblock = fblock;
    m.inst = inst;
    m.fkt = fkt;
    m.op = kMostOpGet;
    m.len = len;
    m.data = data;
    m.channel = 0;
}

VmostClient::VmostClient()
    : connect_(nullptr),
      shadowRegister_(nullptr),
      shadowDeregister_(nullptr),
      fileDescriptorGet_(nullptr),
      processMessages_(nullptr),
      disconnect_(nullptr),
      send_(nullptr),
      libraryConnected_(nullptr),
      open_(false),
      skipNoted_(false),
      lib_(nullptr) {}

VmostClient::Load VmostClient::load() {
    if (!daemonPresent()) {
        return kNoDaemon;
    }
    lib_ = dlopen(!g_testLibrary.empty() ? g_testLibrary.c_str() : kVmostLibrary, RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
        const char* why = dlerror();
        error_ = why ? why : "";
        return kNoLibrary;
    }
    // In the order of kVmostFunctions; every one is looked up before the result is checked.
    bool complete = resolve(lib_, kVmostFunctions[0], connect_);
    complete &= resolve(lib_, kVmostFunctions[1], shadowRegister_);
    complete &= resolve(lib_, kVmostFunctions[2], shadowDeregister_);
    complete &= resolve(lib_, kVmostFunctions[3], fileDescriptorGet_);
    complete &= resolve(lib_, kVmostFunctions[4], processMessages_);
    complete &= resolve(lib_, kVmostFunctions[5], disconnect_);
    complete &= resolve(lib_, kVmostFunctions[6], send_);
    if (!complete) {
        unload();
        return kNoSymbols;
    }
    // A data symbol: the library's own state, read only. Without it, linked() follows our calls alone.
    libraryConnected_ = static_cast<const volatile int*>(dlsym(lib_, kVmostConnectedSymbol));
    open_ = false;
    skipNoted_ = false;
    return kLoaded;
}

void VmostClient::unload() {
    if (lib_) {
        dlclose(lib_);
        lib_ = nullptr;
    }
    connect_ = nullptr;
    shadowRegister_ = nullptr;
    shadowDeregister_ = nullptr;
    fileDescriptorGet_ = nullptr;
    processMessages_ = nullptr;
    disconnect_ = nullptr;
    send_ = nullptr;
    libraryConnected_ = nullptr;
    open_ = false;
}

bool VmostClient::linked() const {
    return open_ && (!libraryConnected_ || *libraryConnected_ != 0);
}

bool VmostClient::usable(const char* what) {
    if (linked()) {
        return true;
    }
    if (!skipNoted_) {
        skipNoted_ = true;
        if (open_) {
            ELOG("vMOST",
                 "the library closed its connection itself after a failed read or write: %s not called, nor any "
                 "later (de)registration or disconnection",
                 what);
        } else {
            ELOG("vMOST", "no connection: %s not called", what);
        }
    }
    return false;
}

int VmostClient::connect(VmostCallback cb, void* user, int flags) {
    const int rc = connect_(cb, user, flags);
    open_ = rc == 0;
    skipNoted_ = false;
    return rc;
}

int VmostClient::shadowRegister(uint8_t fblock, uint8_t inst) {
    return usable("vmostFblockShadowRegister") ? shadowRegister_(fblock, inst) : static_cast<int>(kNotConnected);
}

int VmostClient::shadowDeregister(uint8_t fblock, uint8_t inst) {
    return usable("vmostFblockShadowDeregister") ? shadowDeregister_(fblock, inst) : static_cast<int>(kNotConnected);
}

int VmostClient::fileDescriptorGet(int* fd) {
    return fileDescriptorGet_(fd);
}

int VmostClient::processMessages() {
    return processMessages_();
}

int VmostClient::send(void* msg) {
    return send_(msg);
}

int VmostClient::disconnect() {
    if (!open_) {
        return 0;  // never connected, or already let go
    }
    const bool call = usable("vmostDisconnect");
    open_ = false;
    return call ? disconnect_() : 0;
}

}  // namespace evo
