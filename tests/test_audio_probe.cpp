// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "audio_probe.hpp"
#include "vmost_client.hpp"

static std::string readLog(const char* path) {
    std::string text;
    FILE* file = fopen(path, "r");
    if (file) {
        char buffer[256];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), file)) != 0) {
            text.append(buffer, n);
        }
        fclose(file);
    }
    return text;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    char dir[] = "/tmp/entry-audio-probe-XXXXXX";
    assert(mkdtemp(dir));
    const std::string socketPath = std::string(dir) + "/socket";
    const std::string logPath = std::string(dir) + "/sent";
    assert(setenv("FAKE_VMOST_LOG", logPath.c_str(), 1) == 0);
    evo::setVmostForTest(socketPath.c_str(), argv[1]);
    std::vector<uint8_t> table;
    assert(!evo::readAudioRouteTable(table, 100) && table.empty());

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    assert(socketPath.size() < sizeof(addr.sun_path));
    strcpy(addr.sun_path, socketPath.c_str());
    assert(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    assert(setenv("FAKE_AUDIO_TABLE", "good", 1) == 0);
    assert(evo::readAudioRouteTable(table, 1000));
    const uint8_t expected[] = {0, 0, 0, 1, 0, 3, 0x15, 2};
    assert(table == std::vector<uint8_t>(expected, expected + sizeof(expected)));
    const std::string log = readLog(logPath.c_str());
    assert(log.find("register 03.00\n") != std::string::npos);
    assert(log.find("send addr=ffff 03.00.c03 op=1 len=2 : 00 00\n") != std::string::npos);
    assert(log.find("deregister 03.00\n") != std::string::npos);
    assert(log.find("op=6") == std::string::npos);

    assert(setenv("FAKE_AUDIO_TABLE", "bad", 1) == 0);
    assert(!evo::readAudioRouteTable(table, 1000) && table.empty());
    assert(setenv("FAKE_WLAN_SILENT", "1", 1) == 0);
    assert(!evo::readAudioRouteTable(table, 50) && table.empty());

    close(fd);
    unlink(socketPath.c_str());
    unlink(logPath.c_str());
    rmdir(dir);
    puts("PASS audio route probe: read-only Get, malformed reply and timeout");
    return 0;
}
