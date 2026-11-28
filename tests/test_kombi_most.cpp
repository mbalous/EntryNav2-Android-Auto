// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Tests platform/kombi_most.cpp and kombi_most_layout.cpp: the F-series cluster's guidance fields over MOST, with a
// fake vmostd connection, and the fields learned from the BMW navigation.
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "kombi_display.hpp"
#include "kombi_most.hpp"
#include "kombi_most_fake.hpp"
#include "kombi_test_util.hpp"

namespace {

using namespace evo;
using namespace evo::kombi;
using kombitest::answer;
using kombitest::Bytes;
using kombitest::Car;
using kombitest::FakePort;
using kombitest::Later;
using kombitest::Message;
using kombitest::run;
using kombitest::sizeAnswerOf;
using kombitest::Step;

// Twice the signed area: positive for the winding the stock arrows use (clockwise on the screen).
long winding(const std::vector<HudPoint>& p) {
    long area = 0;
    for (size_t k = 0; k < p.size(); ++k) {
        area +=
            static_cast<long>(p[k].x) * p[(k + 1) % p.size()].y - static_cast<long>(p[(k + 1) % p.size()].x) * p[k].y;
    }
    return area;
}

void extent(const std::vector<HudPoint>& p, int& minX, int& maxX, int& minY, int& maxY) {
    minX = minY = 1 << 20;
    maxX = maxY = -1;
    for (size_t k = 0; k < p.size(); ++k) {
        minX = p[k].x < minX ? p[k].x : minX;
        maxX = p[k].x > maxX ? p[k].x : maxX;
        minY = p[k].y < minY ? p[k].y : minY;
        maxY = p[k].y > maxY ? p[k].y : maxY;
    }
}

Maneuver maneuverOf(int type, const std::string& road, int64_t metres, const std::string& text, int unit) {
    const NavInbox in = kombitest::inbox(kombitest::state(Step(type, road)), kombitest::position(metres, text, unit));
    Maneuver m;
    CHECK(in.nextManeuver(m));
    return m;
}

// ---- the protocol's pieces ----

void registryAndSizes() {
    const uint8_t reg[] = {0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x82, 0x00, 0x01, 0x01, 0x52, 0x00, 0xFF};
    uint16_t a = 0;
    CHECK(registryAddress(reg, sizeof(reg), 0x82, 0x00, &a) && a == 0x0101);
    CHECK(registryAddress(reg, sizeof(reg), 0x02, 0x00, &a) && a == 0x0100);
    CHECK(!registryAddress(reg, sizeof(reg), 0x82, 0x02, &a));
    CHECK(!registryAddress(reg, 3, 0x02, 0x00, &a));  // no whole record

    const uint8_t size[] = {0x00, 0x05, 0x01, 0x00, 0xB0, 0x00, 0x7E};
    uint16_t field = 0;
    FieldSize s;
    CHECK(fieldSize(size, sizeof(size), &field, &s) && field == 5 && s.w == 176 && s.h == 126);
    CHECK(!fieldSize(size, 5, &field, &s));

    FieldSize sizes[kHudFields] = {{396, 68}, {272, 33}, {80, 28}, {44, 28}, {176, 126}, {176, 126}};
    std::string why;
    CHECK(layoutFits(sizes, &why));
    sizes[5].w = 0;
    sizes[4].w = 0;
    CHECK(!layoutFits(sizes, &why) && why.find("never set up") != std::string::npos);
    sizes[4].w = 176;
    sizes[5].w = 150;
    CHECK(!layoutFits(sizes, &why) && why.find("differ") != std::string::npos);
}

void calls() {
    const HudCall show = hudShow(1, true);
    CHECK(show.fkt == 0x204 && show.data == Bytes({0x00, 0x01, 0x01}));
    CHECK(hudSwap(6, 5).data == Bytes({0x00, 0x06, 0x00, 0x05}) && hudSwap(6, 5).fkt == 0x20E);
    CHECK(hudEmpty(5).fkt == 0x220 && hudEmpty(5).data == Bytes({0x00, 0x05}));
    CHECK(hudSize(3).fkt == 0xC04 && hudSize(3).data == Bytes({0x00, 0x03, 0x01}));
    // UTF-16BE after the field and a zero byte, then a zero character.
    CHECK(hudText(2, "Rue A").data ==
          Bytes({0x00, 0x02, 0x00, 0x00, 'R', 0x00, 'u', 0x00, 'e', 0x00, ' ', 0x00, 'A', 0x00, 0x00}));
    CHECK(hudText(2, "\xC3\xA9\xE2\x82\xAC").data == Bytes({0x00, 0x02, 0x00, 0x00, 0xE9, 0x20, 0xAC, 0x00, 0x00}));
    CHECK(hudText(2, "\xF0\x9F\x9A\x97").data == Bytes({0x00, 0x02, 0x00, 0x00, '?', 0x00, 0x00}));  // past U+FFFF
    CHECK(hudText(2, "a\xFF").data == Bytes({0x00, 0x02, 0x00, 0x00, 'a', 0x00, '?', 0x00, 0x00}));
    CHECK(hudText(4, "").data == Bytes({0x00, 0x04, 0x00, 0x00, 0x00}));
}

void pictures() {
    const int directions[] = {kStraightOn,
                              kHalfLeft,
                              kHalfRight,
                              kTightLeft,
                              kTightRight,
                              kLeft,
                              kRight,
                              kUTurnLeft,
                              kUTurnRight,
                              kDestination,
                              kRoundaboutClockwise,
                              kRoundaboutCounterClockwise,
                              kHighwayInLeft,
                              kHighwayOutRight,
                              kLaneChangeLeft,
                              kForkRight};
    const FieldSize fields[] = {{176, 126}, {120, 90}};
    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f) {
        const uint16_t w = fields[f].w, h = fields[f].h;
        for (size_t i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
            const std::vector<HudShape> shapes = hudPicture(directions[i], w, h);
            CHECK(!shapes.empty() && shapes.size() <= 16);
            for (size_t s = 0; s < shapes.size(); ++s) {
                CHECK(convex(shapes[s].points));  // as the stock arrows: convex pieces only
                CHECK(winding(shapes[s].points) > 0);
                CHECK(shapes[s].points.size() >= 3 && shapes[s].points.size() <= 8);
                for (size_t k = 0; k < shapes[s].points.size(); ++k) {
                    const HudPoint& p = shapes[s].points[k];
                    // Inside the field and clear of the shade on its right.
                    CHECK(p.x >= w * 0.24 - 1 && p.x <= w * 0.92 + 1 && p.y >= 0 && p.y < h);
                }
            }
        }
    }
    CHECK(hudPicture(0, 176, 126).empty() && hudPicture(11, 176, 126).empty());
    // Left is the mirror of right about the middle of the clear part.
    const std::vector<HudShape> right = hudPicture(kRight, 176, 126), left = hudPicture(kLeft, 176, 126);
    CHECK(right.size() == left.size());
    const double middle = (176 * 0.24 + 176 * 0.92) / 2;
    for (size_t s = 0; s < right.size(); ++s) {
        int r0, r1, ry0, ry1, l0, l1, ly0, ly1;
        extent(right[s].points, r0, r1, ry0, ry1);
        extent(left[s].points, l0, l1, ly0, ly1);
        CHECK(r0 + l1 >= 2 * middle - 2 && r0 + l1 <= 2 * middle + 2 && r1 + l0 >= 2 * middle - 2 &&
              r1 + l0 <= 2 * middle + 2 && ry0 == ly0 && ry1 == ly1);
    }
    // The roundabout's ring is grey, its arrow white.
    const std::vector<HudShape> ring = hudPicture(kRoundaboutCounterClockwise, 176, 126);
    CHECK(ring.front().colour == kHudGrey && ring.back().colour == kHudWhite);
    std::vector<HudPoint> concave;
    const HudPoint pts[] = {{0, 0}, {10, 0}, {5, 2}, {10, 10}, {0, 10}};
    concave.assign(pts, pts + 5);
    CHECK(!convex(concave));

    HudCalls out;
    pictureCalls(ring, 5, out);
    CHECK(out[0] == hudEmpty(5) && out[1].fkt == 0x219 && out[1].data == Bytes({0x00, 0x05, 0x00}));
    CHECK(out[2].fkt == 0x21B && out[2].data == Bytes({0x00, 0x05, kHudGrey}));
    CHECK(out[3].fkt == 0x21D && out[3].data[2] == 0 && out[3].data[3] == 0 && out[3].data[4] == 3);
    CHECK(out[3].data[5] == ring[0].points.size() && out[3].data.size() == 6 + 4 * ring[0].points.size());
    size_t colours = 0, polygons = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        colours += out[i].fkt == 0x21B;
        polygons += out[i].fkt == 0x21D;
    }
    CHECK(colours == 2 && polygons == ring.size());
    CHECK(out.back().data[2] == 0 && out.back().data[3] == ring.size() - 1);  // indices follow the shapes
}

void views() {
    const HudLayout l;
    const FieldSize picture = {176, 126};
    HudView v;
    v.direction = kRight;
    v.distance = "300";
    v.unit = "m";
    v.street = "Rue A";
    uint16_t visible = l.pictureB;
    HudCalls out;
    viewCalls(v, nullptr, l, picture, visible, out);
    // Picture into the hidden buffer A, swap, empty B, the text fields shown and filled, then the frame.
    CHECK(visible == l.pictureA && out[0] == hudEmpty(l.pictureA));
    size_t i = 0;
    while (out[i].fkt != 0x20E) {
        CHECK(out[i].data[0] == 0 && out[i].data[1] == l.pictureA);
        ++i;
    }
    CHECK(out[i] == hudSwap(l.pictureB, l.pictureA) && out[i + 1] == hudEmpty(l.pictureB));
    CHECK(out[i + 2] == hudShow(l.street, true) && out[i + 3] == hudShow(l.distance, true) &&
          out[i + 4] == hudShow(l.unit, true));
    CHECK(out[i + 5] == hudText(l.street, "Rue A") && out[i + 6] == hudText(l.distance, "300") &&
          out[i + 7] == hudText(l.unit, "m") && out[i + 8] == hudShow(l.frame, true) && out.size() == i + 9);

    // Same maneuver, nearer: the distance only.
    HudView nearer = v;
    nearer.distance = "250";
    out.clear();
    viewCalls(nearer, &v, l, picture, visible, out);
    CHECK(out.size() == 1 && out[0] == hudText(l.distance, "250") && visible == l.pictureA);
    // Next maneuver: into B this time, the street and the distance.
    HudView next = nearer;
    next.direction = kLeft;
    next.street = "Rue B";
    out.clear();
    viewCalls(next, &nearer, l, picture, visible, out);
    CHECK(visible == l.pictureB && out[0] == hudEmpty(l.pictureB));
    bool swapped = false;
    for (size_t k = 0; k < out.size(); ++k) {
        swapped |= out[k] == hudSwap(l.pictureA, l.pictureB);
    }
    CHECK(swapped && out.back() == hudText(l.street, "Rue B"));

    out.clear();
    clearCalls(l, out);
    CHECK(out.size() == 6 && out[0] == hudShow(l.frame, false) && out[1] == hudEmpty(l.pictureA) &&
          out[2] == hudEmpty(l.pictureB) && out[3] == hudText(l.street, "") && out[5] == hudText(l.unit, ""));
}

// ---- what is shown ----

void presenter() {
    GuideSettings s;  // approach 1 mi, hysteresis 1.15, grace 8 s, first distance 3 s, distance every 0.5 s
    HudPresenter p(s);
    HudView v;
    const Maneuver far = maneuverOf(8, "Rue A", 3000, "3.0", kUnitKilometresTenths);
    CHECK(!p.step(&far, 0, v));  // beyond the approach
    const Maneuver near = maneuverOf(8, "Rue A", 800, "800", kUnitMetres);
    CHECK(p.step(&near, 1, v) && v.direction == kRight && v.distance == "800" && v.unit == "m" && v.street == "Rue A");
    // Hysteresis: the same turn stays up to 1.15 mi, then goes.
    const Maneuver back = maneuverOf(8, "Rue A", 1750, "1.8", kUnitKilometresTenths);
    CHECK(p.step(&back, 2, v) && v.distance == "1.8" && v.unit == "km");
    const Maneuver gone = maneuverOf(8, "Rue A", 1900, "1.9", kUnitKilometresTenths);
    CHECK(!p.step(&gone, 3, v));
    // The distance changes at most every 0.5 s on the same maneuver.
    CHECK(p.step(&near, 10, v) && v.distance == "800");
    const Maneuver closer = maneuverOf(8, "Rue A", 700, "700", kUnitMetres);
    CHECK(p.step(&closer, 10.2, v) && v.distance == "800");
    CHECK(p.step(&closer, 10.6, v) && v.distance == "700");
    // Guidance gone: the view stays 8 s.
    CHECK(p.step(nullptr, 11, v) && v.distance == "700");
    CHECK(p.step(nullptr, 18.9, v));
    CHECK(!p.step(nullptr, 19.1, v));
    // A first maneuver without a distance waits 3 s, then shows "-".
    p.reset();
    Maneuver none = near;
    none.distanceText.clear();
    none.hasMetres = false;
    CHECK(!p.step(&none, 30, v) && !p.step(&none, 32.9, v));
    CHECK(p.step(&none, 33.1, v) && v.distance == "-" && v.unit.empty());
    // Miles stay miles; a roundabout's exit goes before the street.
    p.reset();
    const Maneuver miles = maneuverOf(34, "Main St", 700, "0.4", kUnitMilesTenths);
    Maneuver exitTwo = miles;
    exitTwo.cue = "Take the 2nd exit";
    CHECK(p.step(&exitTwo, 40, v) && v.unit == "mi" && v.distance == "0.4" && v.street == "Exit 2 - Main St" &&
          v.direction == kRoundaboutCounterClockwise);
    // A long street is cut to 30 characters.
    p.reset();
    const Maneuver longName = maneuverOf(7, std::string(40, 'x'), 300, "300", kUnitMetres);
    CHECK(p.step(&longName, 50, v) && v.street.size() == 30);
}

void neighbours() {
    const std::string table =
        "IP address       HW type     Flags       HW address            Mask     Device\n"
        "160.48.199.96    0x1         0x2         02:00:00:00:00:96     *        eth0\n"
        "160.48.199.97    0x1         0x0         00:00:00:00:00:00     *        eth0\n"
        "172.16.222.50    0x1         0x2         aa:bb:cc:dd:ee:ff     *        wlan0\n";
    CHECK(arpHasNeighbour(table, "160.48.199.96"));
    CHECK(!arpHasNeighbour(table, "160.48.199.97"));  // incomplete
    CHECK(!arpHasNeighbour(table, "160.48.199.9"));
    CHECK(!arpHasNeighbour(table.substr(0, table.find('\n') + 1), "160.48.199.96"));
}

// ---- the calls, with a fake connection ----

size_t countFkt(const FakePort& port, uint16_t fkt, size_t from = 0) {
    size_t n = 0;
    for (size_t i = from; i < port.sent.size(); ++i) {
        n += port.sent[i].out.fblock == kHudFblock && port.sent[i].out.fkt == fkt;
    }
    return n;
}

struct Place {
    char dir[64];
    std::string marker, off, layout;
    Place() {
        snprintf(dir, sizeof(dir), "/tmp/kombi-most-XXXXXX");
        CHECK(mkdtemp(dir));
        marker = std::string(dir) + "/kombi-most.shown";
        off = std::string(dir) + "/kombi-off";
        layout = std::string(dir) + "/kombi-most-layout";
    }
    ~Place() {
        unlink(marker.c_str());
        unlink(off.c_str());
        unlink(layout.c_str());
        unlink((layout + ".new").c_str());
        unlink((std::string(dir) + "/log").c_str());
        rmdir(dir);
    }
    MostHudConfig config(double ethernetFirstS = 0) const {
        MostHudConfig c;
        c.markerFile = marker;
        c.offFile = off;
        c.layoutFile = layout;
        c.clusterAddress = "160.48.199.96";  // never a neighbour of the PC
        c.ethernetFirstS = ethernetFirstS;
        c.guide.graceS = 0;
        c.guide.firstDistanceS = 0;
        c.guide.distanceEveryS = 0;
        return c;
    }
    bool marked() const { return access(marker.c_str(), F_OK) == 0; }
    void touch(const std::string& path) const {
        FILE* f = fopen(path.c_str(), "w");
        CHECK(f);
        fclose(f);
    }
};

// The navigation messages of one moment, as the session hands them over.
void tell(MostHud& hud, int type, const std::string& road, int64_t metres, const char* text, int unit) {
    const Bytes status = kombitest::status(1), state = kombitest::state(Step(type, road)),
                position = kombitest::position(metres, text, unit);
    MostHud::onNavStatus(kNavStatus, &status[0], status.size(), &hud);
    MostHud::onNavStatus(kNavState, &state[0], state.size(), &hud);
    MostHud::onNavStatus(kNavPosition, &position[0], position.size(), &hud);
}

void guidanceOff(MostHud& hud) {
    const Bytes status = kombitest::status(2);
    MostHud::onNavStatus(kNavStatus, &status[0], status.size(), &hud);
}

void drawsAndClears() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 1000;
    run(hud, port, car, now, 600);
    CHECK(port.sent.empty() && port.shadows.empty());  // nothing on the bus before a maneuver
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.phase() == MostHud::kReady && hud.showing() && place.marked());
    // The registry request, then six sizes, then the frame; every call to the cluster's address, StartResultAck.
    CHECK(port.sent[0].out.addr == kRegistryAddress && port.sent[0].out.fkt == kRegistryFkt &&
          port.sent[0].out.op == kMostOpGet);
    CHECK(countFkt(port, kHudSize) == 6 && countFkt(port, kHudSwap) == 1 && countFkt(port, kHudText) == 3);
    for (size_t i = 1; i < port.sent.size(); ++i) {
        CHECK(port.sent[i].out.addr == 0x0101 && port.sent[i].out.op == kMostOpStartResultAck &&
              port.sent[i].out.fblock == kHudFblock && port.sent[i].out.inst == 0);
    }
    CHECK(port.shadows.size() == 3);
    for (size_t i = 2; i < port.sent.size(); ++i) {
        CHECK(port.at[i] - port.at[i - 1] >= 20);  // 20 ms apart at least
    }
    // Nearer: one text.
    size_t before = port.sent.size();
    tell(hud, 8, "Rue A", 250, "250", kUnitMetres);
    run(hud, port, car, now, 500);
    CHECK(port.sent.size() == before + 1 && port.sent.back().data == hudText(3, "250").data);
    // Guidance ends: cleared, marker gone.
    guidanceOff(hud);
    run(hud, port, car, now, 600);
    CHECK(!hud.showing() && !place.marked() && countFkt(port, kHudShow, before) == 1);
    // The menu's setting: off clears, nothing more is drawn.
    tell(hud, 8, "Rue A", 250, "250", kUnitMetres);
    run(hud, port, car, now, 600);
    CHECK(hud.showing());
    place.touch(place.off);
    run(hud, port, car, now, 1500);
    CHECK(!hud.showing() && !place.marked());
    unlink(place.off.c_str());
    run(hud, port, car, now, 1500);
    CHECK(hud.showing());
    // A session that ends clears too.
    hud.onSessionEnded();
    run(hud, port, car, now, 600);
    CHECK(!hud.showing());
    // Stopping the service with nothing up sends nothing.
    before = port.sent.size();
    hud.finish(port);
    CHECK(port.sent.size() == before && port.shadows.size() == 6);
}

void pacing() {
    {
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        car.silent = true;  // the registry never answers: asked again 30 s later, three times, then given up
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 6000);
        CHECK(hud.phase() == MostHud::kIdle && port.sent.size() == 1 && hud.dueInMs(now) == 100);
        run(hud, port, car, now, 70000);
        CHECK(hud.phase() == MostHud::kAbsent && port.sent.size() == 3);
    }
    {
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        car.hudSilent = true;  // the cluster never answers: the reads judged after 10 s, asked three times 30 s apart
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 5000);
        CHECK(hud.phase() == MostHud::kSizes && port.sent.size() == 1 + 6 && port.at[2] - port.at[1] >= 20);
        run(hud, port, car, now, 80000);
        CHECK(hud.phase() == MostHud::kSizes && port.sent.size() == 1 + 18);
        run(hud, port, car, now, 15000);
        CHECK(hud.phase() == MostHud::kAbsent && port.sent.size() == 1 + 18);
    }
    {
        Place place;
        MostHud hud(place.config(5.0));  // the Ethernet KOMBI's chance first
        FakePort port;
        Car car;
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 4800);
        CHECK(port.sent.empty());
        run(hud, port, car, now, 2000);
        CHECK(hud.showing());
    }
}

void notAllowedOrBmw() {
    Place place;
    FakePort port;
    Car car;
    uint32_t now = 0;
    {
        MostHud hud(place.config());
        hud.setEthernetSeen();  // a G-series KOMBI took an arrow
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 1000);
        CHECK(port.sent.empty() && port.shadows.empty());
    }
    {
        MostHud hud(place.config());
        hud.setBmwGuiding(true);  // the BMW navigation guides
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 1000);
        place.touch(place.off);  // and the arrows off
        hud.setBmwGuiding(false);
        run(hud, port, car, now, 1500);
        CHECK(port.sent.empty() && port.shadows.empty());
        unlink(place.off.c_str());
    }
    {
        Car g;
        g.hud = false;  // a car without the MOST cluster
        MostHud hud(place.config());
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, g, now, 1000);
        CHECK(hud.phase() == MostHud::kAbsent && port.sent.size() == 1 && countFkt(port, kHudSize) == 0);
        CHECK(port.shadows.size() == 6 && !(port.shadows[3] & 0x10000) && !(port.shadows[4] & 0x10000) &&
              !(port.shadows[5] & 0x10000));  // the copies dropped again
    }
    {
        Car blank;
        blank.pictureW = 0;  // fields never set up: nothing drawn
        port.sent.clear();
        MostHud hud(place.config());
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, blank, now, 2000);
        CHECK(hud.phase() == MostHud::kSizes && countFkt(port, kHudSize) == 6);  // asked again later
        run(hud, port, blank, now, 70000);
        CHECK(hud.phase() == MostHud::kAbsent && countFkt(port, kHudSize) == 18 && countFkt(port, kHudPolygon) == 0);
    }
}

// Right after an iDrive restart the cluster answers the size reads late: they are taken, and the picture goes up.
void coldCluster() {
    {
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        car.sizeDelayMs = 1800;
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 6000);
        CHECK(hud.phase() == MostHud::kReady && hud.showing() && countFkt(port, kHudSize) == 6);
    }
    {
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        car.hudSilent = true;  // nothing the first time, then the cluster wakes up: the missing sizes asked again
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 5000);
        CHECK(hud.phase() == MostHud::kSizes && !hud.showing());
        car.hudSilent = false;
        run(hud, port, car, now, 40000);
        CHECK(hud.phase() == MostHud::kReady && hud.showing() && countFkt(port, kHudSize) == 12);
    }
}

// For minutes after a cold iDrive start the cluster answers every call seconds late (1.1 to 7.5 s on an F30): the arrow
// still goes up, eight calls waiting at most, nothing stops, and a copy of a call still waiting is not another client.
void lateAnswers() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    car.sizeDelayMs = 5300;
    car.answerDelayMs = 5300;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 3000);
    CHECK(hud.phase() == MostHud::kSizes && !hud.showing());
    run(hud, port, car, now, 30000);
    CHECK(hud.phase() == MostHud::kReady && hud.showing() && place.marked());
    CHECK(countFkt(port, kHudSize) == 6 && countFkt(port, kHudSwap) == 1 && countFkt(port, kHudText) == 3);
    const size_t first = 1 + 6;  // the first drawing call
    CHECK(port.sent.size() > first + 8 && port.at[first + 8] - port.at[first] >= 5300);
    // A new distance; the cluster's copy of that call comes while it waits, well after it was sent.
    tell(hud, 8, "Rue A", 250, "250", kUnitMetres);
    size_t before = port.sent.size();
    run(hud, port, car, now, 3500);
    CHECK(port.sent.size() == before + 1);
    const Message waiting = port.sent.back();
    hud.onMessage(answer(kHudFblock, waiting.out.fkt, kMostOpStartResultAck, waiting.data, 0x0100));
    tell(hud, 8, "Rue A", 200, "200", kUnitMetres);
    before = port.sent.size();
    run(hud, port, car, now, 8000);
    CHECK(port.sent.size() == before + 1 && port.sent.back().data == hudText(3, "200").data && hud.showing());
}

// Sizes answered, drawing calls never: eight go out, and only after 10 s without an answer does the drawing stop.
void silentDrawing() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    car.drawSilent = true;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 5000);
    CHECK(hud.phase() == MostHud::kReady && port.sent.size() == 1 + 6 + 8);
    run(hud, port, car, now, 7000);
    CHECK(hud.phase() == MostHud::kStopped && !hud.showing() && place.marked() && port.sent.size() == 1 + 6 + 8);
}

// The iDrive setting moves the guidance to the head-up display: the cluster says so, the picture sizes are read again
// and the arrow is drawn anew inside the HUD's smaller picture. The same place again, or the navigation's answer on the
// same function, changes nothing.
void headUpDisplay() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.showing() && countFkt(port, kHudSize) == 6);
    car.pictureW = 126;
    car.pictureH = 77;
    const Bytes hudPlace({0x02});
    hud.onMessage(answer(kGuidanceFblock, kGuidanceMovedFkt, 0x0, hudPlace, 0x0101));
    size_t before = port.sent.size();
    run(hud, port, car, now, 1000);
    CHECK(countFkt(port, kHudSize, before) == 2 && countFkt(port, kHudSwap, before) == 1);
    CHECK(countFkt(port, kHudText, before) == 0);  // the texts stay
    for (size_t i = before; i < port.sent.size(); ++i) {
        const Bytes& d = port.sent[i].data;
        if (port.sent[i].out.fkt != kHudPolygon) {
            continue;
        }
        for (size_t k = 6; k + 3 < d.size(); k += 4) {
            CHECK(((d[k] << 8) | d[k + 1]) < 126 && ((d[k + 2] << 8) | d[k + 3]) < 77);
        }
    }
    before = port.sent.size();
    hud.onMessage(answer(kGuidanceFblock, kGuidanceMovedFkt, 0x0, hudPlace, 0x0101));       // the same place again
    hud.onMessage(answer(kGuidanceFblock, kGuidanceMovedFkt, 0xC, Bytes({0x01}), 0x0100));  // the navigation
    run(hud, port, car, now, 1000);
    CHECK(port.sent.size() == before);
    // Back to the cluster.
    car.pictureW = 176;
    car.pictureH = 126;
    hud.onMessage(answer(kGuidanceFblock, kGuidanceMovedFkt, 0x0, Bytes({0x01}), 0x0101));
    run(hud, port, car, now, 1000);
    CHECK(countFkt(port, kHudSize, before) == 2 && countFkt(port, kHudSwap, before) == 1 && hud.showing());
}

void refusedThenNextSession() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    car.refuseFkt = kHudSwap;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.phase() == MostHud::kStopped && !hud.showing() && place.marked());
    CHECK(countFkt(port, kHudText) == 0);  // nothing after the refused call
    const size_t before = port.sent.size();
    run(hud, port, car, now, 1000);
    CHECK(port.sent.size() == before);
    // Next session: the possibly half-drawn picture is cleared first, then drawn again.
    car.refuseFkt = 0;
    hud.onSessionEnded();
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.phase() == MostHud::kReady && hud.showing() && countFkt(port, kHudShow, before) >= 2);
    CHECK(port.sent[before].data == hudShow(1, false).data);
}

void otherClientAndEchoes() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.showing());
    // Copies of our own calls on the shadow are not another client.
    const Message last = port.sent.back();
    hud.onMessage(answer(kHudFblock, last.out.fkt, kMostOpStartResultAck, last.data, 0x0100));
    tell(hud, 8, "Rue A", 250, "250", kUnitMetres);
    size_t before = port.sent.size();
    run(hud, port, car, now, 500);
    CHECK(port.sent.size() == before + 1);
    // The BMW navigation draws: the picture is left to it, uncleared, the marker removed.
    const Bytes polygon({0x00, 0x05, 0x00, 0x00, 0x03});
    hud.onMessage(answer(kHudFblock, kHudPolygon, kMostOpStartResultAck, polygon, 0x0100));
    tell(hud, 8, "Rue A", 200, "200", kUnitMetres);
    before = port.sent.size();
    run(hud, port, car, now, 1000);
    CHECK(port.sent.size() == before && !hud.showing() && !place.marked());
    // Thirty seconds later it may draw again.
    run(hud, port, car, now, 30000);
    CHECK(hud.showing());
}

void staleMarker() {
    Place place;
    place.touch(place.marker);
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    run(hud, port, car, now, 2000);  // no guidance, but a picture left by a killed run
    CHECK(hud.phase() == MostHud::kReady && !place.marked() && countFkt(port, kHudShow) == 1);
    CHECK(port.sent.back().data == hudText(4, "").data);
}

void finishClears() {
    {  // Cluster acknowledges every clear call during finish(): marker is removed.
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 2000);
        CHECK(hud.showing() && place.marked());
        const size_t before = port.sent.size();
        port.ackSent = before;
        port.ackTarget = &hud;
        hud.finish(port);
        CHECK(port.sent.size() == before + 6 && port.sent[before].data == hudShow(1, false).data && !place.marked());
    }
    {  // Cluster silent during finish(): the clears went out without a refusal, the marker goes, and the stop does
       // not wait for answers a cold cluster gives seconds late.
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 2000);
        CHECK(hud.showing() && place.marked());
        const size_t before = port.sent.size();
        hud.finish(port);
        CHECK(port.sent.size() == before + 6 && !place.marked());
    }
}

void stoppedBeforeSizesAndHighClock() {
    {  // Stopped while still reading sizes: next session re-reads sizes instead of jumping to kReady unverified.
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 30);  // registry answered, kSizes entered
        port.refuseSends = true;       // vmostd refuses the 5 size reads -> kStopped with layout unverified
        run(hud, port, car, now, 1000);
        CHECK(hud.phase() == MostHud::kStopped && !hud.showing());
        port.refuseSends = false;
        hud.onSessionEnded();
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        const size_t before = port.sent.size();
        run(hud, port, car, now, 2000);
        CHECK(hud.phase() == MostHud::kReady && hud.showing() && countFkt(port, kHudSize, before) == 6);
    }
    {  // Uptime past 2^31 ms (24.85 days): no sentinel-0 freeze on lookup, send pacing, or foreign timeout.
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        uint32_t now = 0x80001000U;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 2000);
        CHECK(hud.phase() == MostHud::kReady && hud.showing() && place.marked());
    }
}

void refusedSends() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 300);  // registry, sizes, part of the frame
    CHECK(hud.phase() == MostHud::kReady && countFkt(port, kHudPolygon) > 0);
    port.refuseSends = true;  // vmostd refuses everything from here: five refusals, then stop
    run(hud, port, car, now, 2000);
    CHECK(hud.phase() == MostHud::kStopped && !hud.showing() && place.marked());
}

// ---- the fields learned from the BMW navigation ----

Bytes textCall(uint16_t field, const std::string& utf8) {
    return hudText(field, utf8).data;
}

void textKinds() {
    CHECK(textKind(&textCall(3, "300")[0], textCall(3, "300").size()) == kTextDistance);
    const char* const distances[] = {"1.2", "0,4", "1/4", "300 m", "12km", " 80 "};
    for (size_t i = 0; i < sizeof(distances) / sizeof(distances[0]); ++i) {
        const Bytes t = textCall(3, distances[i]);
        CHECK(textKind(&t[0], t.size()) == kTextDistance);
    }
    const char* const units[] = {"m", "km", "KM", "ft", "mi", "yd", "yds", "mi."};
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); ++i) {
        const Bytes t = textCall(4, units[i]);
        CHECK(textKind(&t[0], t.size()) == kTextUnit);
    }
    // Streets, in any script; a number inside a name is still a street.
    const char* const streets[] = {"Rue A",
                                   "Main Street",
                                   "3rd Avenue",
                                   "A7",
                                   "Exit 2 - Main St",
                                   "m Street",
                                   "\xEC\x9D\x80\xEA\xB5\xAC\xEB\xB9\x84\xEB\xA1\x9C",
                                   "Stra\xC3\x9F"
                                   "e"};
    for (size_t i = 0; i < sizeof(streets) / sizeof(streets[0]); ++i) {
        const Bytes t = textCall(2, streets[i]);
        CHECK(textKind(&t[0], t.size()) == kTextStreet);
    }
    const char* const others[] = {"-", "12:30", "--:--"};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
        const Bytes t = textCall(2, others[i]);
        CHECK(textKind(&t[0], t.size()) == kTextOther);
    }
    const Bytes empty = textCall(2, ""), blank = textCall(2, "   ");
    CHECK(textKind(&empty[0], empty.size()) == kTextEmpty && textKind(&blank[0], blank.size()) == kTextEmpty);
    // One byte per character when the format byte is not 0 (ISO-8859-15, UTF-8).
    const Bytes narrow({0x00, 0x03, 0x01, '3', '0', '0', 0x00});
    CHECK(textKind(&narrow[0], narrow.size()) == kTextDistance);
    CHECK(textKind(nullptr, 0) == kTextEmpty && textKind(&narrow[0], 2) == kTextEmpty);
}

// A copy of the BMW navigation's call on the fields, as the shadow brings it.
void navCall(MostHud& hud, const HudCall& call) {
    hud.onMessage(answer(kHudFblock, call.fkt, kMostOpStartResultAck, call.data, 0x0100));
}

void navCall(HudObserver& o, const HudCall& call, uint32_t now) {
    o.onCall(call.fkt, &call.data[0], call.data.size(), now);
}

// The fields a BMW navigation guides with, and whether the copies carry the cluster's answers to its size reads.
struct NavFields {
    uint16_t frame, street, distance, unit, a, b;
    FieldSize size;
    bool answers;
};

const char* const kNavStreets[] = {"Teheran-ro", "\xEC\x9D\x80\xEA\xB5\xAC\xEB\xB9\x84\xEB\xA1\x9C", "Main Street"};

// A BMW route of three maneuvers, as the copies show it, between ticks: size reads, the frame, each picture drawn in
// the hidden buffer and swapped, the street, the distance coming down, the unit; at the end everything hidden and
// emptied.
void bmwRoute(MostHud& hud, FakePort& port, Car& car, uint32_t& now, const NavFields& f) {
    navCall(hud, hudSize(f.a));
    navCall(hud, hudSize(f.b));
    if (f.answers) {
        hud.onMessage(answer(kHudFblock, kHudSize, kMostOpResultAck, sizeAnswerOf(f.a, f.size.w, f.size.h)));
        hud.onMessage(answer(kHudFblock, kHudSize, kMostOpResultAck, sizeAnswerOf(f.b, f.size.w, f.size.h)));
    }
    navCall(hud, hudShow(f.frame, true));
    const char* const distances[] = {"800", "500", "300", "100"};
    uint16_t visible = f.b;
    for (int k = 0; k < 3; ++k) {
        const uint16_t hidden = visible == f.a ? f.b : f.a;
        HudCalls picture;
        pictureCalls(hudPicture(k == 1 ? kLeft : kRight, f.size.w ? f.size.w : 120, f.size.h ? f.size.h : 90), hidden,
                     picture);
        for (size_t i = 0; i < picture.size(); ++i) {
            navCall(hud, picture[i]);
        }
        navCall(hud, hudSwap(visible, hidden));
        visible = hidden;
        navCall(hud, hudText(f.street, kNavStreets[k]));
        navCall(hud, hudText(f.unit, k == 2 ? "km" : "m"));
        for (size_t d = 0; d < 4; ++d) {
            navCall(hud, hudText(f.distance, k == 2 ? "1.2" : distances[d]));
            run(hud, port, car, now, 2000);
        }
    }
    navCall(hud, hudShow(f.frame, false));
    navCall(hud, hudEmpty(f.a));
    navCall(hud, hudEmpty(f.b));
    navCall(hud, hudText(f.street, ""));
    navCall(hud, hudText(f.distance, ""));
    navCall(hud, hudText(f.unit, ""));
}

const NavFields kF15 = {0x01, 0x0C, 0x10, 0x0D, 0x04, 0x05, {120, 90}, true};

// The service's log (stdout) caught in a file while a test runs.
class Capture {
public:
    explicit Capture(const std::string& path) : path_(path) {
        fflush(stdout);
        saved_ = dup(1);
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(saved_ >= 0 && fd >= 0 && dup2(fd, 1) == 1);
        close(fd);
    }
    std::string stop() {
        fflush(stdout);
        dup2(saved_, 1);
        close(saved_);
        std::string text;
        FILE* f = fopen(path_.c_str(), "r");
        CHECK(f);
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            text.append(buf, n);
        }
        fclose(f);
        fputs(text.c_str(), stdout);  // still in the test's output
        return text;
    }

private:
    std::string path_;
    int saved_;
};

// The log line holding `what`, from its module tag on (the uptime before it is any number).
std::string lineWith(const std::string& text, const std::string& what) {
    const size_t at = text.find(what);
    CHECK(at != std::string::npos);
    const size_t start = text.rfind("[KombiMost]", at), end = text.find('\n', at);
    CHECK(start != std::string::npos);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

size_t occurrences(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}

bool shadowOn(const FakePort& port, uint8_t fblock) {
    int last = -1;
    for (size_t i = 0; i < port.shadows.size(); ++i) {
        if (((port.shadows[i] >> 8) & 0xFF) == fblock) {
            last = port.shadows[i];
        }
    }
    return last >= 0 && (last & 0x10000);
}

// Every call to the cluster from `from` on names one of these fields, and polygons only go to the pictures.
void onlyFields(const FakePort& port, size_t from, const NavFields& f) {
    const uint16_t allowed[] = {f.frame, f.street, f.distance, f.unit, f.a, f.b};
    for (size_t i = from; i < port.sent.size(); ++i) {
        const Message& m = port.sent[i];
        CHECK(m.out.fblock == kHudFblock && m.data.size() >= 2);
        const uint16_t field = static_cast<uint16_t>((m.data[0] << 8) | m.data[1]);
        bool ok = false;
        for (size_t k = 0; k < 6; ++k) {
            ok = ok || field == allowed[k];
        }
        CHECK(ok);
        CHECK(m.out.fkt != kHudPolygon || field == f.a || field == f.b);
    }
}

// The texts written on the fields from `from` on: the frame goes up before them.
void framedTexts(const FakePort& port, size_t from, const NavFields& f, const char* street, const char* distance,
                 const char* unit) {
    size_t frameAt = port.sent.size(), firstText = port.sent.size();
    bool streetSeen = false, distanceSeen = false, unitSeen = false;
    for (size_t i = from; i < port.sent.size(); ++i) {
        const Message& m = port.sent[i];
        if (m.out.fkt == kHudShow && m.data == hudShow(f.frame, true).data && frameAt == port.sent.size()) {
            frameAt = i;
        }
        if (m.out.fkt == kHudText && firstText == port.sent.size()) {
            firstText = i;
        }
        streetSeen = streetSeen || m.data == hudText(f.street, street).data;
        distanceSeen = distanceSeen || m.data == hudText(f.distance, distance).data;
        unitSeen = unitSeen || m.data == hudText(f.unit, unit).data;
    }
    CHECK(frameAt < firstText && firstText < port.sent.size() && streetSeen && distanceSeen && unitSeen);
}

// The fields of a BMW route, deduced without vmostd: pictures, texts by their kind, frame, size.
void observerDeduces() {
    HudObserver o;
    LearnedLayout l;
    std::string why;
    CHECK(!o.open() && !o.deduce(l, why) && why.find("no pictures") != std::string::npos);
    uint32_t now = 1000;
    navCall(o, hudShow(0x01, true), now);
    navCall(o, hudShow(0x16, false), now);  // a field only hidden is no frame
    HudCalls pic;
    pictureCalls(hudPicture(kRight, 120, 90), 0x04, pic);
    for (size_t i = 0; i < pic.size(); ++i) {
        navCall(o, pic[i], now);
    }
    navCall(o, hudSwap(0x05, 0x04), now);
    // A second pair swapped and drawn on less: an overlay, not the pictures.
    navCall(o, hudSwap(0x08, 0x07), now);
    navCall(o, pic[3], now);  // one polygon on 0x04 again, none on 0x07/0x08
    navCall(o, hudText(0x0C, "Teheran-ro"), now);
    navCall(o, hudText(0x0C, "101"), now);  // a road number alone: the field stays a street by most of its texts
    navCall(o, hudText(0x0C, "Main Street"), now);
    navCall(o, hudText(0x10, "300"), now);
    navCall(o, hudText(0x10, "250"), now);
    navCall(o, hudText(0x0D, "m"), now);
    navCall(o, hudText(0x11, "12:30"), now);  // a clock: no role
    CHECK(o.open() && o.calls() > 10);
    CHECK(o.deduce(l, why));
    CHECK(l.layout.frame == 0x01 && l.layout.street == 0x0C && l.layout.distance == 0x10 && l.layout.unit == 0x0D);
    CHECK(l.layout.pictureB == 0x04 && l.layout.pictureA == 0x05);  // 0x04 was left visible
    CHECK(l.picture.w == 0 && l.picture.h == 0);                    // no size seen: not guessed from the polygons
    o.onSize(0x04, FieldSize{120, 90});
    CHECK(o.deduce(l, why) && l.picture.w == 120 && l.picture.h == 90);
    o.onSize(0x05, FieldSize{100, 90});
    CHECK(!o.deduce(l, why) && why.find("differ") != std::string::npos);
    o.onSize(0x05, FieldSize{120, 90});
    CHECK(o.deduce(l, why));
    // Its description: handles, counts and kinds; never a text, never a coordinate.
    const std::string d = o.describe();
    CHECK(d.find("0x0c") != std::string::npos && d.find("(street)") != std::string::npos &&
          d.find("0x04 swapped 1 polygons") != std::string::npos && d.find("120x90") != std::string::npos);
    CHECK(d.find("Teheran") == std::string::npos && d.find("300") == std::string::npos);
    // Too small for an arrow.
    o.onSize(0x04, FieldSize{60, 40});
    o.onSize(0x05, FieldSize{60, 40});
    CHECK(!o.deduce(l, why) && why.find("too small") != std::string::npos);
    // Without a swap: the two fields drawn on most.
    HudObserver n;
    for (size_t i = 0; i < pic.size(); ++i) {
        navCall(n, pic[i], now);
    }
    CHECK(!n.deduce(l, why));  // one field drawn on: no pair
    HudCalls other;
    pictureCalls(hudPicture(kLeft, 120, 90), 0x09, other);
    for (size_t i = 0; i < other.size(); ++i) {
        navCall(n, other[i], now);
    }
    CHECK(n.deduce(l, why) && l.layout.pictureA == 0x04 && l.layout.pictureB == 0x09 && !l.layout.frame &&
          !l.layout.street);
    // A newer layout with the same pictures keeps what it did not see; other pictures replace everything.
    KeptLayout kept;
    kept.hasLayout = true;
    kept.learned.layout.frame = 0x01;
    kept.learned.layout.street = 0x0C;
    kept.learned.layout.distance = 0x10;
    kept.learned.layout.unit = 0x0D;
    kept.learned.layout.pictureA = 0x05;
    kept.learned.layout.pictureB = 0x04;
    kept.learned.picture = FieldSize{120, 90};
    LearnedLayout fresh;
    fresh.layout.frame = 0x01;
    fresh.layout.street = 0x0C;
    fresh.layout.distance = 0x10;
    fresh.layout.unit = 0;
    fresh.layout.pictureA = 0x04;
    fresh.layout.pictureB = 0x05;
    const LearnedLayout merged = mergeLearned(kept, fresh);
    CHECK(merged.layout.unit == 0x0D && merged.layout.pictureB == 0x05 && merged.picture.w == 120);
    fresh.layout.pictureA = 0x07;
    fresh.layout.pictureB = 0x08;
    CHECK(mergeLearned(kept, fresh) == fresh);
    // The 6WA's own fields, partly seen, are the 6WA.
    HudLayout sixWa, seen;
    seen.unit = 0;
    seen.pictureA = 6;
    seen.pictureB = 5;
    CHECK(within(seen, sixWa));
    seen.street = 0x0C;
    CHECK(!within(seen, sixWa));
}

void keptFile() {
    Place place;
    KeptLayout k;
    k.cluster = 0x0101;
    k.hasLayout = true;
    k.learned.layout.frame = 0x01;
    k.learned.layout.street = 0x0C;
    k.learned.layout.distance = 0x10;
    k.learned.layout.unit = 0;  // not seen: no line
    k.learned.layout.pictureA = 0x05;
    k.learned.layout.pictureB = 0x04;
    k.learned.picture = FieldSize{120, 90};
    const std::string text = keptLayoutText(k);
    CHECK(text.find("cluster=0x0101\n") != std::string::npos && text.find("unit=") == std::string::npos &&
          text.find("pictures=0x05,0x04\n") != std::string::npos && text.find("picture=120x90\n") != std::string::npos);
    KeptLayout back;
    CHECK(parseKeptLayout(text, back) && back.cluster == 0x0101 && back.hasLayout && back.learned == k.learned);
    // Written atomically (no temporary left), read back.
    CHECK(readKeptLayout(place.layout, back) == kKeptNone);
    CHECK(writeKeptLayout(place.layout, k));
    CHECK(access((place.layout + ".new").c_str(), F_OK) != 0);
    KeptLayout read;
    CHECK(readKeptLayout(place.layout, read) == kKeptRead && read.learned == k.learned && read.cluster == 0x0101);
    // The cluster alone; Windows line ends.
    CHECK(parseKeptLayout("cluster=0x0101\r\n", back) && back.cluster == 0x0101 && !back.hasLayout);
    // Anything that does not read as one is refused whole.
    const char* const bad[] = {"",
                               "# nothing\n",
                               "garbage\n",
                               "cluster=0x0101\nframe=0x01\n",           // fields without pictures
                               "pictures=0x04,0x04\n",                   // one picture twice
                               "pictures=0x04,0x05\nstreet=0x04\n",      // a handle in two roles
                               "pictures=0x04,0x0\n",                    // cut short
                               "pictures=0x04\n",                        // one picture
                               "pictures=0x04,0x05\npicture=0x5\n",      // a size without its width
                               "pictures=0x04,0x05\npicture=5000x90\n",  // a size too large
                               "pictures=0x04,0x05\nframe=0x00\n",       // handle 0
                               "pictures=0x04,0x05\nframe=1\n",          // not hex
                               "cluster=0xffff\n",                       // not an address
                               "cluster=0x0101\ncluster=0x0102\n",       // twice
                               "cluster=0x0101\nhud=0x02\n",             // unknown
                               "pictures=0x04,0x05\nstreet=0x0c0c0\n"};  // too long
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(!parseKeptLayout(bad[i], back));
    }
    FILE* f = fopen(place.layout.c_str(), "w");
    CHECK(f);
    fputs("cluster=0x0101\n", f);
    for (int i = 0; i < 400; ++i) {
        fputs("# a long comment line, far more than the file ever holds\n", f);
    }
    fclose(f);
    CHECK(readKeptLayout(place.layout, back) == kKeptBad);
    // A damaged file is ignored: the 6WA's six fields are read as before, and the file is written anew.
    f = fopen(place.layout.c_str(), "w");
    CHECK(f);
    fputs("pictures=0x04,0x", f);
    fclose(f);
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.showing() && countFkt(port, kHudSize) == 6 && hud.fields().pictureA == 5);
    CHECK(readKeptLayout(place.layout, back) == kKeptRead && back.cluster == 0x0101 && !back.hasLayout);
}

// #88: no handle answers a size read. A BMW route teaches the fields its navigation uses (an F15's, as another project
// saw them), with the size the cluster gave it; Android Auto then draws on those, and a next run starts with them.
void learnsFromBmwNavigation() {
    Place place;
    FakePort port;
    Car car = Car::f15();
    uint32_t now = 0;
    {
        MostHud hud(place.config());
        Capture log(std::string(place.dir) + "/log");
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 100000);
        // The 6WA's six fields: no answer, three times; the copies of 82/00 stay, to listen.
        CHECK(hud.phase() == MostHud::kAbsent && countFkt(port, kHudSize) == 18 && countFkt(port, kHudPolygon) == 0);
        CHECK(shadowOn(port, kHudFblock));
        const size_t before = port.sent.size();
        bmwRoute(hud, port, car, now, kF15);
        CHECK(port.sent.size() == before);  // nothing sent while the navigation guides
        run(hud, port, car, now, 31000);
        // The route over: its layout is kept, and tried at once.
        KeptLayout kept;
        CHECK(readKeptLayout(place.layout, kept) == kKeptRead && kept.cluster == 0x0101 && kept.hasLayout);
        const HudLayout& l = kept.learned.layout;
        CHECK(l.frame == 0x01 && l.street == 0x0C && l.distance == 0x10 && l.unit == 0x0D && l.pictureA == 0x05 &&
              l.pictureB == 0x04 && kept.learned.picture.w == 120 && kept.learned.picture.h == 90);
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 15000);
        CHECK(hud.phase() == MostHud::kReady && hud.showing() && place.marked());
        CHECK(countFkt(port, kHudSize, before) == 2 && countFkt(port, kHudPolygon, before) > 0);
        onlyFields(port, before, kF15);
        framedTexts(port, before, kF15, "Rue A", "300", "m");
        // Drawn in the hidden buffer (the navigation left 0x04 visible), inside the size it was given.
        for (size_t i = before; i < port.sent.size(); ++i) {
            const Bytes& d = port.sent[i].data;
            if (port.sent[i].out.fkt == kHudPolygon) {
                CHECK(d[1] == 0x05);
                for (size_t k = 6; k + 3 < d.size(); k += 4) {
                    CHECK(((d[k] << 8) | d[k + 1]) < 120 && ((d[k + 2] << 8) | d[k + 3]) < 90);
                }
            }
        }
        CHECK(countFkt(port, kHudSwap, before) == 1);
        const size_t drawn = port.sent.size();
        port.ackSent = drawn;
        port.ackTarget = &hud;  // the cluster confirms the clearing: the marker goes
        hud.finish(port);
        // Cleared on these fields too: the texts emptied before the frame goes down.
        onlyFields(port, drawn, kF15);
        CHECK(port.sent.back().data == hudShow(0x01, false).data && !place.marked());
        const std::string text = log.stop();
        // One summary of the route: handles, kinds and the size; no street, no distance, anywhere in the log.
        CHECK(occurrences(text, "the BMW navigation on the cluster, ") == 1);
        const std::string summary = lineWith(text, "the BMW navigation on the cluster, ");
        CHECK(summary.find("0x0c texts 4 (street)") != std::string::npos &&
              summary.find("0x10 texts 13 (distance)") != std::string::npos &&
              summary.find("0x0d texts 4 (unit)") != std::string::npos &&
              summary.find("0x04 swapped 3 polygons") != std::string::npos &&
              summary.find("120x90") != std::string::npos && summary.find("(kept)") != std::string::npos);
        for (size_t s = 0; s < sizeof(kNavStreets) / sizeof(kNavStreets[0]); ++s) {
            CHECK(text.find(kNavStreets[s]) == std::string::npos);
        }
        CHECK(summary.find("800") == std::string::npos && summary.find("1.2") == std::string::npos);
    }
    {
        // The next run: the cluster known, listened to from the start; only the learned pictures' sizes read.
        MostHud hud(place.config());
        FakePort next;
        run(hud, next, car, now, 600);
        CHECK(next.sent.empty() && shadowOn(next, kHudFblock));
        tell(hud, 8, "Rue B", 200, "200", kUnitMetres);
        run(hud, next, car, now, 15000);
        CHECK(hud.phase() == MostHud::kReady && hud.showing());
        CHECK(countFkt(next, kHudSize) == 2 && next.sent[1].data == hudSize(0x05).data);
        onlyFields(next, 1, kF15);
        framedTexts(next, 1, kF15, "Rue B", "200", "m");
    }
}

// The copies without the cluster's answers: the learned pictures' sizes are read when Android Auto guides. Without any
// size at all nothing is drawn, the log says so, and the navigation is still listened to.
void learnedWithoutTheirSizes() {
    NavFields f = kF15;
    f.answers = false;
    {
        // Pictures on 0x07/0x08, which answer our size reads.
        NavFields other = f;
        other.a = 0x07;
        other.b = 0x08;
        other.size = FieldSize{150, 100};
        Place place;
        FakePort port;
        Car car = Car::f15();
        car.listed.push_back(std::make_pair(other.a, other.size));
        car.listed.push_back(std::make_pair(other.b, other.size));
        MostHud hud(place.config());
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 100000);
        CHECK(hud.phase() == MostHud::kAbsent);
        const size_t before = port.sent.size();
        bmwRoute(hud, port, car, now, other);
        run(hud, port, car, now, 31000);
        KeptLayout kept;
        CHECK(readKeptLayout(place.layout, kept) == kKeptRead && kept.hasLayout && kept.learned.picture.w == 0 &&
              kept.learned.layout.pictureB == 0x07);
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 3000);
        CHECK(hud.phase() == MostHud::kReady && hud.showing() && countFkt(port, kHudSize, before) == 2);
        onlyFields(port, before, other);
        for (size_t i = before; i < port.sent.size(); ++i) {
            const Bytes& d = port.sent[i].data;
            for (size_t k = 6; port.sent[i].out.fkt == kHudPolygon && k + 3 < d.size(); k += 4) {
                CHECK(((d[k] << 8) | d[k + 1]) < 150 && ((d[k + 2] << 8) | d[k + 3]) < 100);
            }
        }
    }
    {
        Place place;
        FakePort port;
        Car car = Car::f15();
        car.refuseSizes = true;  // and every size read refused
        MostHud hud(place.config());
        Capture log(std::string(place.dir) + "/log");
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 100000);
        CHECK(hud.phase() == MostHud::kAbsent);
        bmwRoute(hud, port, car, now, f);
        run(hud, port, car, now, 31000);
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 15000);
        // The learned pictures read, refused: nothing drawn, and the navigation still listened to.
        CHECK(hud.phase() == MostHud::kAbsent && !hud.showing() && countFkt(port, kHudSize) == 20);
        CHECK(countFkt(port, kHudPolygon) == 0 && countFkt(port, kHudText) == 0 && shadowOn(port, kHudFblock));
        const std::string text = log.stop();
        // The refusals said once, with their first bytes; the counts in the judgement.
        CHECK(occurrences(text, "a size read refused: function 0xc04, operation 0xf, from 0x0101, 3 bytes: 02 00 01") ==
              1);
        CHECK(text.find("(0 of 6 read, 0 late, 6 refused)") != std::string::npos);
        CHECK(text.find("no picture size") != std::string::npos && text.find("size not seen") != std::string::npos);
    }
}

// A 6WA car that saw the BMW navigation on its own six fields draws exactly as before on the next run.
void sixWaStaysSixWa() {
    Place place, fresh;
    uint32_t now = 0;
    {
        MostHud hud(place.config());
        FakePort port;
        Car car;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 7000);  // past the time a copy of one of our calls is still ours
        CHECK(hud.showing());
        NavFields sixWa = {1, 2, 3, 4, 5, 6, {176, 126}, true};
        const size_t before = port.sent.size();
        bmwRoute(hud, port, car, now, sixWa);
        CHECK(port.sent.size() == before && !hud.showing());  // left to it, uncleared
        run(hud, port, car, now, 31000);
        KeptLayout kept;
        CHECK(readKeptLayout(place.layout, kept) == kKeptRead && kept.hasLayout && kept.learned.layout.frame == 1 &&
              kept.learned.layout.street == 2 && kept.learned.layout.distance == 3 && kept.learned.layout.unit == 4);
        port.ackSent = port.sent.size();
        port.ackTarget = &hud;  // the cluster confirms the clearing: no marker left for the next run
        hud.finish(port);
        CHECK(!place.marked());
    }
    // The next run, against a car that never kept anything: the same calls, one for one.
    FakePort a, b;
    Car car;
    uint32_t t = now;
    MostHud learned(place.config()), plain(fresh.config());
    tell(learned, 8, "Rue A", 300, "300", kUnitMetres);
    tell(plain, 8, "Rue A", 300, "300", kUnitMetres);
    run(learned, a, car, now, 2000);
    run(plain, b, car, t, 2000);
    CHECK(learned.showing() && plain.showing() && a.sent.size() == b.sent.size());
    for (size_t i = 0; i < a.sent.size(); ++i) {
        CHECK(a.sent[i].out.fkt == b.sent[i].out.fkt && a.sent[i].data == b.sent[i].data);
    }
}

}  // namespace

int main() {
    registryAndSizes();
    calls();
    pictures();
    views();
    presenter();
    neighbours();
    drawsAndClears();
    pacing();
    notAllowedOrBmw();
    coldCluster();
    lateAnswers();
    silentDrawing();
    headUpDisplay();
    refusedThenNextSession();
    otherClientAndEchoes();
    staleMarker();
    finishClears();
    stoppedBeforeSizesAndHighClock();
    refusedSends();
    textKinds();
    observerDeduces();
    keptFile();
    learnsFromBmwNavigation();
    learnedWithoutTheirSizes();
    sixWaStaysSixWa();
    printf(
        "PASS kombi most: registry, sizes, calls, pictures, views, presenter, neighbours, drawing and clearing, "
        "pacing, Ethernet and BMW navigation first, cold cluster, late answers, silent cluster, head-up display, "
        "refusals, other client, stale marker, exit, text kinds, fields deduced, kept file, fields learned from the "
        "BMW navigation, learned without their sizes, 6WA unchanged\n");
    return 0;
}
