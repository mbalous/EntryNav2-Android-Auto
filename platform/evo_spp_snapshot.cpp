// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Reads the SPP slots of the Alps Bluetooth process through /proc/<pid>/mem, read-only.
#define _LARGEFILE64_SOURCE
#include "evo_spp_snapshot.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "evo_elf.hpp"

namespace evo {
namespace {
// The tables come from the library's own exports, so another build of it is found too. Their sizes pin the layout
// the offsets in read() were established on (BMW G20, software 202111011321). Another build with these sizes is
// accepted, and decode() then cross-checks every read: the two copies of each UUID must agree, or nothing is changed.
const uint32_t kAppSize = 15032, kInfoSize = 480, kConnectionsSize = 36;
bool g_allowAnyPathForTest = false;

bool trustedLibraryPath(const std::string& path) {
    if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos) {
        return false;
    }
    if (g_allowAnyPathForTest) {
        return true;
    }
    return path.compare(0, 5, "/opt/") == 0 || path.compare(0, 15, "/usr/local/lib/") == 0 ||
           path.compare(0, 9, "/usr/lib/") == 0 || path.compare(0, 5, "/lib/") == 0;
}

bool readLayout(const std::string& path, SppLayout& out, std::string& why) {
    ElfFile elf;
    ElfSymbol app, info, connections;
    if (!elf.load(path)) {
        why = "Alps library unreadable";
        return false;
    }
    if (!elf.symbol("app", app) || !elf.symbol("spp_info", info) || !elf.symbol("spp_connected", connections) ||
        !app.got || !info.got || !connections.got) {
        why = "Alps tables missing from the library";
        return false;
    }
    if (app.size != kAppSize || info.size != kInfoSize || connections.size != kConnectionsSize) {
        char detail[96];
        snprintf(detail, sizeof(detail), "unknown Alps structure (sizes %u/%u/%u)", app.size, info.size,
                 connections.size);
        why = detail;
        return false;
    }
    out.app = app.value;
    out.info = info.value;
    out.connections = connections.value;
    out.appGot = app.got;
    out.infoGot = info.got;
    out.connectionsGot = connections.got;
    return true;
}
std::string startIdentity(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    size_t p = line.rfind(')');
    if (p == std::string::npos) {
        return "";
    }
    std::istringstream fields(line.substr(p + 1));
    std::string value;
    for (int i = 3; i <= 22; ++i) {
        if (!(fields >> value)) {
            return "";
        }
    }
    return value;  // monotonic process start ticks, survives command-name spaces
}
}  // namespace
SppSnapshotReader::SppSnapshotReader() : fd_(-1), pid_(0), base_(0) {}
SppSnapshotReader::~SppSnapshotReader() {
    close();
}
void SppSnapshotReader::close() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    pid_ = 0;
    base_ = 0;
    identity_.clear();
}
void SppSnapshotReader::setAllowAnyPathForTest(bool allow) {
    g_allowAnyPathForTest = allow;
}
bool SppSnapshotReader::bytes(uint64_t offset, void* data, unsigned size) {
    if (fd_ < 0 || offset > 0x100000000ULL) {
        return false;
    }
    const uint64_t address = static_cast<uint64_t>(base_) + offset;
    return address <= 0x100000000ULL && size <= 0x100000000ULL - address &&
           pread64(fd_, data, size, static_cast<off64_t>(address)) == static_cast<ssize_t>(size);
}
bool SppSnapshotReader::open(int expectedPid, int expectedUid) {
    if (fd_ >= 0) {
        return false;
    }
    error_ = "Alps library not found or not unique";
    std::string rejected;
    SppLayout found;
    unsigned matches = 0;
    int selectedPid = 0;
    uint32_t selectedBase = 0;
    std::string selectedPath;
    auto inspectPid = [&](int pid) {
        char buf[32];
        snprintf(buf, sizeof(buf), "/proc/%d", pid);
        const std::string prefix = buf;
        struct stat procSt;
        if (stat(prefix.c_str(), &procSt) != 0 || !S_ISDIR(procSt.st_mode)) {
            return;
        }
        if (expectedUid >= 0 && procSt.st_uid != static_cast<uid_t>(expectedUid)) {
            return;
        }
        std::ifstream maps((prefix + "/maps").c_str());
        std::string line;
        while (std::getline(maps, line)) {
            unsigned long begin, finish, offset, inode;
            unsigned major, minor;
            char perms[5];
            int used = 0;
            if (sscanf(line.c_str(), "%lx-%lx %4s %lx %x:%x %lu %n", &begin, &finish, perms, &offset, &major, &minor,
                       &inode, &used) != 7 ||
                offset != 0 || !used || finish <= begin) {
                continue;
            }
            std::string path = line.substr(used);
            size_t slash = path.rfind('/');
            if (slash == std::string::npos || path.substr(slash + 1) != "libevo_btappl.so") {
                continue;
            }
            if (begin > 0xffffffffUL || !trustedLibraryPath(path)) {
                continue;
            }
            std::string why;
            if (!readLayout(prefix + "/root" + path, found, why)) {
                rejected = why;
                continue;
            }
            ++matches;
            selectedPid = pid;
            selectedBase = static_cast<uint32_t>(begin);
            selectedPath = prefix;
        }
    };
    if (expectedPid > 0) {
        inspectPid(expectedPid);
    } else {
        DIR* proc = opendir("/proc");
        if (!proc) {
            return false;
        }
        dirent* item;
        while ((item = readdir(proc))) {
            char* end = nullptr;
            long pid = strtol(item->d_name, &end, 10);
            if (!*item->d_name || *end || pid <= 0 || pid > INT_MAX) {
                continue;
            }
            inspectPid(static_cast<int>(pid));
        }
        closedir(proc);
    }
    if (matches != 1) {
        if (!matches && !rejected.empty()) {
            error_ = rejected;
        } else if (matches > 1) {
            error_ = "Alps library not unique";
        } else if (expectedPid > 0 || expectedUid >= 0) {
            char detail[96];
            snprintf(detail, sizeof(detail), "Alps library not found (pid=%d, uid=%d)", expectedPid, expectedUid);
            error_ = detail;
        }
        return false;
    }
    const std::string beforeId = startIdentity(selectedPid);
    if (beforeId.empty()) {
        return false;
    }
    // No ptrace, no pause, no write to a BMW process. O_RDONLY only.
    const int memFd = ::open((selectedPath + "/mem").c_str(), O_RDONLY | O_CLOEXEC);
    if (memFd < 0) {
        error_ = "reading Alps in /proc refused: no Bluetooth change allowed";
        return false;
    }
    if (startIdentity(selectedPid) != beforeId) {
        ::close(memFd);
        error_ = "Alps process replaced while opening /proc";
        return false;
    }
    pid_ = selectedPid;
    base_ = selectedBase;
    layout_ = found;
    identity_ = beforeId;
    fd_ = memFd;
    uint32_t a, b, c;
    const uint64_t expectApp = static_cast<uint64_t>(base_) + layout_.app;
    const uint64_t expectInfo = static_cast<uint64_t>(base_) + layout_.info;
    const uint64_t expectConn = static_cast<uint64_t>(base_) + layout_.connections;
    if (expectApp > 0xFFFFFFFFULL || expectInfo > 0xFFFFFFFFULL || expectConn > 0xFFFFFFFFULL ||
        !bytes(layout_.appGot, &a, 4) || !bytes(layout_.infoGot, &b, 4) || !bytes(layout_.connectionsGot, &c, 4) ||
        a != expectApp || b != expectInfo || c != expectConn) {
        error_ = "incompatible Alps layout (relocations)";
        close();
        return false;
    }
    error_.clear();
    return true;
}
bool SppSnapshotReader::decode(uint32_t state, const uint8_t record[34], const uint8_t data[80],
                               const uint8_t connected[6], SppSlotSnapshot& out, bool allowSplit) {
    static const unsigned length[] = {0, 2, 4, 16}, tags[] = {0, 1, 2, 4};
    SppSlotSnapshot parsed;
    if (state > 2 || record[32] < 1 || record[32] > 3 || record[33] < 1 || record[33] > 3) {
        return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        unsigned type = record[32 + i], dataOffset = i ? 56 : 39, tagOffset = i ? 55 : 38;
        unsigned lowType = data[tagOffset] == 1 ? 1 : data[tagOffset] == 2 ? 2 : data[tagOffset] == 4 ? 3 : 0;
        if (!lowType) {
            return false;
        }
        if (!allowSplit &&
            (data[tagOffset] != tags[type] || memcmp(record + 16 * i, data + dataOffset, length[type]))) {
            return false;
        }
        parsed.uuidPayload.push_back(static_cast<uint8_t>(type));
        parsed.uuidPayload.insert(parsed.uuidPayload.end(), record + 16 * i, record + 16 * i + length[type]);
        parsed.transportUuidPayload.push_back(static_cast<uint8_t>(lowType));
        parsed.transportUuidPayload.insert(parsed.transportUuidPayload.end(), data + dataOffset,
                                           data + dataOffset + length[lowType]);
    }
    parsed.state = state;
    parsed.transportState = data[4];
    for (unsigned i = 0; i < 6; ++i) {
        parsed.connected = parsed.connected || connected[i] != 0;
    }
    out = parsed;
    return true;
}
bool SppSnapshotReader::read(unsigned instance, SppSlotSnapshot& out, bool allowSplit) {
    if (instance > 5 || identity_.empty() || startIdentity(pid_) != identity_) {
        error_ = "invalid instance or Alps process replaced";
        return false;
    }
    SppSlotSnapshot first, second;
    for (int round = 0; round < 2; ++round) {
        uint32_t state;
        uint8_t record[34], data[80], connected[6];
        if (!bytes(static_cast<uint64_t>(layout_.app) + 0x514ULL + instance * 4ULL, &state, 4) ||
            !bytes(static_cast<uint64_t>(layout_.app) + 0x64cULL + instance * 34ULL, record, 34) ||
            !bytes(static_cast<uint64_t>(layout_.info) + instance * 80ULL, data, 80) ||
            !bytes(static_cast<uint64_t>(layout_.connections) + instance * 6ULL, connected, 6)) {
            error_ = "incomplete read of the Alps tables in /proc";
            return false;
        }
        if (!decode(state, record, data, connected, round ? second : first, allowSplit)) {
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "SPP state or UUID copies incompatible (state=%u transport=%u types=%u/%u tags=%u/%u)", state,
                     data[4], record[32], record[33], data[38], data[55]);
            error_ = detail;
            return false;
        }
        if (!round) {
            usleep(20000);
        }
    }
    if (first.state != second.state || first.transportState != second.transportState ||
        first.connected != second.connected || first.uuidPayload != second.uuidPayload ||
        first.transportUuidPayload != second.transportUuidPayload) {
        error_ = "SPP state changing";
        return false;
    }
    out = second;
    error_.clear();
    return true;
}
}  // namespace evo
