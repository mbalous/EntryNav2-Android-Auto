// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The service's sockets and pipes stay out of the programs it starts (close-on-exec), and a program started while the
// phone listener runs does not keep its port.
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "car_inbox.hpp"
#include "kombi_client.hpp"
#include "kombi_fake_kombi.hpp"
#include "kombi_test_util.hpp"
#include "tcp_server.hpp"

extern char** environ;

namespace {

std::atomic<int> g_handed(-1);
std::atomic<int> g_calls(0);

void onPhone(int fd, void*) {
    g_handed = fd;
    ++g_calls;
}

bool handedOver(int calls) {
    for (int i = 0; i < 300 && g_calls < calls; ++i) {
        usleep(10000);
    }
    return g_calls == calls;
}

bool closedOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

std::vector<int> openDescriptors() {
    std::vector<int> fds;
    DIR* dir = opendir("/proc/self/fd");
    CHECK(dir != nullptr);
    const int own = dirfd(dir);
    while (dirent* e = readdir(dir)) {
        char* end = nullptr;
        const long fd = strtol(e->d_name, &end, 10);
        if (end != e->d_name && *end == '\0' && fd != own) {
            fds.push_back(static_cast<int>(fd));
        }
    }
    closedir(dir);
    return fds;
}

// The descriptors opened since before that a started program would get.
int inheritedSince(const std::vector<int>& before) {
    int count = 0;
    const std::vector<int> now = openDescriptors();
    for (size_t i = 0; i < now.size(); ++i) {
        bool old = false;
        for (size_t j = 0; j < before.size() && !old; ++j) {
            old = before[j] == now[i];
        }
        if (!old && !closedOnExec(now[i])) {
            fprintf(stderr, "descriptor %d would be inherited\n", now[i]);
            ++count;
        }
    }
    return count;
}

uint16_t localPort(int fd, bool peer) {
    sockaddr_in a = sockaddr_in();
    socklen_t size = sizeof(a);
    const int rc = peer ? getpeername(fd, reinterpret_cast<sockaddr*>(&a), &size)
                        : getsockname(fd, reinterpret_cast<sockaddr*>(&a), &size);
    return rc == 0 && a.sin_family == AF_INET ? ntohs(a.sin_port) : 0;
}

int dial(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in to = sockaddr_in();
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    return fd;
}

// This test itself, started again to wait a few seconds: a program the service would start meanwhile.
pid_t startProgram(const char* self) {
    pid_t pid = -1;
    char hold[] = "--hold";
    char* argv[] = {const_cast<char*>(self), hold, nullptr};
    CHECK(posix_spawn(&pid, self, nullptr, nullptr, argv, environ) == 0);
    return pid;
}

void endProgram(pid_t pid) {
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
}

// The listener and the phone's socket; then the case another project met in a car: a program started while the
// listener runs, the listener made again (stop, start) while it still runs. Without close-on-exec it keeps the port:
// the new listener cannot bind, and the phone's connection is queued on its copy, never handed over.
void phoneListener(const char* self) {
    const uint16_t port = static_cast<uint16_t>(40000 + getpid() % 20000);
    const std::vector<int> before = openDescriptors();
    aa::TcpServer server(port);
    server.setHotspotAddress("127.0.0.1");
    server.setConnectionCallback(onPhone, nullptr);
    CHECK(server.start());
    const int phone = dial(port);
    CHECK(phone >= 0 && handedOver(1));
    CHECK(closedOnExec(g_handed));
    CHECK(inheritedSince(before) == 0);  // the listener, the phone's socket, the thread's
    close(phone);
    close(g_handed);
    server.notifySessionEnded();

    const pid_t program = startProgram(self);
    server.stop();
    CHECK(server.start());
    const int again = dial(port);
    CHECK(again >= 0 && handedOver(2));
    close(again);
    close(g_handed);
    server.stop();
    endProgram(program);
}

// The session's inbox wakes its thread through a pipe.
void inbox() {
    const std::vector<int> before = openDescriptors();
    aa::CarInbox inbox;
    CHECK(openDescriptors().size() > before.size());
    CHECK(inheritedSince(before) == 0);
}

// The Ethernet KOMBI's connection, kept between sends.
void clusterLink() {
    kombitest::FakeKombi k;
    evo::kombi::ClusterLink link("127.0.0.1", k.port());
    evo::kombi::RouteGraph g;
    std::vector<evo::kombi::Request> requests;
    CHECK(g.routeStart(evo::kombi::ClusterManeuver(evo::kombi::kLeft, "TEST ST", "0.5", evo::kombi::kClusterMiles),
                       requests));
    requests.resize(1);
    CHECK(link.send(requests) == evo::kombi::kSent);
    int ours = -1;
    const std::vector<int> now = openDescriptors();
    for (size_t i = 0; i < now.size(); ++i) {
        if (localPort(now[i], true) == k.port() && localPort(now[i], false) != k.port()) {
            ours = now[i];
        }
    }
    CHECK(ours >= 0 && closedOnExec(ours));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--hold") == 0) {
        sleep(10);
        return 0;
    }
    phoneListener(argv[0]);
    inbox();
    clusterLink();
    puts("PASS close-on-exec: phone listener and socket, a program started meanwhile keeps no port, inbox, KOMBI link");
    return 0;
}
