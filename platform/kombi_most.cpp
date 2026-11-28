// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The F-series cluster's guidance fields over MOST (see kombi_most.hpp).
#include "kombi_most.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "evo_log.hpp"
#include "kombi_display.hpp"
#include "service_contract.hpp"

namespace evo {
namespace kombi {

namespace {

const char* const kTag = "KombiMost";
const uint32_t kGapMs = 20;  // between two calls, at least
// A cold cluster answers every call 1.1 to 7.5 s late, for minutes after an iDrive start.
const uint32_t kAnswerMs = 10000;    // a call without an answer this long counts as missed
const uint32_t kSlowMs = 1500;       // an answer later than this is slow (logged)
const uint32_t kGivenUpMs = 30000;   // an answer to a call given up on is still recognised this long after the call
const size_t kWaitingMax = 8;        // calls waiting for their answers at once: one at a time takes minutes per arrow
const unsigned kSlowLogged = 3;      // slow or late answers logged (all are counted)
const uint32_t kRegistryMs = 1500;   // the registry's answer
const uint32_t kRetryMs = 30000;     // a registry or sizes that did not answer are asked again this much later
const uint32_t kSizeWaitMs = 10000;  // the size reads' answers, late ones included
const int kSizeTries = 3;            // size reads without usable answers before giving up for this run
const unsigned kRemeasuresLogged = 20;
const uint32_t kForeignMs = 30000;  // another client drew on the fields: they are left to it this long
const uint32_t kEchoMs = 3000;      // our call's copy: while it waits, and this long after it went out or was answered
const int kMissedLimit = 5;         // calls in a row without an answer: the cluster is not there
const int kLookupTries = 3;         // registry requests without an answer before giving up for this run
const size_t kRecentCalls = 32;     // our calls kept to recognise their copies
const size_t kStreetChars = 30;     // the 6WA street field is 272 px wide
const size_t kDistanceChars = 4;    // and the distance field 80 px
const unsigned kManeuversLogged = 100;
// The BMW navigation's calls: a stretch ends after this long without one (its distance changes every few seconds), and
// the layout they show is kept every minute of a long stretch already, the car may be switched off before it ends.
const uint32_t kStretchQuietMs = 30000;
const uint32_t kStretchCheckMs = 60000;
const unsigned kSummariesLogged = 8;  // stretch summaries per run, besides those that changed the kept layout
const size_t kMeasuredMax = 32;       // fields whose size is kept
const size_t kOddLogged = 32;         // kinds of unattached answers logged
const size_t kOddBytes = 8;           // their first bytes
const double kMetresPerMile = 1609.344;
const double kPi = 3.14159265358979323846;

void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

HudCall fieldCall(uint16_t fkt, uint16_t field) {
    HudCall c;
    c.fkt = fkt;
    put16(c.data, field);
    return c;
}

HudCall fieldByteCall(uint16_t fkt, uint16_t field, uint8_t value) {
    HudCall c = fieldCall(fkt, field);
    c.data.push_back(value);
    return c;
}

bool drawingFunction(uint16_t fkt) {
    return (fkt >= 0x200 && fkt <= 0x22B) || (fkt >= 0xC03 && fkt <= 0xC09);
}

// ---- the picture, drawn in a 100 x 100 square (y down), right-hand versions; a left one is its mirror ----

struct P {
    double x, y;
};
using Poly = std::vector<P>;

const double kStroke = 8;     // half the width of an arrow's shaft
const double kHeadHalf = 21;  // half the base of its head

// A shaft from a to b: a rectangle.
Poly shaft(P a, P b, double half = kStroke) {
    const double dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    const double nx = -dy / len * half, ny = dx / len * half;
    Poly p;
    p.push_back(P{a.x + nx, a.y + ny});
    p.push_back(P{b.x + nx, b.y + ny});
    p.push_back(P{b.x - nx, b.y - ny});
    p.push_back(P{a.x - nx, a.y - ny});
    return p;
}

// Rounds the corner where two shafts meet.
Poly joint(P c, double radius = kStroke) {
    Poly p;
    for (int i = 0; i < 8; ++i) {
        const double a = (22.5 + 45.0 * i) * kPi / 180.0;
        p.push_back(P{c.x + radius * std::cos(a), c.y + radius * std::sin(a)});
    }
    return p;
}

// An arrow head: its base centred on base, its point at tip.
Poly head(P base, P tip, double half = kHeadHalf) {
    const double dx = tip.x - base.x, dy = tip.y - base.y, len = std::sqrt(dx * dx + dy * dy);
    const double nx = -dy / len * half, ny = dx / len * half;
    Poly p;
    p.push_back(tip);
    p.push_back(P{base.x - nx, base.y - ny});
    p.push_back(P{base.x + nx, base.y + ny});
    return p;
}

// A piece of the roundabout's ring, between two angles (degrees).
Poly ringPiece(P c, double inner, double outer, double from, double to) {
    const double a = from * kPi / 180.0, b = to * kPi / 180.0;
    Poly p;
    p.push_back(P{c.x + outer * std::cos(a), c.y + outer * std::sin(a)});
    p.push_back(P{c.x + outer * std::cos(b), c.y + outer * std::sin(b)});
    p.push_back(P{c.x + inner * std::cos(b), c.y + inner * std::sin(b)});
    p.push_back(P{c.x + inner * std::cos(a), c.y + inner * std::sin(a)});
    return p;
}

struct Piece {
    uint8_t colour;
    Poly poly;
};
using Glyph = std::vector<Piece>;

void add(Glyph& g, const Poly& p, uint8_t colour = kHudWhite) {
    g.push_back(Piece{colour, p});
}

Glyph straight() {
    Glyph g;
    add(g, shaft(P{50, 94}, P{50, 38}));
    add(g, head(P{50, 38}, P{50, 6}));
    return g;
}

Glyph turn() {  // 90 degrees to the right
    Glyph g;
    add(g, shaft(P{34, 94}, P{34, 50}));
    add(g, joint(P{34, 50}));
    add(g, shaft(P{34, 50}, P{64, 50}));
    add(g, head(P{64, 50}, P{94, 50}));
    return g;
}

Glyph halfTurn() {  // 45 degrees to the right: keep, fork, ramps, lane change
    Glyph g;
    add(g, shaft(P{38, 94}, P{38, 60}));
    add(g, joint(P{38, 60}));
    add(g, shaft(P{38, 60}, P{60, 38}));
    add(g, head(P{60, 38}, P{82, 16}));
    return g;
}

Glyph tightTurn() {  // 135 degrees to the right
    Glyph g;
    add(g, shaft(P{32, 94}, P{32, 30}));
    add(g, joint(P{32, 30}));
    add(g, shaft(P{32, 30}, P{56, 54}));
    add(g, head(P{56, 54}, P{78, 76}));
    return g;
}

Glyph uTurn() {  // up on the left, back down on the right
    Glyph g;
    add(g, shaft(P{32, 94}, P{32, 34}));
    add(g, joint(P{32, 34}));
    add(g, shaft(P{32, 34}, P{66, 34}));
    add(g, joint(P{66, 34}));
    add(g, shaft(P{66, 34}, P{66, 56}));
    add(g, head(P{66, 56}, P{66, 86}, 19));
    return g;
}

Glyph destination() {  // a map pin
    Glyph g;
    add(g, joint(P{50, 34}, 24));
    Poly tail;
    tail.push_back(P{31, 46});
    tail.push_back(P{69, 46});
    tail.push_back(P{50, 92});
    add(g, tail);
    add(g, joint(P{50, 34}, 9), 0);  // black: the pin's hole
    return g;
}

Glyph roundabout() {  // grey ring, entry from below, exit ahead
    Glyph g;
    const P centre = {50, 46};
    for (int i = 0; i < 8; ++i) {
        add(g, ringPiece(centre, 14, 23, 45.0 * i, 45.0 * (i + 1)), kHudGrey);
    }
    add(g, shaft(P{50, 94}, P{50, 66}, 7));
    add(g, head(P{50, 25}, P{50, 3}, 17));
    return g;
}

// The glyph for a direction and whether it is mirrored.
bool glyphFor(int direction, Glyph& g, bool& left) {
    left = false;
    switch (direction) {
        case kStraightOn:
            g = straight();
            return true;
        case kHalfLeft:
        case kHighwayInLeft:
        case kHighwayOutLeft:
        case kLaneChangeLeft:
        case kForkLeft:
            left = true;
            g = halfTurn();
            return true;
        case kHalfRight:
        case kHighwayInRight:
        case kHighwayOutRight:
        case kLaneChangeRight:
        case kForkRight:
            g = halfTurn();
            return true;
        case kTightLeft:
            left = true;
            g = tightTurn();
            return true;
        case kTightRight:
            g = tightTurn();
            return true;
        case kLeft:
            left = true;
            g = turn();
            return true;
        case kRight:
            g = turn();
            return true;
        case kUTurnLeft:
            left = true;
            g = uTurn();
            return true;
        case kUTurnRight:
            g = uTurn();
            return true;
        case kDestination:
            g = destination();
            return true;
        case kRoundaboutClockwise:
        case kRoundaboutCounterClockwise:
            g = roundabout();
            return true;
        default:
            return false;
    }
}

double clampTo(double v, double hi) {
    return v < 0 ? 0 : v > hi ? hi : v;
}

}  // namespace

bool layoutFits(const FieldSize sizes[kHudFields], std::string* why) {
    const FieldSize &frame = sizes[0], &street = sizes[1], &distance = sizes[2], &unit = sizes[3];
    const FieldSize &a = sizes[4], &b = sizes[5];
    const char* problem = nullptr;
    if (a.w == 0 || a.h == 0 || frame.w == 0 || frame.h == 0) {
        problem = "a field is 0x0: they were never set up (the BMW navigation does it)";
    } else if (a.w != b.w || a.h != b.h) {
        problem = "the two picture buffers differ";
    } else if (a.w < kMinPictureW || a.h < kMinPictureH || a.w > 32767 || a.h > 32767) {
        problem = "picture size unusable for an arrow";
    } else if (street.w < 60 || street.h < 12 || distance.w < 24 || distance.h < 12 || unit.w < 12 || unit.h < 12) {
        problem = "a text field too small";
    }
    if (problem && why) {
        *why = problem;
    }
    return !problem;
}

bool registryAddress(const uint8_t* data, size_t len, uint8_t fblock, uint8_t inst, uint16_t* address) {
    for (size_t i = 0; data && i + 4 <= len; i += 4) {
        if (data[i + 2] == fblock && data[i + 3] == inst) {
            *address = get16(data + i);
            return true;
        }
    }
    return false;
}

bool fieldSize(const uint8_t* data, size_t len, uint16_t* field, FieldSize* size) {
    if (!data || len < 6) {
        return false;
    }
    *field = get16(data);
    size->w = get16(data + len - 4);
    size->h = get16(data + len - 2);
    return true;
}

HudCall hudShow(uint16_t field, bool on) {
    return fieldByteCall(kHudShow, field, on ? 1 : 0);
}

HudCall hudSwap(uint16_t hide, uint16_t show) {
    HudCall c = fieldCall(kHudSwap, hide);
    put16(c.data, show);
    return c;
}

HudCall hudText(uint16_t field, const std::string& utf8) {
    HudCall c = fieldByteCall(kHudText, field, 0);
    for (size_t i = 0; i < utf8.size();) {
        const uint8_t b = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = '?';
        size_t n = 1;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            n = 2;
        } else if ((b & 0xF0) == 0xE0) {
            n = 3;
        } else if ((b & 0xF8) == 0xF0) {
            n = 4;
        }
        if (n > 1) {
            bool ok = i + n <= utf8.size();
            uint32_t v = b & (0x7F >> n);
            for (size_t k = 1; ok && k < n; ++k) {
                const uint8_t t = static_cast<uint8_t>(utf8[i + k]);
                ok = (t & 0xC0) == 0x80;
                v = (v << 6) | (t & 0x3F);
            }
            cp = ok && v >= 0x80 && v <= 0xFFFF && (v < 0xD800 || v > 0xDFFF) ? v : '?';
            if (!ok) {
                n = 1;
            }
        }
        put16(c.data, static_cast<uint16_t>(cp));
        i += n;
    }
    put16(c.data, 0);
    return c;
}

HudCall hudEmpty(uint16_t field) {
    return fieldCall(kHudEmpty, field);
}

HudCall hudSize(uint16_t field) {
    return fieldByteCall(kHudSize, field, 1);
}

std::vector<HudShape> hudPicture(int direction, uint16_t w, uint16_t h) {
    std::vector<HudShape> shapes;
    Glyph g;
    bool left;
    if (!glyphFor(direction, g, left) || w < 2 || h < 2) {
        return shapes;
    }
    // The part of the field left clear of the shade on the right, as seen on the 6WA (176 x 126: x 42-162, y 6-124).
    const double x0 = w * 0.24, x1 = w * 0.92, y0 = h * 0.05, y1 = h * 0.98;
    const double side = std::min(x1 - x0, y1 - y0);
    const double ox = x0 + (x1 - x0 - side) / 2, oy = y0 + (y1 - y0 - side) / 2;
    for (size_t i = 0; i < g.size(); ++i) {
        HudShape s;
        s.colour = g[i].colour;
        for (size_t k = 0; k < g[i].poly.size(); ++k) {
            const double x = left ? 100.0 - g[i].poly[k].x : g[i].poly[k].x;
            const double y = g[i].poly[k].y;
            HudPoint p;
            p.x = static_cast<int16_t>(std::lround(clampTo(ox + x * side / 100.0, w - 1)));
            p.y = static_cast<int16_t>(std::lround(clampTo(oy + y * side / 100.0, h - 1)));
            s.points.push_back(p);
        }
        // One winding for every polygon, clockwise on the screen, as the stock arrows are listed (a mirror reverses).
        long area = 0;
        for (size_t k = 0; k < s.points.size(); ++k) {
            const HudPoint &a = s.points[k], &b = s.points[(k + 1) % s.points.size()];
            area += static_cast<long>(a.x) * b.y - static_cast<long>(b.x) * a.y;
        }
        if (area < 0) {
            std::reverse(s.points.begin(), s.points.end());
        }
        shapes.push_back(s);
    }
    return shapes;
}

bool convex(const std::vector<HudPoint>& points) {
    const size_t n = points.size();
    if (n < 3) {
        return false;
    }
    int sign = 0;
    for (size_t i = 0; i < n; ++i) {
        const HudPoint &a = points[i], &b = points[(i + 1) % n], &c = points[(i + 2) % n];
        const long cross = static_cast<long>(b.x - a.x) * (c.y - b.y) - static_cast<long>(b.y - a.y) * (c.x - b.x);
        const int s = cross > 0 ? 1 : cross < 0 ? -1 : 0;
        if (s && sign && s != sign) {
            return false;
        }
        sign = s ? s : sign;
    }
    return sign != 0;
}

void pictureCalls(const std::vector<HudShape>& shapes, uint16_t buffer, HudCalls& out) {
    out.push_back(hudEmpty(buffer));
    out.push_back(fieldByteCall(kHudLine, buffer, 0));
    int colour = -1;
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].colour != colour) {
            colour = shapes[i].colour;
            out.push_back(fieldByteCall(kHudColour, buffer, shapes[i].colour));
        }
        HudCall c = fieldCall(kHudPolygon, buffer);
        put16(c.data, static_cast<uint16_t>(i));
        c.data.push_back(3);  // filled
        c.data.push_back(static_cast<uint8_t>(shapes[i].points.size()));
        for (size_t k = 0; k < shapes[i].points.size(); ++k) {
            put16(c.data, static_cast<uint16_t>(shapes[i].points[k].x));
            put16(c.data, static_cast<uint16_t>(shapes[i].points[k].y));
        }
        out.push_back(c);
    }
}

void viewCalls(const HudView& view, const HudView* shown, const HudLayout& layout, FieldSize picture, uint16_t& visible,
               HudCalls& out) {
    if (!shown || shown->direction != view.direction) {
        const uint16_t hidden = visible == layout.pictureA ? layout.pictureB : layout.pictureA;
        pictureCalls(hudPicture(view.direction, picture.w, picture.h), hidden, out);
        out.push_back(hudSwap(visible, hidden));
        out.push_back(hudEmpty(visible));
        visible = hidden;
    }
    const uint16_t texts[] = {layout.street, layout.distance, layout.unit};
    const std::string* values[] = {&view.street, &view.distance, &view.unit};
    const std::string* before[] = {shown ? &shown->street : nullptr, shown ? &shown->distance : nullptr,
                                   shown ? &shown->unit : nullptr};
    if (!shown && layout.frame && layout.frameFirst) {
        out.push_back(hudShow(layout.frame, true));
    }
    for (int i = 0; i < 3 && !shown; ++i) {
        if (texts[i]) {
            out.push_back(hudShow(texts[i], true));
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (texts[i] && (!shown || *before[i] != *values[i])) {
            out.push_back(hudText(texts[i], *values[i]));
        }
    }
    if (!shown && layout.frame && !layout.frameFirst) {
        out.push_back(hudShow(layout.frame, true));
    }
}

void clearCalls(const HudLayout& layout, HudCalls& out) {
    if (layout.frame && !layout.frameFirst) {
        out.push_back(hudShow(layout.frame, false));
    }
    out.push_back(hudEmpty(layout.pictureA));
    out.push_back(hudEmpty(layout.pictureB));
    const uint16_t texts[] = {layout.street, layout.distance, layout.unit};
    for (int i = 0; i < 3; ++i) {
        if (texts[i]) {
            out.push_back(hudText(texts[i], ""));
        }
    }
    if (layout.frame && layout.frameFirst) {
        out.push_back(hudShow(layout.frame, false));
    }
}

// ---- what to show ----

HudPresenter::HudPresenter(const GuideSettings& settings)
    : settings_(settings),
      showing_(false),
      gapOn_(false),
      noDistanceOn_(false),
      gapAt_(0),
      noDistanceAt_(0),
      distanceAt_(0) {}

void HudPresenter::reset() {
    showing_ = false;
    gapOn_ = false;
    noDistanceOn_ = false;
}

bool HudPresenter::step(const Maneuver* next, double now, HudView& view) {
    if (!next) {
        if (!showing_) {
            return false;
        }
        if (!gapOn_) {
            gapOn_ = true;
            gapAt_ = now;
        }
        if (now - gapAt_ >= settings_.graceS) {
            reset();
            return false;
        }
        view = view_;
        return true;
    }
    gapOn_ = false;
    Maneuver m = *next;
    double metres;
    if (settings_.approachMiles > 0 && metresToManeuver(m, metres)) {
        const bool same = showing_ && m.sameTurn(turn_);
        if (metres > settings_.approachMiles * kMetresPerMile * (same ? settings_.hysteresis : 1.0)) {
            reset();
            return false;
        }
    }
    const bool sameTurn = showing_ && m.sameTurn(turn_);
    if (m.distanceText.empty() && sameTurn && !turn_.distanceText.empty()) {
        m.distanceText = turn_.distanceText;  // keep the last distance
        m.distanceUnit = turn_.distanceUnit;
        m.hasMetres = turn_.hasMetres;
        m.metres = turn_.metres;
    }
    HudView v;
    v.direction = clusterDirection(m.type);
    if (m.distanceText.empty()) {
        if (!noDistanceOn_) {
            noDistanceOn_ = true;
            noDistanceAt_ = now;
        }
        if (!showing_ && now - noDistanceAt_ < settings_.firstDistanceS) {
            return false;
        }
        v.distance = "-";
    } else {
        noDistanceOn_ = false;
        std::string text;
        int unit = m.distanceUnit;
        if (clusterUnit(m.distanceUnit) == kNoClusterUnit && m.hasMetres) {
            metric(static_cast<double>(m.metres), text, unit);  // a unit the phone did not give: metric
            m.distanceText = text;
            m.distanceUnit = unit;
        }
        shortDistance(m, kDistanceChars, text, unit);
        v.distance = text;
        if (text != "-" && clusterUnit(unit) != kNoClusterUnit) {
            v.unit = clusterUnitName(clusterUnit(unit));
        }
    }
    const int exit = roundaboutExit(m);
    v.street = m.road;
    if (exit > 0) {
        char label[24];
        snprintf(label, sizeof(label), "Exit %d", exit);
        v.street = m.road.empty() ? std::string(label) : std::string(label) + " - " + m.road;
    }
    v.street = utf8Prefix(v.street, kStreetChars);
    // The distance changes at most every distanceEveryS on the same maneuver.
    if (showing_ && v.direction == view_.direction && v.street == view_.street && v.distance != view_.distance &&
        now - distanceAt_ < settings_.distanceEveryS) {
        v.distance = view_.distance;
        v.unit = view_.unit;
    }
    if (!showing_ || v.distance != view_.distance) {
        distanceAt_ = now;
    }
    view_ = v;
    turn_ = m;
    showing_ = true;
    view = v;
    return true;
}

// ---- the Ethernet KOMBI's neighbour entry ----

bool arpHasNeighbour(const std::string& table, const std::string& ip) {
    size_t pos = table.find('\n');  // past the header line
    while (pos != std::string::npos && pos + 1 < table.size()) {
        const size_t end = table.find('\n', pos + 1);
        const std::string line = table.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
        pos = end;
        char addr[64], type[16], flags[16], mac[32];
        if (sscanf(line.c_str(), "%63s %15s %15s %31s", addr, type, flags, mac) != 4 || ip != addr) {
            continue;
        }
        const unsigned long f = strtoul(flags, nullptr, 16);
        if ((f & 0x2) && strcmp(mac, "00:00:00:00:00:00") != 0) {
            return true;
        }
    }
    return false;
}

bool ethernetNeighbour(const std::string& ip) {
    FILE* f = fopen("/proc/net/arp", "r");
    if (!f) {
        return false;
    }
    std::string table;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && table.size() < 65536) {
        table.append(buf, n);
    }
    fclose(f);
    return arpHasNeighbour(table, ip);
}

// ---- the calls, on the controller reader's thread ----

MostHudConfig::MostHudConfig()
    : markerFile(evo::contract::kKombiMostShown),
      offFile(evo::contract::kKombiOff),
      layoutFile(evo::contract::kKombiMostLayout),
      clusterAddress("160.48.199.96"),
      ethernetFirstS(5.0) {}

MostHud::MostHud(const MostHudConfig& config)
    : config_(config),
      messages_(0),
      generation_(0),
      bmw_(false),
      ethernet_(false),
      fieldsReady_(false),
      clock_(0),
      started_(false),
      startedAt_(0),
      inputsAt_(0),
      settingAt_(0),
      arpAt_(0),
      messageAt_(0),
      wantSince_(0),
      seenMessages_(0),
      seenGeneration_(0),
      inboxRevision_(0),
      guiding_(false),
      settingOn_(false),
      arpSeen_(false),
      ethernetLogged_(false),
      allowed_(false),
      bmwNow_(false),
      has_(false),
      wantOn_(false),
      presenter_(config.guide),
      phase_(kIdle),
      layoutVerified_(false),
      shadowed_(false),
      address_(0xFFFF),
      lookupTries_(0),
      lookupAt_(0),
      retryAt_(0),
      retryArmed_(false),
      registryIn_(false),
      trial_(kTry6wa),
      layout_(config.layout),
      sizesWaiting_(false),
      sizesAskedAt_(0),
      sizeTries_(0),
      sizeRefusals_(0),
      learnedReady_(false),
      learnedTried_(false),
      sixWaFailed_(false),
      listening_(false),
      watchingEarly_(false),
      stretchChecks_(0),
      summaries_(0),
      remeasureDue_(false),
      remeasuring_(false),
      haveMovedTo_(false),
      remeasureAt_(0),
      remeasures_(0),
      lateSizes_(0),
      batch_(kNoBatch),
      batchFailed_(false),
      serial_(0),
      refused_(false),
      refusals_(0),
      slowAnswers_(0),
      lateAnswers_(0),
      lastSendAt_(0),
      sentOnce_(false),
      missed_(0),
      foreignUntil_(0),
      foreignActive_(false),
      foreignLogged_(false),
      shown_(false),
      unsure_(false),
      marked_(false),
      staleMarker_(false),
      firstAcked_(false),
      visible_(config.layout.pictureB),  // the stock navigation leaves the second buffer visible
      drawn_(0) {
    pthread_mutex_init(&mu_, nullptr);
    memset(&keptPicture_, 0, sizeof(keptPicture_));
    memset(&drawnPicture_, 0, sizeof(drawnPicture_));
    memset(&pendingPicture_, 0, sizeof(pendingPicture_));
    staleMarker_ = access(config_.markerFile.c_str(), F_OK) == 0;
    marked_ = staleMarker_;
}

MostHud::~MostHud() {
    pthread_mutex_destroy(&mu_);
}

void MostHud::onNavStatus(uint16_t id, const uint8_t* body, size_t size, void* hud) {
    MostHud* h = static_cast<MostHud*>(hud);
    if (!h) {
        return;
    }
    pthread_mutex_lock(&h->mu_);
    if (h->inbox_.keep(id, body ? body : reinterpret_cast<const uint8_t*>(""), body ? size : 0)) {
        ++h->messages_;
    }
    pthread_mutex_unlock(&h->mu_);
}

void MostHud::onSessionEnded() {
    pthread_mutex_lock(&mu_);
    inbox_.clear();
    ++generation_;
    pthread_mutex_unlock(&mu_);
}

// The setting, the Ethernet KOMBI, the BMW navigation and the phone's next maneuver, every 250 ms.
void MostHud::readInputs(uint32_t now) {
    if (started_ && now - inputsAt_ < 250) {
        return;
    }
    inputsAt_ = now;
    if (!started_) {
        started_ = true;
        startedAt_ = now;
        settingAt_ = now - 1000;
        arpAt_ = now - 10000;
        messageAt_ = now;
        loadKept();
    }
    // The messages are copied and read again only when one came in, not on every look.
    pthread_mutex_lock(&mu_);
    const bool newMessages = inbox_.revision() != inboxRevision_;
    NavInbox inbox;
    if (newMessages) {
        inbox = inbox_;
        inboxRevision_ = inbox_.revision();
    }
    const unsigned messages = messages_, generation = generation_;
    pthread_mutex_unlock(&mu_);
    if (newMessages) {
        guiding_ = inbox.guiding() && inbox.nextManeuver(next_);
    }
    if (generation != seenGeneration_) {
        seenGeneration_ = generation;
        presenter_.reset();
        if (phase_ == kStopped) {
            missed_ = 0;
            // The last session's calls no longer count against this one.
            givenUp_.insert(givenUp_.end(), waiting_.begin(), waiting_.end());
            waiting_.clear();
            if (layoutVerified_) {
                phase_ = kReady;
                ELOG(kTag, "new Android Auto session: the cluster may be drawn on again");
            } else {
                phase_ = kSizes;
                sizeTries_ = 0;
                retryArmed_ = false;
                askSizes(false);
                ELOG(kTag, "new Android Auto session: reading the cluster's fields' sizes again");
            }
        }
    }
    if (messages != seenMessages_) {
        seenMessages_ = messages;
        messageAt_ = now;
    }
    if (now - settingAt_ >= 1000) {
        settingAt_ = now;
        settingOn_ = access(config_.offFile.c_str(), F_OK) != 0;
    }
    const bool ethernet = ethernet_;
    if (ethernet && !ethernetLogged_) {
        ethernetLogged_ = true;
        ELOG(kTag, "the Ethernet KOMBI took an arrow: the MOST cluster is left alone");
    }
    if (!ethernet && now - arpAt_ >= 10000) {
        arpAt_ = now;
        const bool seen = ethernetNeighbour(config_.clusterAddress);
        if (seen != arpSeen_) {
            ELOG(kTag, "KOMBI %s %s the neighbour table: the MOST cluster is %s", config_.clusterAddress.c_str(),
                 seen ? "in" : "no longer in", seen ? "left alone" : "allowed again");
        }
        arpSeen_ = seen;
    }
    allowed_ = settingOn_ && !ethernet && !arpSeen_;
    bmwNow_ = bmw_;
    const Maneuver* shown = nullptr;
    if (guiding_ && now - messageAt_ <= config_.guide.staleS * 1000) {
        shown = &next_;
    }
    if (allowed_ && !bmwNow_) {
        has_ = presenter_.step(shown, now / 1000.0, view_);
    } else {
        presenter_.reset();
        has_ = false;
    }
    // The MOST registry only after the guidance wanted a picture for a while: the Ethernet KOMBI's chance first.
    const bool wanting = (has_ || staleMarker_) && allowed_ && !bmwNow_;
    if (wanting && !wantOn_) {
        wantSince_ = now;
    }
    wantOn_ = wanting;
}

bool MostHud::wants(const VmostMsgView& m) const {
    return m.fblock == kHudFblock || (m.fblock == kRegistryFblock && m.fkt == kRegistryFkt) ||
           (m.fblock == kGuidanceFblock && m.fkt == kGuidanceMovedFkt);
}

namespace {

bool sameCall(const HudCall& c, const VmostMsgView& m) {
    return c.fkt == m.fkt && c.data.size() == m.len && (m.len == 0 || memcmp(c.data.data(), m.data, m.len) == 0);
}

}  // namespace

bool MostHud::ours(const VmostMsgView& m, uint32_t now) const {
    for (size_t i = 0; i < waiting_.size(); ++i) {
        if (sameCall(waiting_[i].call, m)) {
            return true;
        }
    }
    for (size_t i = 0; i < recent_.size(); ++i) {
        const Sent& s = recent_[i];
        if (sameCall(s.call, m) && (now - s.at <= kEchoMs || (s.answered && now - s.answeredAt <= kEchoMs))) {
            return true;
        }
    }
    return false;
}

// The oldest call of the answer's function, of its field when the answer starts with one of theirs.
bool MostHud::takeAnswer(std::deque<Waiting>& list, const VmostMsgView& m, Waiting& out) {
    size_t pick = list.size();
    for (size_t i = 0; i < list.size(); ++i) {
        const HudCall& c = list[i].call;
        if (c.fkt != m.fkt) {
            continue;
        }
        if (m.len >= 2 && c.data.size() >= 2 && memcmp(c.data.data(), m.data, 2) == 0) {
            pick = i;
            break;
        }
        if (pick == list.size()) {
            pick = i;
        }
    }
    if (pick == list.size()) {
        return false;
    }
    out = list[pick];
    list.erase(list.begin() + static_cast<long>(pick));
    return true;
}

// An answer that belongs to none of our calls, once per function and operation: its first bytes, a text's never.
void MostHud::oddAnswer(const VmostMsgView& m, const char* what) {
    const uint32_t kind = (static_cast<uint32_t>(m.fkt) << 8) | (m.op & 0xFF);
    if (std::find(oddLogged_.begin(), oddLogged_.end(), kind) != oddLogged_.end() || oddLogged_.size() >= kOddLogged) {
        return;
    }
    oddLogged_.push_back(kind);
    const size_t shown = std::min<size_t>(m.data ? m.len : 0, m.fkt == kHudText ? 2 : kOddBytes);
    char bytes[3 * kOddBytes + 1] = "";
    for (size_t i = 0; i < shown; ++i) {
        snprintf(bytes + 3 * i, 4, " %02x", m.data[i]);
    }
    ELOG(kTag, "%s: function 0x%03x, operation 0x%x, from 0x%04x, %u bytes%s%s%s (once per function and operation)",
         what, m.fkt, m.op, m.addr, m.len, shown ? ":" : "", bytes,
         m.fkt == kHudText && m.len > shown ? " (its text not logged)"
         : m.len > shown                    ? " ..."
                                            : "");
}

// An answer to one of our drawing calls, waiting or given up on; any other (another client's, an unexpected one) is
// only logged.
void MostHud::onAnswer(const VmostMsgView& m, uint32_t now) {
    Waiting w;
    bool late = false;
    if (!takeAnswer(waiting_, m, w)) {
        if (!takeAnswer(givenUp_, m, w)) {
            oddAnswer(m, "an answer to no call of ours");
            return;
        }
        late = true;
    }
    for (size_t i = 0; i < recent_.size(); ++i) {
        if (recent_[i].serial == w.serial) {
            recent_[i].answered = true;
            recent_[i].answeredAt = now;
        }
    }
    const unsigned after = static_cast<unsigned>(now - w.at);
    if (late) {
        if (++lateAnswers_ <= kSlowLogged) {
            ELOG(kTag, "the cluster answered function 0x%03x after %u ms, once given up on (%u such answers)", m.fkt,
                 after, lateAnswers_);
        }
    } else if (after > kSlowMs && ++slowAnswers_ <= kSlowLogged) {
        ELOG(kTag,
             "the cluster answered function 0x%03x after %u ms: slow, as a cold cluster is for minutes after an "
             "iDrive start (%u slow answers)",
             m.fkt, after, slowAnswers_);
    }
    if (m.op == kMostOpErrorAck || m.op == kMostOpError) {
        refused_ = true;
        ++refusals_;
        ELOG(kTag, "the cluster refused function 0x%03x (operation 0x%x, error %02x %02x)", m.fkt, m.op,
             m.len > 0 ? m.data[0] : 0, m.len > 1 ? m.data[1] : 0);
    } else {
        missed_ = 0;  // it is there
    }
}

// Waiting calls without an answer for 10 s are missed; the answers still to come are recognised for a while.
void MostHud::expire(uint32_t now) {
    while (!waiting_.empty() && now - waiting_.front().at >= kAnswerMs) {
        const Waiting w = waiting_.front();
        waiting_.pop_front();
        givenUp_.push_back(w);
        if (++missed_ == 1) {
            ELOG(kTag, "no answer to function 0x%03x within %u s", w.call.fkt, kAnswerMs / 1000);
        }
    }
    while (!givenUp_.empty() && (now - givenUp_.front().at >= kGivenUpMs || givenUp_.size() > kRecentCalls)) {
        givenUp_.pop_front();
    }
}

void MostHud::onMessage(const VmostMsgView& m) {
    const uint32_t now = clock_;
    if (m.fblock == kRegistryFblock) {
        if (phase_ == kLookup && m.fkt == kRegistryFkt && m.op == kMostOpStatus) {
            registry_.assign(m.data, m.data + m.len);
            registryIn_ = true;
        }
        return;
    }
    if (m.fblock == kGuidanceFblock) {
        onGuidanceMoved(m);
        return;
    }
    if (m.inst != kHudInstance) {
        return;
    }
    if (m.fkt == kHudSize && (m.op == kMostOpResultAck || m.op == kMostOpStatus)) {
        // A size is taken whenever it comes, whoever asked: a cold cluster answers seconds late, and the BMW
        // navigation's sizes are the ones its fields have.
        uint16_t field;
        FieldSize size;
        if (fieldSize(m.data, m.len, &field, &size) && field >= 1) {
            observer_.onSize(field, size);
            Measured* s = measure(field);
            if (s) {
                s->size = size;
                s->in = true;
                s->ever = true;
                if (s->askedAt && now - s->askedAt > kSlowMs) {
                    ++lateSizes_;
                }
            }
        } else {
            oddAnswer(m, "a size answer too short to read");
        }
        return;
    }
    if (m.op <= 8) {  // a request: a copy of ours, or another client drawing (the BMW navigation)
        if (ours(m, now)) {
            return;
        }
        observer_.onCall(m.fkt, m.data, m.len, now);
        if (drawingFunction(m.fkt)) {
            if (!foreignLogged_) {
                ELOG(kTag,
                     "another client draws on the cluster (address 0x%04x, function 0x%03x): Android Auto's picture "
                     "gives way",
                     m.addr, m.fkt);
                foreignLogged_ = true;
            }
            foreignUntil_ = now + kForeignMs;
            foreignActive_ = true;
        }
        return;
    }
    if (m.fkt == kHudSize && (m.op == kMostOpErrorAck || m.op == kMostOpError)) {
        // Size reads are not waited for: a refusal is counted for the judgement, and said once.
        ++sizeRefusals_;
        oddAnswer(m, "a size read refused");
        return;
    }
    if (m.op == kMostOpResultAck || m.op == kMostOpStatus || m.op == kMostOpErrorAck || m.op == kMostOpError) {
        onAnswer(m, now);
    } else {
        oddAnswer(m, "an answer of an unexpected kind");
    }
}

// After a call left unanswered, one at a time until an answer comes back.
size_t MostHud::room() const {
    const size_t most = missed_ > 0 ? 1 : kWaitingMax;
    return waiting_.size() < most ? most - waiting_.size() : 0;
}

int MostHud::dueInMs(uint32_t nowMs) const {
    if (refused_ || (batch_ != kNoBatch && queue_.empty() && waiting_.empty())) {
        return 0;
    }
    int32_t due = 100;
    if (!queue_.empty() && room() > 0) {
        due = sentOnce_ ? std::min(due, static_cast<int32_t>(lastSendAt_ + kGapMs - nowMs)) : 0;
    }
    if (!waiting_.empty()) {
        due = std::min(due, static_cast<int32_t>(waiting_.front().at + kAnswerMs - nowMs));
    }
    return due > 0 ? due : 0;
}

void MostHud::tick(uint32_t now, VmostPort& port) {
    clock_ = now;
    readInputs(now);
    expire(now);
    learn(now);
    // Size reads are not waited for: a size is taken whenever it comes, and checkSizes() judges them.
    if (refused_) {
        refused_ = false;
        batchFailed_ = true;
        queue_.clear();
        unsure_ = true;  // part of a picture may be up, or not cleared
        stop("the cluster refused a call");
    } else if (missed_ >= kMissedLimit) {
        batchFailed_ = true;
        queue_.clear();
        stop("five calls in a row without an answer");
    }
    if (!queue_.empty()) {
        if (room() > 0 && (!sentOnce_ || static_cast<int32_t>(now - lastSendAt_) >= static_cast<int32_t>(kGapMs))) {
            sendNext(now, port);
        }
        return;
    }
    if (!waiting_.empty()) {
        return;  // the batch's last answers
    }
    if (batch_ != kNoBatch) {
        endBatch();
        if (phase_ == kAbsent && !listening_) {
            unshadow(port);
        }
    }
    switch (phase_) {
        case kIdle:
            if (wantOn_ && now - wantSince_ >= static_cast<uint32_t>(config_.ethernetFirstS * 1000) &&
                (!retryArmed_ || static_cast<int32_t>(now - retryAt_) >= 0)) {
                retryArmed_ = false;
                watchingEarly_ = false;
                startLookup(now, port);
            } else if (!shadowed_ && kept_.cluster && allowed_) {
                // A car whose cluster is known: its fields are listened to from the start, for the BMW navigation.
                shadowAll(port);
                watchingEarly_ = true;
                ELOG(kTag, "HUD_Terminal 82/00 at 0x%04x on an earlier run: the BMW navigation's calls are listened to",
                     kept_.cluster);
            } else if (watchingEarly_ && !allowed_) {
                unshadow(port);
            }
            break;
        case kLookup:
            if (registryIn_) {
                onRegistry(now);
                if (phase_ == kAbsent) {
                    unshadow(port);
                }
            } else if (now - lookupAt_ >= kRegistryMs) {
                if (++lookupTries_ >= kLookupTries) {
                    ELOG(kTag, "the MOST registry never answered: no Android Auto picture on the cluster this run");
                    phase_ = kAbsent;
                    unshadow(port);
                } else {
                    ELOG(kTag, "the MOST registry did not answer: asked again in %u s", kRetryMs / 1000);
                    phase_ = kIdle;
                    retryAt_ = now + kRetryMs;
                    retryArmed_ = true;
                }
            }
            break;
        case kSizes:
            checkSizes(now);
            if (phase_ == kAbsent && !listening_) {
                unshadow(port);
            }
            break;
        case kReady:
            decide(now);
            break;
        case kAbsent:
            // Fields learned while listening: tried once the navigation has left them.
            if (listening_ && learnedReady_ && !learnedTried_ && !giveWay(now)) {
                tryLearned();
            }
            break;
        case kStopped:
            break;
    }
}

void MostHud::shadowAll(VmostPort& port) {
    if (shadowed_) {
        return;
    }
    const int a = port.shadow(kRegistryFblock, kRegistryInstance, true);
    const int b = port.shadow(kHudFblock, kHudInstance, true);
    const int c = port.shadow(kGuidanceFblock, kGuidanceInstance, true);
    ELOG(kTag,
         "copies of the MOST registry, the cluster's guidance fields (82/00) and the navigation (52/00): %s (%d), "
         "%s (%d), %s (%d)",
         a == 0 ? "OK" : "refused", a, b == 0 ? "OK" : "refused", b, c == 0 ? "OK" : "refused", c);
    shadowed_ = true;
}

// The BMW navigation guides, or another client drew on the fields within 30 s: nothing is sent to the cluster.
bool MostHud::giveWay(uint32_t now) const {
    return bmwNow_ || static_cast<int32_t>(foreignUntil_ - now) > 0;
}

void MostHud::startLookup(uint32_t now, VmostPort& port) {
    shadowAll(port);
    static const uint8_t none[4] = {0, 0, 0, 0};
    VmostMsgOut m;
    memset(&m, 0, sizeof(m));
    m.addr = kRegistryAddress;
    m.fblock = kRegistryFblock;
    m.inst = kRegistryInstance;
    m.fkt = kRegistryFkt;
    m.op = kMostOpGet;
    m.data = none;
    const int rc = port.send(m);
    ELOG(kTag, "looking for the cluster's guidance fields (HUD_Terminal 82/00) in the MOST registry: request %s (%d)",
         rc == 0 ? "sent" : "refused", rc);
    registryIn_ = false;
    registry_.clear();
    lookupAt_ = now;
    phase_ = kLookup;
}

void MostHud::onRegistry(uint32_t now) {
    (void)now;
    const unsigned devices = static_cast<unsigned>(registry_.size() / 4);
    uint16_t address;
    if (!registryAddress(registry_.data(), registry_.size(), kHudFblock, kHudInstance, &address)) {
        ELOG(kTag, "MOST registry: %u entries, no HUD_Terminal (82/00): no MOST cluster to draw on", devices);
        phase_ = kAbsent;
        if (staleMarker_) {
            mark(false);
            staleMarker_ = false;
        }
        return;
    }
    address_ = address;
    uint16_t hud;
    const bool separateHud = registryAddress(registry_.data(), registry_.size(), kHudFblock, 0x02, &hud);
    ELOG(kTag, "MOST registry: %u entries; HUD_Terminal 82/00 at 0x%04x%s: reading its fields' sizes", devices,
         address_, separateHud ? " (a separate head-up display 82/02 too, left alone)" : "");
    if (kept_.cluster != address_) {
        kept_.cluster = address_;  // the next runs listen to the BMW navigation from the start
        keep();
    }
    if (learnedReady_ && !learnedTried_) {
        tryLearned();
    } else {
        try6wa();
    }
}

// The 6WA's fields: all six sizes read.
void MostHud::try6wa() {
    trial_ = kTry6wa;
    layout_ = config_.layout;
    layoutVerified_ = false;  // another set of fields: not drawn on, nothing to clear
    const HudLayout& l = config_.layout;
    const uint16_t fields[kHudFields] = {l.frame, l.street, l.distance, l.unit, l.pictureA, l.pictureB};
    reads_.assign(fields, fields + kHudFields);
    sizeTries_ = 0;
    sizeRefusals_ = 0;
    listening_ = false;
    askSizes(false);
    phase_ = kSizes;
}

// The fields learned from the BMW navigation: the pictures' sizes read, the texts written under the frame.
void MostHud::tryLearned() {
    learnedTried_ = true;
    trial_ = kTryLearned;
    layoutVerified_ = false;
    layout_ = kept_.learned.layout;
    layout_.frameFirst = true;
    keptPicture_ = kept_.learned.picture;
    reads_.clear();
    reads_.push_back(layout_.pictureA);
    reads_.push_back(layout_.pictureB);
    sizeTries_ = 0;
    sizeRefusals_ = 0;
    listening_ = false;
    ELOG(kTag, "fields learned from the BMW navigation (%s): reading the pictures' sizes",
         describeLayout(kept_.learned).c_str());
    askSizes(false);
    phase_ = kSizes;
}

MostHud::Measured* MostHud::measure(uint16_t field) {
    for (size_t i = 0; i < measured_.size(); ++i) {
        if (measured_[i].field == field) {
            return &measured_[i];
        }
    }
    if (measured_.size() >= kMeasuredMax) {
        return nullptr;
    }
    Measured m;
    memset(&m, 0, sizeof(m));
    m.field = field;
    measured_.push_back(m);
    return &measured_.back();
}

const MostHud::Measured* MostHud::measured(uint16_t field) const {
    for (size_t i = 0; i < measured_.size(); ++i) {
        if (measured_[i].field == field) {
            return &measured_[i];
        }
    }
    return nullptr;
}

// The first picture's size as the cluster last gave it; the learned one while it never did.
FieldSize MostHud::picture() const {
    const Measured* a = measured(layout_.pictureA);
    return a && a->ever ? a->size : keptPicture_;
}

// The size reads, of every field tried or of those not answered yet.
void MostHud::askSizes(bool missingOnly) {
    HudCalls calls;
    for (size_t i = 0; i < reads_.size(); ++i) {
        Measured* s = measure(reads_[i]);
        if (s && (!missingOnly || !s->in)) {
            s->in = false;
            calls.push_back(hudSize(reads_[i]));
        }
    }
    sizesWaiting_ = false;
    queue(kSizesBatch, calls);
}

bool MostHud::sizesComplete() const {
    for (size_t i = 0; i < reads_.size(); ++i) {
        const Measured* s = measured(reads_[i]);
        if (!s || !s->in) {
            return false;
        }
    }
    return true;
}

// After the reads: every size in, or 10 s gone, judges them; a judged failure asks again 30 s later, unless the fields
// are left to the BMW navigation by then. Fields learned meanwhile are tried instead of the 6WA's.
void MostHud::checkSizes(uint32_t now) {
    if (batch_ == kSizesBatch) {
        return;  // still asking
    }
    if (sizesWaiting_ && (sizesComplete() || now - sizesAskedAt_ >= kSizeWaitMs)) {
        sizesWaiting_ = false;
        onSizes(now);
    } else if (!sizesWaiting_ && (!retryArmed_ || static_cast<int32_t>(now - retryAt_) >= 0) && !giveWay(now)) {
        retryArmed_ = false;
        if (trial_ == kTry6wa && learnedReady_ && !learnedTried_) {
            tryLearned();
        } else {
            askSizes(!sizesComplete());  // unusable sizes: all again; missing ones: those only
        }
    }
}

void MostHud::onSizes(uint32_t now) {
    if (trial_ == kTryLearned) {
        onLearnedSizes();
        return;
    }
    FieldSize s[kHudFields];
    unsigned in = 0;
    for (size_t i = 0; i < kHudFields; ++i) {
        const Measured* m = i < reads_.size() ? measured(reads_[i]) : nullptr;
        s[i] = m ? m->size : FieldSize{0, 0};
        in += m && m->in ? 1 : 0;
    }
    ELOG(kTag,
         "fields: frame %ux%u, street %ux%u, distance %ux%u, unit %ux%u, pictures %ux%u and %ux%u (%u of %u read, "
         "%u late, %u refused)",
         s[0].w, s[0].h, s[1].w, s[1].h, s[2].w, s[2].h, s[3].w, s[3].h, s[4].w, s[4].h, s[5].w, s[5].h, in,
         static_cast<unsigned>(kHudFields), lateSizes_, sizeRefusals_);
    std::string why;
    if (!sizesComplete()) {
        why = "a size was not given";
    } else if (!layoutFits(s, &why)) {
        why = why.empty() ? "unknown" : why;
    }
    if (why.empty()) {
        ELOG(kTag, "guidance fields ready: Android Auto's next maneuver goes to the cluster when the phone guides");
        layoutVerified_ = true;
        fieldsReady_ = true;
        visible_ = layout_.pictureB;
        phase_ = kReady;
        return;
    }
    if (++sizeTries_ < kSizeTries) {
        ELOG(kTag, "these fields are not the ones drawn here yet (%s): asked again in %u s", why.c_str(),
             kRetryMs / 1000);
        retryAt_ = now + kRetryMs;
        retryArmed_ = true;
        return;
    }
    sixWaFailed_ = true;
    if (learnedReady_ && !learnedTried_) {
        ELOG(kTag, "these fields are not the ones drawn here (%s, %d tries)", why.c_str(), sizeTries_);
        tryLearned();
        return;
    }
    ELOG(kTag,
         "these fields are not the ones drawn here (%s, %d tries): no Android Auto picture on the cluster; the BMW "
         "navigation's calls are listened to, a BMW route teaches its fields",
         why.c_str(), sizeTries_);
    phase_ = kAbsent;
    listening_ = true;
}

// The learned pictures: the size they answer, else the one the cluster gave the BMW navigation. Never a guess.
void MostHud::onLearnedSizes() {
    const Measured *a = measured(layout_.pictureA), *b = measured(layout_.pictureB);
    const bool inA = a && a->in, inB = b && b->in;
    FieldSize size = {0, 0};
    const char* from = "";
    char why[128] = "";
    if (inA && inB && (a->size.w != b->size.w || a->size.h != b->size.h)) {
        snprintf(why, sizeof(why), "the two pictures answered %ux%u and %ux%u", a->size.w, a->size.h, b->size.w,
                 b->size.h);
    } else if (inA || inB) {
        size = inA ? a->size : b->size;
        from = "as answered";
    } else if (keptPicture_.w || keptPicture_.h) {
        size = keptPicture_;
        from = "the one the cluster gave the BMW navigation";
    } else {
        snprintf(why, sizeof(why),
                 "no picture size: the cluster answered neither the BMW navigation's size reads nor ours");
    }
    if (!why[0] && !pictureFits(size)) {
        snprintf(why, sizeof(why), "pictures %ux%u: too small for an arrow", size.w, size.h);
    }
    ELOG(kTag, "learned pictures 0x%02x and 0x%02x: %u of 2 size reads answered, %u refused; size %ux%u%s%s%s",
         layout_.pictureA, layout_.pictureB, static_cast<unsigned>((inA ? 1 : 0) + (inB ? 1 : 0)), sizeRefusals_,
         size.w, size.h, from[0] ? " (" : "", from, from[0] ? ")" : "");
    if (!why[0]) {
        keptPicture_ = size;
        visible_ = layout_.pictureB;
        layoutVerified_ = true;
        fieldsReady_ = true;
        phase_ = kReady;
        ELOG(kTag,
             "guidance fields ready, as the BMW navigation uses them: Android Auto's next maneuver goes to the cluster "
             "when the phone guides");
        return;
    }
    if (!sixWaFailed_) {
        ELOG(kTag, "the fields learned from the BMW navigation are not usable (%s): the 6WA's are tried", why);
        try6wa();
        return;
    }
    ELOG(kTag,
         "the fields learned from the BMW navigation are not usable (%s): no Android Auto picture on the cluster; "
         "its calls are still listened to",
         why);
    layout_ = config_.layout;
    phase_ = kAbsent;
    listening_ = true;
}

// The cluster says the guidance moved between itself and the HUD: both picture sizes are read again, once per new place
// it names (it may repeat the same one many times a second). Only its own message: the navigation answers on the same
// function.
void MostHud::onGuidanceMoved(const VmostMsgView& m) {
    if (m.fkt != kGuidanceMovedFkt || m.inst != kGuidanceInstance || m.addr != address_ || !layoutVerified_ ||
        (phase_ != kReady && phase_ != kStopped)) {
        return;
    }
    if (haveMovedTo_ && movedTo_.size() == m.len && (m.len == 0 || memcmp(movedTo_.data(), m.data, m.len) == 0)) {
        return;
    }
    movedTo_.assign(m.data, m.data + m.len);
    haveMovedTo_ = true;
    remeasureDue_ = true;
}

void MostHud::decide(uint32_t now) {
    const bool has = has_, allowed = allowed_, bmw = bmwNow_;
    const HudView& view = view_;
    if (foreignActive_ && static_cast<int32_t>(foreignUntil_ - now) <= 0) {
        foreignActive_ = false;
    }
    const bool foreign = foreignActive_;
    if (!foreign) {
        foreignLogged_ = false;
    }
    if (bmw || foreign) {
        if (shown_ || unsure_ || staleMarker_) {
            ELOG(kTag, "the BMW navigation uses the cluster: Android Auto's picture left to it, not cleared");
            shown_ = false;
            unsure_ = false;
            staleMarker_ = false;
            mark(false);
        }
        return;
    }
    HudCalls calls;
    const uint16_t a = layout_.pictureA, b = layout_.pictureB;
    if (remeasureDue_) {
        remeasureDue_ = false;
        if (++remeasures_ <= kRemeasuresLogged) {
            ELOG(kTag, "the guidance moved between the cluster and the head-up display: picture size read again");
        }
        Measured* sa = measure(a);
        if (sa) {
            sa->in = false;
        }
        Measured* sb = measure(b);
        if (sb) {
            sb->in = false;
        }
        remeasuring_ = true;
        remeasureAt_ = now;
        calls.push_back(hudSize(a));
        calls.push_back(hudSize(b));
        queue(kRemeasureBatch, calls);
        return;
    }
    if (remeasuring_) {
        // Drawn at the new size once both are in; at the old one if they never come.
        const Measured *ia = measured(a), *ib = measured(b);
        if (!(ia && ia->in && ib && ib->in) && now - remeasureAt_ < kSizeWaitMs) {
            return;
        }
        remeasuring_ = false;
    }
    if (unsure_ || staleMarker_) {
        ELOG(kTag, "clearing a picture possibly left on the cluster (%s)",
             staleMarker_ ? "by a previous run" : "a call was refused");
        clearCalls(layout_, calls);
        queue(kClearBatch, calls);
        return;
    }
    const FieldSize picture = this->picture();
    const bool fits = pictureFits(picture);
    if (!has || !allowed || !fits) {
        if (shown_) {
            ELOG(kTag, "picture cleared (%s)",
                 !allowed ? "arrows off, or an Ethernet KOMBI"
                 : !has   ? "no maneuver to show"
                          : "picture field too small for an arrow");
            clearCalls(layout_, calls);
            queue(kClearBatch, calls);
        }
        return;
    }
    const bool resized = shown_ && (picture.w != drawnPicture_.w || picture.h != drawnPicture_.h);
    if (shown_ && view == shownView_ && !resized) {
        return;
    }
    HudView shown = shownView_;
    if (resized) {
        ELOG(kTag, "picture field now %ux%u (was %ux%u): arrow drawn again", picture.w, picture.h, drawnPicture_.w,
             drawnPicture_.h);
        shown.direction = -1;  // a new picture, the texts as they are
    }
    uint16_t visible = visible_;
    viewCalls(view, shown_ ? &shown : nullptr, layout_, picture, visible, calls);
    if (calls.empty()) {
        shownView_ = view;
        return;
    }
    if ((!shown_ || view.direction != shownView_.direction) && drawn_ < kManeuversLogged) {
        ELOG(kTag, "maneuver: direction %d, %s %s, street of %u byte(s)", view.direction, view.distance.c_str(),
             view.unit.c_str(), static_cast<unsigned>(view.street.size()));  // no street name in the log
    }
    mark(true);
    visible_ = visible;
    pending_ = view;
    pendingPicture_ = picture;
    queue(kDrawBatch, calls);
}

void MostHud::queue(Batch batch, const HudCalls& calls) {
    batch_ = batch;
    batchFailed_ = false;
    queue_.insert(queue_.end(), calls.begin(), calls.end());
}

void MostHud::sendNext(uint32_t now, VmostPort& port) {
    const HudCall call = queue_.front();
    queue_.pop_front();
    VmostMsgOut m;
    memset(&m, 0, sizeof(m));
    m.addr = address_;
    m.fblock = kHudFblock;
    m.inst = kHudInstance;
    m.fkt = call.fkt;
    m.op = kMostOpStartResultAck;
    m.len = static_cast<uint16_t>(call.data.size());
    m.data = call.data.data();
    const unsigned serial = ++serial_;
    recent_.push_back(Sent{call, now, serial, false, 0});
    while (recent_.size() > kRecentCalls) {
        recent_.pop_front();
    }
    lastSendAt_ = now;
    sentOnce_ = true;
    const int rc = port.send(m);
    if (rc == 0) {
        if (call.fkt == kHudSize) {
            // Read, not waited for: its answer is taken whenever it comes.
            Measured* s = call.data.size() >= 2 ? measure(get16(call.data.data())) : nullptr;
            if (s) {
                s->askedAt = now ? now : 1;  // 0 is "never asked"
            }
        } else {
            waiting_.push_back(Waiting{call, now, serial});
        }
    } else {
        ELOG(kTag, "function 0x%03x not handed to vmostd (%d)", call.fkt, rc);
        if (++missed_ >= kMissedLimit) {
            batchFailed_ = true;
            queue_.clear();
            stop("vmostd refused five calls in a row");
        }
    }
}

void MostHud::endBatch() {
    const Batch batch = batch_;
    const bool failed = batchFailed_;
    batch_ = kNoBatch;
    batchFailed_ = false;
    switch (batch) {
        case kSizesBatch:
            sizesWaiting_ = true;  // checkSizes() takes it from here
            sizesAskedAt_ = clock_;
            break;
        case kRemeasureBatch:
            break;  // the answers come in whenever (onMessage)
        case kDrawBatch:
            if (failed) {
                unsure_ = true;
                break;
            }
            shown_ = true;
            shownView_ = pending_;
            drawnPicture_ = pendingPicture_;
            ++drawn_;
            if (!firstAcked_) {
                // Every call answered is all that is known: whether the picture shows is not.
                firstAcked_ = true;
                ELOG(kTag, "first picture sent to the cluster, no call refused (%ux%u)", drawnPicture_.w,
                     drawnPicture_.h);
            }
            break;
        case kClearBatch:
            if (!failed) {
                shown_ = false;
                unsure_ = false;
                staleMarker_ = false;
                mark(false);
            }
            break;
        case kNoBatch:
            break;
    }
}

void MostHud::stop(const char* why) {
    if (phase_ != kStopped && phase_ != kAbsent) {
        ELOG(kTag, "%s: no Android Auto picture on the cluster until the next Android Auto session", why);
        phase_ = kStopped;
    }
}

void MostHud::mark(bool up) {
    if (up == marked_) {
        return;
    }
    if (up) {
        const int fd = open(config_.markerFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0) {
            close(fd);
        }
    } else {
        unlink(config_.markerFile.c_str());
    }
    marked_ = up;
}

void MostHud::unshadow(VmostPort& port) {
    if (shadowed_) {
        port.shadow(kGuidanceFblock, kGuidanceInstance, false);
        port.shadow(kHudFblock, kHudInstance, false);
        port.shadow(kRegistryFblock, kRegistryInstance, false);
        shadowed_ = false;
    }
    listening_ = false;
    watchingEarly_ = false;
}

// ---- the fields the BMW navigation uses ----

void MostHud::loadKept() {
    KeptLayout kept;
    switch (readKeptLayout(config_.layoutFile, kept)) {
        case kKeptNone:
            return;
        case kKeptBad:
            ELOG(kTag, "%s does not read as the cluster's fields: ignored (written anew once they are learned)",
                 config_.layoutFile.c_str());
            return;
        case kKeptRead:
            break;
    }
    kept_ = kept;
    learnedReady_ = kept_.hasLayout && !within(kept_.learned.layout, config_.layout);
    if (kept_.hasLayout) {
        ELOG(kTag, "fields learned from the BMW navigation on an earlier run (%s)%s",
             describeLayout(kept_.learned).c_str(), learnedReady_ ? "" : ": the 6WA's, drawn as such");
    }
}

void MostHud::keep() {
    if (!writeKeptLayout(config_.layoutFile, kept_)) {
        ELOG(kTag, "%s could not be written: what was learned holds for this run only", config_.layoutFile.c_str());
    }
}

// A stretch of the BMW navigation's calls: summed up once it ends (30 s without a call), or the service stops; the
// layout they show is kept as soon as it changes, checked every minute of a long stretch.
void MostHud::learn(uint32_t now, bool closing) {
    if (!observer_.open()) {
        return;
    }
    const bool ended = closing || now - observer_.lastAt() >= kStretchQuietMs;
    const bool due = now - observer_.firstAt() >= kStretchCheckMs * (stretchChecks_ + 1);
    if (!ended && !due) {
        return;
    }
    if (due) {
        ++stretchChecks_;
    }
    LearnedLayout fresh;
    std::string why;
    const bool usable = observer_.deduce(fresh, why);
    LearnedLayout merged = usable ? mergeLearned(kept_, fresh) : fresh;
    const bool changed = usable && (!kept_.hasLayout || kept_.learned != merged);
    if (changed) {
        kept_.hasLayout = true;
        kept_.learned = merged;
        if (address_ != 0xFFFF) {
            kept_.cluster = address_;
        }
        keep();
        const bool sixWa = within(merged.layout, config_.layout);
        learnedReady_ = !sixWa;
        learnedTried_ = false;
    }
    if (changed || (ended && summaries_ < kSummariesLogged)) {
        ++summaries_;
        std::string outcome;
        if (!usable) {
            outcome = "nothing learned: " + why;
        } else {
            outcome = "learned: " + describeLayout(merged);
            outcome += changed ? (within(merged.layout, config_.layout) ? " (the 6WA's; kept)" : " (kept)")
                               : " (already kept)";
            if (!merged.picture.w && !merged.picture.h) {
                outcome += "; size not seen: read when Android Auto guides";
            }
        }
        ELOG(kTag, "the BMW navigation on the cluster%s, %u calls in %u s: %s; %s", ended ? "" : " (still guiding)",
             observer_.calls(), static_cast<unsigned>((observer_.lastAt() - observer_.firstAt()) / 1000),
             observer_.describe().c_str(), outcome.c_str());
    }
    if (ended) {
        observer_.closeStretch();
        stretchChecks_ = 0;
    }
}

void MostHud::finish(VmostPort& port) {
    const bool bmw = bmw_;
    const bool mayBeUp = shown_ || unsure_ || batch_ == kDrawBatch;
    learn(clock_, true);
    queue_.clear();
    givenUp_.insert(givenUp_.end(), waiting_.begin(), waiting_.end());  // not waited for any more
    waiting_.clear();
    if (layoutVerified_ && (phase_ == kReady || phase_ == kStopped) && mayBeUp && !bmw) {
        HudCalls calls;
        clearCalls(layout_, calls);
        const unsigned refusalsBefore = refusals_;
        unsigned failed = 0;
        for (size_t i = 0; i < calls.size(); ++i) {
            const HudCall& call = calls[i];
            VmostMsgOut m;
            memset(&m, 0, sizeof(m));
            m.addr = address_;
            m.fblock = kHudFblock;
            m.inst = kHudInstance;
            m.fkt = call.fkt;
            m.op = kMostOpStartResultAck;
            m.len = static_cast<uint16_t>(call.data.size());
            m.data = call.data.data();
            if (port.send(m) != 0) {
                ++failed;
            } else {
                waiting_.push_back(Waiting{call, clock_, ++serial_});  // a refusal in the pause below is seen
            }
            port.pump(static_cast<int>(kGapMs));
        }
        // No longer wait for every answer: a cold cluster answers seconds late (#177), and the stop of Android Auto
        // waits on this thread. Sent without a refusal is cleared.
        waiting_.clear();
        const bool refusedOrFailed = failed > 0 || refusals_ != refusalsBefore;
        ELOG(kTag, "service stopping: picture cleared%s",
             refusedOrFailed ? " (not every call taken: the marker stays)" : "");
        if (!refusedOrFailed) {
            mark(false);
        }
    }
    unshadow(port);
}

}  // namespace kombi
}  // namespace evo
