// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Tests platform/kombi_nav.cpp end to end: the arrows in the service, with their threads, files and a fake KOMBI.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "evo_log.hpp"
#include "kombi_fake_kombi.hpp"
#include "kombi_most.hpp"
#include "kombi_most_fake.hpp"
#include "kombi_nav.hpp"
#include "kombi_test_util.hpp"

namespace {

using namespace evo::kombi;
using kombitest::Bytes;
using kombitest::FakeKombi;
using kombitest::Step;

int g_bmw = -1;
int bmwProbe() {
    return g_bmw;
}

bool exists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

void touch(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    CHECK(f);
    fclose(f);
}

// A temporary folder for the setting and marker files, and the arrows' settings for the fake KOMBI.
struct Place {
    char dir[64];
    std::string off, marker, single, mostMarker, mostLayout;
    Place() {
        snprintf(dir, sizeof(dir), "/tmp/kombi-navXXXXXX");
        CHECK(mkdtemp(dir));
        off = std::string(dir) + "/kombi-off";
        marker = std::string(dir) + "/kombi-nav.shown";
        single = std::string(dir) + "/kombi-single-maneuver";
        mostMarker = std::string(dir) + "/kombi-most.shown";
        mostLayout = std::string(dir) + "/kombi-most-layout";
    }
    ~Place() {
        unlink(off.c_str());
        unlink(marker.c_str());
        unlink(single.c_str());
        unlink(mostMarker.c_str());
        unlink(mostLayout.c_str());
        unlink((mostLayout + ".new").c_str());
        rmdir(dir);
    }
    ArrowsConfig config(uint16_t port) const {
        ArrowsConfig c;
        c.clusterAddress = "127.0.0.1";
        c.clusterPort = port;
        c.offFile = off;
        c.markerFile = marker;
        c.singleFile = single;
        c.guide.approachMiles = 0;
        return c;
    }
    // The MOST cluster's settings: its registry asked at once, the Ethernet KOMBI (160.48.199.96) never a neighbour.
    MostHudConfig mostConfig() const {
        MostHudConfig c;
        c.markerFile = mostMarker;
        c.offFile = off;
        c.layoutFile = mostLayout;
        c.ethernetFirstS = 0;
        c.guide.graceS = 0;
        c.guide.firstDistanceS = 0;
        c.guide.distanceEveryS = 0;
        return c;
    }
};

// The service's log (stdout) caught in a temporary file while a test runs.
class LogCapture {
public:
    LogCapture() : file_(tmpfile()), saved_(-1) {
        CHECK(file_);
        fflush(stdout);
        saved_ = dup(STDOUT_FILENO);
        CHECK(saved_ >= 0 && dup2(fileno(file_), STDOUT_FILENO) >= 0);
    }
    // Waits until the log holds `what` (each line is flushed as it is written).
    bool waitFor(const std::string& what, double seconds = 10.0) {
        const double end = kombitest::secondsNow() + seconds;
        for (;;) {
            std::string text;
            char buf[4096];
            ssize_t n;
            for (off_t at = 0; (n = pread(fileno(file_), buf, sizeof(buf), at)) > 0; at += n) {
                text.append(buf, static_cast<size_t>(n));
            }
            if (text.find(what) != std::string::npos) {
                return true;
            }
            if (kombitest::secondsNow() >= end) {
                return false;
            }
            usleep(10000);
        }
    }
    std::string stop() {
        fflush(stdout);
        CHECK(dup2(saved_, STDOUT_FILENO) >= 0);
        close(saved_);
        rewind(file_);
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), file_)) > 0) {
            text.append(buf, n);
        }
        fclose(file_);
        fputs(text.c_str(), stdout);  // still in the test's output
        return text;
    }

private:
    FILE* file_;
    int saved_;
};

size_t occurrences(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}

// The messages of one moment, as the session hands them over.
void tell(Arrows& arrows, const Step& step, int64_t metres, const char* text, int unit) {
    const Bytes status = kombitest::status(1), state = kombitest::state(step),
                position = kombitest::position(metres, text, unit);
    Arrows::onNavStatus(kNavStatus, &status[0], status.size(), &arrows);
    Arrows::onNavStatus(kNavState, &state[0], state.size(), &arrows);
    Arrows::onNavStatus(kNavPosition, &position[0], position.size(), &arrows);
}

// The configured file selects both the graph and its log once, before start(), even if the file changes meanwhile.
void routeModeSelectedOnce() {
    for (int single = 0; single < 2; ++single) {
        FakeKombi k;
        Place p;
        if (single) {
            touch(p.single);
        }
        Arrows arrows(p.config(k.port()));
        if (single) {
            CHECK(unlink(p.single.c_str()) == 0);
        } else {
            touch(p.single);
        }

        LogCapture log;
        arrows.start();
        const std::string lines = log.stop();
        CHECK(lines.find(single ? "route graph: next maneuver only (14 requests)"
                                : "route graph: whole captured route") != std::string::npos);
        CHECK(lines.find(single ? "route graph: whole captured route" : "route graph: next maneuver only") ==
              std::string::npos);

        const size_t count = single ? 14 : 97;
        tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
        CHECK(k.waitFor(count) && k.count() == count);
        arrows.stop();
        CHECK(k.count() == count + 6 && !exists(p.marker));
    }
}

void endToEnd() {
    FakeKombi k;
    Place p;
    {
        Arrows arrows(p.config(k.port()));
        CHECK(arrows.settingOn());  // on by default
        arrows.start();
        tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
        Arrows::onNavStatus(0x8001, nullptr, 0, &arrows);  // a message the arrows don't read
        CHECK(k.waitFor(97) && k.count() == 97);
        usleep(300000);
        CHECK(exists(p.marker));  // an arrow may be up
        tell(arrows, Step(7, "TEST ST"), 700, "0.4", kUnitMilesTenths);
        CHECK(k.waitFor(98) && k.count() == 98);
        arrows.onSessionEnded();  // the phone left: cleared at once
        CHECK(k.waitFor(104));
        usleep(400000);
        CHECK(k.count() == 104 && !exists(p.marker));
        tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);  // next session, new route
        CHECK(k.waitFor(104 + 97));
        touch(p.off);  // the menu turns the arrows off: cleared within a second
        CHECK(k.waitFor(104 + 97 + 6, 3.0) && !arrows.settingOn());
        arrows.stop();  // nothing up: no cancel at exit
        CHECK(k.count() == 104 + 97 + 6);
    }
    unlink(p.off.c_str());
    size_t base = k.count();
    {
        Arrows arrows(p.config(k.port()));  // exit with an arrow up: cancelled, marker gone
        arrows.start();
        tell(arrows, Step(8, "MAIN ST"), 300, "0.2", kUnitMilesTenths);
        CHECK(k.waitFor(base + 97));
        arrows.stop();
        CHECK(k.count() == base + 97 + 6 && !exists(p.marker));
    }
    base = k.count();
    touch(p.marker);  // left by a killed run
    touch(p.off);     // the cancel goes even when off
    {
        Arrows arrows(p.config(k.port()));
        arrows.start();
        CHECK(k.waitFor(base + 6));
        tell(arrows, Step(8, "MAIN ST"), 300, "0.2", kUnitMilesTenths);
        usleep(1200000);
        arrows.stop();
        CHECK(k.count() == base + 6 && !exists(p.marker));
    }
    const std::vector<kombitest::Seen> seen = k.seen();
    for (size_t i = 0; i < seen.size(); ++i) {
        CHECK(isAllowedMethod(seen[i].method));
    }
}

// While the BMW navigation guides nothing is written and nothing cancelled; when it ends the arrow comes back whole.
// Unknown never stops the arrows.
void bmwNavigationWins() {
    FakeKombi k;
    Place p;
    g_bmw = -1;
    {
        Arrows arrows(p.config(k.port()));
        arrows.start(&bmwProbe);
        tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
        CHECK(k.waitFor(97) && k.count() == 97);
        g_bmw = 1;
        usleep(2600000);
        tell(arrows, Step(7, "TEST ST"), 700, "0.4", kUnitMilesTenths);
        usleep(600000);
        CHECK(k.count() == 97 && !exists(p.marker));  // the factory graph owns the models
        g_bmw = 0;
        CHECK(k.waitFor(97 * 2, 5.0) && k.count() == 97 * 2);
        arrows.stop();
        CHECK(k.count() == 97 * 2 + 6 && !exists(p.marker));
    }
    g_bmw = -1;
}

// A refused request: the arrows stay off until the next session.
void offUntilNextSession() {
    FakeKombi k;
    Place p;
    k.refuse(1);
    Arrows arrows(p.config(k.port()));
    arrows.start();
    tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
    CHECK(k.waitFor(1 + 6));
    tell(arrows, Step(7, "TEST ST"), 700, "0.4", kUnitMilesTenths);
    usleep(1000000);
    CHECK(k.count() == 1 + 6);
    arrows.onSessionEnded();
    usleep(600000);
    tell(arrows, Step(7, "TEST ST"), 700, "0.4", kUnitMilesTenths);
    CHECK(k.waitFor(1 + 6 + 97));
    arrows.stop();
    CHECK(k.count() == 1 + 6 + 97 + 6);
}

// The MOST cluster's side of the arrows: what reaches vmostd.
class CountingPort : public evo::VmostPort {
public:
    CountingPort() : sent(0) {}
    int send(evo::VmostMsgOut&) override {
        ++sent;
        return 0;
    }
    int shadow(uint8_t, uint8_t, bool) override { return 0; }
    void pump(int) override {}
    int sent;
};

// Ticks the MOST cluster's calls for about a second, as the controller reader would.
int mostCalls(MostHud& most) {
    CountingPort port;
    for (int i = 0; i < 100; ++i) {
        most.tick(evo::monoMs(), port);
        usleep(10000);
    }
    return port.sent;
}

// The Ethernet KOMBI tells the MOST cluster it took an arrow: on a G-series car the MOST cluster stays alone, even with
// the phone guiding.
void mostAloneWithEthernet() {
    FakeKombi k;
    Place p;
    MostHud most(p.mostConfig());
    Arrows arrows(p.config(k.port()));
    arrows.attachMost(&most);
    arrows.start();
    tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
    CHECK(k.waitFor(97));
    const Bytes status = kombitest::status(1), state = kombitest::state(Step(7, "TEST ST")),
                position = kombitest::position(800, "0.5", kUnitMilesTenths);
    MostHud::onNavStatus(kNavStatus, &status[0], status.size(), &most);
    MostHud::onNavStatus(kNavState, &state[0], state.size(), &most);
    MostHud::onNavStatus(kNavPosition, &position[0], position.size(), &most);
    CHECK(mostCalls(most) == 0 && most.phase() == MostHud::kIdle);
    CHECK(!most.fieldsReady());
    arrows.stop();
}

// The phone's messages of one moment, to the MOST cluster.
void tellMost(MostHud& most, const Step& step, int64_t metres, const char* text, int unit) {
    const Bytes status = kombitest::status(1), state = kombitest::state(step),
                position = kombitest::position(metres, text, unit);
    MostHud::onNavStatus(kNavStatus, &status[0], status.size(), &most);
    MostHud::onNavStatus(kNavState, &state[0], state.size(), &most);
    MostHud::onNavStatus(kNavPosition, &position[0], position.size(), &most);
}

// An F-series car: nothing answers at the Ethernet KOMBI's address (here a port that refuses every connection, nothing
// ever written), the MOST cluster's fields answer. The Ethernet KOMBI has its chance first; once the MOST fields are
// ready it is not tried again in this run, neither in this session nor in the next.
void ethernetLeftToMost() {
    Place p;
    const int closed = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(a);
    CHECK(closed >= 0 && bind(closed, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) == 0 &&
          getsockname(closed, reinterpret_cast<struct sockaddr*>(&a), &size) == 0);  // bound, not listening
    MostHud most(p.mostConfig());
    Arrows arrows(p.config(ntohs(a.sin_port)));
    arrows.attachMost(&most);
    LogCapture log;
    arrows.start();
    tell(arrows, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);  // tried once, refused: again in 30 s
    CHECK(log.waitFor("KOMBI link failed: retrying in 30 s"));
    // The MOST cluster, as the controller reader drives it: registry, six sizes, the picture.
    kombitest::FakePort port;
    kombitest::Car car;
    uint32_t now = 1000;
    tellMost(most, Step(7, "TEST ST"), 800, "0.5", kUnitMilesTenths);
    kombitest::run(most, port, car, now, 2000);
    CHECK(most.fieldsReady() && most.phase() == MostHud::kReady && most.showing());
    usleep(600000);  // the arrows' next ticks
    arrows.onSessionEnded();
    usleep(400000);
    tell(arrows, Step(8, "MAIN ST"), 300, "0.2", kUnitMilesTenths);  // the next session's route
    usleep(1200000);
    arrows.stop();
    const std::string text = log.stop();
    CHECK(occurrences(text, "request(s) FAILED") == 1);  // the first try only
    CHECK(occurrences(text, "the Ethernet KOMBI 127.0.0.1 is not tried again this run") == 1);
    // What the first picture's line says: what is known, nothing about what the driver sees.
    CHECK(occurrences(text, "first picture sent to the cluster, no call refused (176x126)") == 1);
    most.finish(port);
    close(closed);
}

}  // namespace

int main() {
    routeModeSelectedOnce();
    endToEnd();
    bmwNavigationWins();
    offUntilNextSession();
    mostAloneWithEthernet();
    ethernetLeftToMost();
    puts(
        "PASS kombi nav: route mode and log, menu setting, session end, exit, marker of a killed run, BMW navigation, "
        "refusal, MOST cluster left alone by an Ethernet KOMBI, Ethernet KOMBI left once the MOST fields answered");
    return 0;
}
