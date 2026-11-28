// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The cluster's guidance fields learned from the BMW navigation's calls, and their file (see kombi_most_layout.hpp).
#include "kombi_most_layout.hpp"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace evo {
namespace kombi {

namespace {

const size_t kMaxFields = 48;  // handles followed; the calls on more are counted, not sorted
const size_t kMaxPairs = 8;
const size_t kMaxSizes = 48;
const size_t kFieldsDescribed = 12;  // the log line stays well within one line
const size_t kTextLooked = 64;       // characters of a text looked at
const size_t kFileMax = 4096;
const uint16_t kMaxSide = 2048;
const uint16_t kCreate = 0x200, kRelease = 0x203;
const char kHeader[] =
    "# The cluster's guidance fields, learned from the BMW navigation (entry-aad, kombi_most_layout.hpp)";

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

bool digit(uint16_t c) {
    return c >= '0' && c <= '9';
}

bool letterOrDigit(uint16_t c) {
    return c >= 0x80 || digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// t[from, to) is a distance's unit, in any case, a full stop after it or not.
bool unitText(const std::vector<uint16_t>& t, size_t from, size_t to) {
    static const char* const units[] = {"m", "km", "ft", "mi", "yd", "yds"};
    if (to > from && t[to - 1] == '.') {
        --to;
    }
    for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); ++u) {
        const size_t n = strlen(units[u]);
        bool same = to - from == n;
        for (size_t i = 0; same && i < n; ++i) {
            const uint16_t c = t[from + i];
            same = (c >= 'A' && c <= 'Z' ? c + 32 : c) == static_cast<uint8_t>(units[u][i]);
        }
        if (same) {
            return true;
        }
    }
    return false;
}

bool sameSize(FieldSize a, FieldSize b) {
    return a.w == b.w && a.h == b.h;
}

bool known(FieldSize s) {
    return s.w != 0 || s.h != 0;
}

void appendf(std::string& out, const char* fmt, unsigned a, unsigned b = 0) {
    char buf[48];
    snprintf(buf, sizeof(buf), fmt, a, b);
    out += buf;
}

// "0x" and one to four hex digits, not 0.
bool parseHandle(const std::string& s, uint16_t& out) {
    if (s.size() < 3 || s.size() > 6 || s[0] != '0' || s[1] != 'x') {
        return false;
    }
    unsigned v = 0;
    for (size_t i = 2; i < s.size(); ++i) {
        const char c = s[i];
        const int d = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
        if (d < 0) {
            return false;
        }
        v = v * 16 + static_cast<unsigned>(d);
    }
    if (v == 0) {
        return false;
    }
    out = static_cast<uint16_t>(v);
    return true;
}

bool parseSide(const std::string& s, uint16_t& out) {
    if (s.empty() || s.size() > 4) {
        return false;
    }
    unsigned v = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!digit(static_cast<uint8_t>(s[i]))) {
            return false;
        }
        v = v * 10 + static_cast<unsigned>(s[i] - '0');
    }
    if (v == 0 || v > kMaxSide) {
        return false;
    }
    out = static_cast<uint16_t>(v);
    return true;
}

// The handles a layout names, pictures first; 0 for a field it does not have.
void handles(const HudLayout& l, uint16_t out[6]) {
    out[0] = l.pictureA;
    out[1] = l.pictureB;
    out[2] = l.frame;
    out[3] = l.street;
    out[4] = l.distance;
    out[5] = l.unit;
}

bool distinct(const HudLayout& l) {
    uint16_t h[6];
    handles(l, h);
    if (h[0] == 0 || h[1] == 0) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        for (int k = i + 1; k < 6; ++k) {
            if (h[i] && h[i] == h[k]) {
                return false;
            }
        }
    }
    return true;
}

bool names(const HudLayout& l, uint16_t handle) {
    uint16_t h[6];
    handles(l, h);
    for (int i = 0; i < 6; ++i) {
        if (h[i] == handle) {
            return true;
        }
    }
    return false;
}

}  // namespace

HudLayout::HudLayout() : frame(1), street(2), distance(3), unit(4), pictureA(5), pictureB(6), frameFirst(false) {}

bool pictureFits(FieldSize s) {
    return s.w >= kMinPictureW && s.h >= kMinPictureH;
}

TextKind textKind(const uint8_t* data, size_t len) {
    if (!data || len < 3) {
        return kTextEmpty;
    }
    // [field][encoding: 00 for UTF-16BE][characters][00 00], as the head unit writes them
    const uint8_t* p = data + 3;
    const size_t n = len - 3;
    const bool wide = data[2] == 0 && n % 2 == 0;
    std::vector<uint16_t> t;
    for (size_t i = 0; wide ? i + 1 < n : i < n; i += wide ? 2 : 1) {
        const uint16_t c = wide ? be16(p + i) : p[i];
        if (c == 0 || t.size() >= kTextLooked) {
            break;
        }
        t.push_back(c);
    }
    size_t a = 0, b = t.size();
    while (a < b && t[a] == ' ') {
        ++a;
    }
    while (b > a && t[b - 1] == ' ') {
        --b;
    }
    bool any = false;
    for (size_t i = a; i < b; ++i) {
        any = any || letterOrDigit(t[i]);
    }
    if (a == b) {
        return kTextEmpty;
    }
    if (!any) {
        return kTextOther;  // "-", "--:--"
    }
    size_t i = a;
    unsigned digits = 0;
    bool colon = false;
    while (i < b && (digit(t[i]) || t[i] == '.' || t[i] == ',' || t[i] == '/' || t[i] == ':')) {
        digits += digit(t[i]) ? 1 : 0;
        colon = colon || t[i] == ':';
        ++i;
    }
    if (digits > 0 && colon) {
        return i == b ? kTextOther : kTextStreet;  // a time
    }
    if (digits > 0) {
        size_t j = i;
        while (j < b && t[j] == ' ') {
            ++j;
        }
        if (i == b || unitText(t, j, b)) {
            return kTextDistance;  // "300", "1,2", "300 m"
        }
        return kTextStreet;  // "3rd Street"
    }
    if (i == a && unitText(t, a, b)) {
        return kTextUnit;
    }
    return kTextStreet;
}

const char* textKindName(TextKind kind) {
    switch (kind) {
        case kTextDistance:
            return "distance";
        case kTextUnit:
            return "unit";
        case kTextStreet:
            return "street";
        case kTextOther:
            return "other";
        default:
            return "empty";
    }
}

LearnedLayout::LearnedLayout() {
    picture.w = 0;
    picture.h = 0;
}

bool LearnedLayout::operator==(const LearnedLayout& o) const {
    uint16_t a[6], b[6];
    handles(layout, a);
    handles(o.layout, b);
    return memcmp(a, b, sizeof(a)) == 0 && sameSize(picture, o.picture);
}

bool within(const HudLayout& learned, const HudLayout& sixWa) {
    const bool pictures = (learned.pictureA == sixWa.pictureA && learned.pictureB == sixWa.pictureB) ||
                          (learned.pictureA == sixWa.pictureB && learned.pictureB == sixWa.pictureA);
    return pictures && (!learned.frame || learned.frame == sixWa.frame) &&
           (!learned.street || learned.street == sixWa.street) &&
           (!learned.distance || learned.distance == sixWa.distance) && (!learned.unit || learned.unit == sixWa.unit);
}

std::string describeLayout(const LearnedLayout& l) {
    std::string out;
    const char* const names[] = {"frame", "street", "distance", "unit"};
    const uint16_t fields[] = {l.layout.frame, l.layout.street, l.layout.distance, l.layout.unit};
    for (int i = 0; i < 4; ++i) {
        out += names[i];
        if (fields[i]) {
            appendf(out, " 0x%02x, ", fields[i]);
        } else {
            out += " none, ";
        }
    }
    appendf(out, "pictures 0x%02x/0x%02x", l.layout.pictureA, l.layout.pictureB);
    if (known(l.picture)) {
        appendf(out, ", %ux%u", l.picture.w, l.picture.h);
    } else {
        out += ", size not known";
    }
    return out;
}

// ---- what another client does with the fields ----

HudObserver::HudObserver() : open_(false), firstAt_(0), lastAt_(0), calls_(0) {}

HudObserver::Field* HudObserver::field(uint16_t handle) {
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].handle == handle) {
            return &fields_[i];
        }
    }
    if (fields_.size() >= kMaxFields) {
        return nullptr;
    }
    Field f;
    memset(&f, 0, sizeof(f));
    f.handle = handle;
    fields_.push_back(f);
    return &fields_.back();
}

const HudObserver::Field* HudObserver::find(uint16_t handle) const {
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].handle == handle) {
            return &fields_[i];
        }
    }
    return nullptr;
}

void HudObserver::onCall(uint16_t fkt, const uint8_t* data, size_t len, uint32_t now) {
    if (!open_) {
        open_ = true;
        firstAt_ = now;
        calls_ = 0;
    }
    lastAt_ = now;
    ++calls_;
    if (!data || len < 2) {
        return;
    }
    const uint16_t handle = be16(data);
    Field* f = field(handle);
    if (!f) {
        return;
    }
    switch (fkt) {
        case kHudShow:
            if (len >= 3) {
                ++(data[2] ? f->shownOn : f->shownOff);
            }
            break;
        case kHudSwap:
            if (len >= 4) {
                const uint16_t shown = be16(data + 2);
                Field* g = field(shown);
                f = field(handle);  // still there: adding g may have moved it
                ++f->swapped;
                if (g && shown != handle) {
                    ++g->swapped;
                    const uint16_t low = std::min(handle, shown), high = std::max(handle, shown);
                    bool found = false;
                    for (size_t i = 0; i < pairs_.size() && !found; ++i) {
                        if (pairs_[i].low == low && pairs_[i].high == high) {
                            ++pairs_[i].swaps;
                            pairs_[i].shown = shown;
                            found = true;
                        }
                    }
                    if (!found && pairs_.size() < kMaxPairs) {
                        const Pair p = {low, high, 1, shown};
                        pairs_.push_back(p);
                    }
                }
            }
            break;
        case kHudText:
            ++f->texts;
            ++f->kinds[textKind(data, len)];
            break;
        case kHudPolygon:
            ++f->polygons;
            break;
        case kHudEmpty:
            ++f->emptied;
            break;
        case kHudSize:
            ++f->sizeReads;
            break;
        case kCreate:
            ++f->created;
            break;
        case kRelease:
            ++f->released;
            break;
        default:
            ++(fkt >= 0x200 && fkt <= 0x22B ? f->drawn : f->other);
            break;
    }
}

void HudObserver::onSize(uint16_t handle, FieldSize size) {
    for (size_t i = 0; i < sizes_.size(); ++i) {
        if (sizes_[i].first == handle) {
            sizes_[i].second = size;
            return;
        }
    }
    if (sizes_.size() < kMaxSizes) {
        sizes_.push_back(std::make_pair(handle, size));
    }
}

bool HudObserver::sizeOf(uint16_t handle, FieldSize& size) const {
    for (size_t i = 0; i < sizes_.size(); ++i) {
        if (sizes_[i].first == handle) {
            size = sizes_[i].second;
            return true;
        }
    }
    return false;
}

// The kind most of the field's texts had: a street on a tie, then a distance.
TextKind HudObserver::kindOf(const Field& f) const {
    const TextKind order[] = {kTextStreet, kTextDistance, kTextUnit};
    TextKind best = kTextEmpty;
    unsigned most = 0;
    for (size_t i = 0; i < 3; ++i) {
        if (f.kinds[order[i]] > most) {
            most = f.kinds[order[i]];
            best = order[i];
        }
    }
    return best;
}

bool HudObserver::inPair(uint16_t handle) const {
    for (size_t i = 0; i < pairs_.size(); ++i) {
        if (pairs_[i].low == handle || pairs_[i].high == handle) {
            return true;
        }
    }
    return false;
}

bool HudObserver::deduce(LearnedLayout& out, std::string& why) const {
    // The pictures: the pair swapped and drawn on most; without a swap, the two fields drawn on most.
    uint16_t low = 0, high = 0, shown = 0;
    unsigned bestDrawn = 0, bestSwaps = 0;
    for (size_t i = 0; i < pairs_.size(); ++i) {
        const Field *a = find(pairs_[i].low), *b = find(pairs_[i].high);
        const unsigned drawn = (a ? a->polygons : 0) + (b ? b->polygons : 0);
        if (!low || drawn > bestDrawn || (drawn == bestDrawn && pairs_[i].swaps > bestSwaps)) {
            low = pairs_[i].low;
            high = pairs_[i].high;
            bestDrawn = drawn;
            bestSwaps = pairs_[i].swaps;
            shown = pairs_[i].shown;
        }
    }
    if (!low) {
        const Field *first = nullptr, *second = nullptr;
        for (size_t i = 0; i < fields_.size(); ++i) {
            const Field* f = &fields_[i];
            if (!f->polygons) {
                continue;
            }
            if (!first || f->polygons > first->polygons ||
                (f->polygons == first->polygons && f->handle < first->handle)) {
                second = first;
                first = f;
            } else if (!second || f->polygons > second->polygons ||
                       (f->polygons == second->polygons && f->handle < second->handle)) {
                second = f;
            }
        }
        if (!second) {
            why = "no pictures: the navigation swapped no two fields, and drew polygons on fewer than two";
            return false;
        }
        low = std::min(first->handle, second->handle);
        high = std::max(first->handle, second->handle);
        shown = high;
    }
    LearnedLayout l;
    l.layout.pictureB = shown;  // the one it left visible
    l.layout.pictureA = l.layout.pictureB == low ? high : low;
    // The texts: for each role, the field whose texts mostly had that kind, the one with most of them.
    uint16_t* roles[] = {&l.layout.distance, &l.layout.unit, &l.layout.street};
    const TextKind kinds[] = {kTextDistance, kTextUnit, kTextStreet};
    for (size_t r = 0; r < 3; ++r) {
        const Field* best = nullptr;
        for (size_t i = 0; i < fields_.size(); ++i) {
            const Field& f = fields_[i];
            if (f.handle == low || f.handle == high || !f.texts || kindOf(f) != kinds[r]) {
                continue;
            }
            if (!best || f.kinds[kinds[r]] > best->kinds[kinds[r]] ||
                (f.kinds[kinds[r]] == best->kinds[kinds[r]] && f.handle < best->handle)) {
                best = &f;
            }
        }
        *roles[r] = best ? best->handle : 0;
    }
    // The frame: shown, and neither a picture nor a text, nor swapped, nor drawn on.
    const Field* frame = nullptr;
    for (size_t i = 0; i < fields_.size(); ++i) {
        const Field& f = fields_[i];
        if (!f.shownOn || f.texts || f.polygons || inPair(f.handle) || f.handle == low || f.handle == high) {
            continue;
        }
        if (!frame || f.shownOn > frame->shownOn || (f.shownOn == frame->shownOn && f.handle < frame->handle)) {
            frame = &f;
        }
    }
    l.layout.frame = frame ? frame->handle : 0;
    // Their size, as the cluster gave it: never guessed from the polygons.
    FieldSize a, b;
    const bool knownA = sizeOf(l.layout.pictureA, a), knownB = sizeOf(l.layout.pictureB, b);
    char text[96];
    if (knownA && knownB && !sameSize(a, b)) {
        snprintf(text, sizeof(text), "the two pictures differ in size (%ux%u and %ux%u)", a.w, a.h, b.w, b.h);
        why = text;
        return false;
    }
    if (knownA || knownB) {
        l.picture = knownA ? a : b;
        if (!pictureFits(l.picture)) {
            snprintf(text, sizeof(text), "pictures %ux%u: too small for an arrow", l.picture.w, l.picture.h);
            why = text;
            return false;
        }
    }
    out = l;
    return true;
}

std::string HudObserver::describe() const {
    std::vector<Field> sorted(fields_);
    std::sort(sorted.begin(), sorted.end(), [](const Field& a, const Field& b) { return a.handle < b.handle; });
    std::string out;
    for (size_t i = 0; i < sorted.size() && i < kFieldsDescribed; ++i) {
        const Field& f = sorted[i];
        if (i) {
            out += "; ";
        }
        appendf(out, "0x%02x", f.handle);
        const unsigned counts[] = {f.shownOn, f.shownOff,  f.swapped, f.polygons, f.drawn,
                                   f.emptied, f.sizeReads, f.created, f.released, f.other};
        const char* const what[] = {" shown %u",   " hidden %u",    " swapped %u", " polygons %u", " drawn %u",
                                    " emptied %u", " size read %u", " created %u", " released %u", " other %u"};
        for (size_t k = 0; k < sizeof(counts) / sizeof(counts[0]); ++k) {
            if (counts[k]) {
                appendf(out, what[k], counts[k]);
            }
        }
        if (f.texts) {
            appendf(out, " texts %u", f.texts);
            out += " (";
            out += textKindName(kindOf(f));
            out += ")";
        }
        FieldSize s;
        if (sizeOf(f.handle, s)) {
            appendf(out, " %ux%u", s.w, s.h);
        }
    }
    if (sorted.size() > kFieldsDescribed) {
        appendf(out, "; %u more fields", static_cast<unsigned>(sorted.size() - kFieldsDescribed));
    }
    return out.empty() ? std::string("no field named") : out;
}

// ---- the file ----

KeptLayout::KeptLayout() : cluster(0), hasLayout(false) {}

LearnedLayout mergeLearned(const KeptLayout& kept, const LearnedLayout& fresh) {
    if (!kept.hasLayout) {
        return fresh;
    }
    const HudLayout& k = kept.learned.layout;
    const bool samePictures = (fresh.layout.pictureA == k.pictureA && fresh.layout.pictureB == k.pictureB) ||
                              (fresh.layout.pictureA == k.pictureB && fresh.layout.pictureB == k.pictureA);
    if (!samePictures) {
        return fresh;
    }
    LearnedLayout m = fresh;
    uint16_t* mine[] = {&m.layout.frame, &m.layout.street, &m.layout.distance, &m.layout.unit};
    const uint16_t theirs[] = {k.frame, k.street, k.distance, k.unit};
    for (int i = 0; i < 4; ++i) {
        if (!*mine[i] && theirs[i] && !names(m.layout, theirs[i])) {
            *mine[i] = theirs[i];
        }
    }
    if (!known(m.picture)) {
        m.picture = kept.learned.picture;
    }
    return m;
}

bool parseKeptLayout(const std::string& text, KeptLayout& out) {
    KeptLayout k;
    const char* const keys[] = {"cluster", "frame", "street", "distance", "unit", "pictures", "picture"};
    const size_t kKeys = sizeof(keys) / sizeof(keys[0]);
    bool seen[kKeys] = {false, false, false, false, false, false, false};
    HudLayout l;
    l.frame = l.street = l.distance = l.unit = l.pictureA = l.pictureB = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        end = end == std::string::npos ? text.size() : end;
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            return false;
        }
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        size_t which = kKeys;
        for (size_t i = 0; i < kKeys; ++i) {
            if (key == keys[i]) {
                which = i;
            }
        }
        if (which == kKeys || seen[which]) {
            return false;  // unknown or twice
        }
        seen[which] = true;
        bool ok = true;
        switch (which) {
            case 0:
                ok = parseHandle(value, k.cluster) && k.cluster != 0xFFFF;
                break;
            case 1:
                ok = parseHandle(value, l.frame);
                break;
            case 2:
                ok = parseHandle(value, l.street);
                break;
            case 3:
                ok = parseHandle(value, l.distance);
                break;
            case 4:
                ok = parseHandle(value, l.unit);
                break;
            case 5: {
                const size_t comma = value.find(',');
                ok = comma != std::string::npos && parseHandle(value.substr(0, comma), l.pictureA) &&
                     parseHandle(value.substr(comma + 1), l.pictureB);
                break;
            }
            default: {
                const size_t x = value.find('x');
                ok = x != std::string::npos && parseSide(value.substr(0, x), k.learned.picture.w) &&
                     parseSide(value.substr(x + 1), k.learned.picture.h);
                break;
            }
        }
        if (!ok) {
            return false;
        }
    }
    const bool layoutKeys = seen[1] || seen[2] || seen[3] || seen[4] || seen[5] || seen[6];
    if (layoutKeys && (!seen[5] || !distinct(l))) {
        return false;  // a layout without its pictures, or a handle named twice
    }
    if (!layoutKeys && !seen[0]) {
        return false;  // nothing in it
    }
    k.hasLayout = layoutKeys;
    l.frameFirst = false;
    k.learned.layout = l;
    out = k;
    return true;
}

std::string keptLayoutText(const KeptLayout& kept) {
    std::string out = kHeader;
    out += '\n';
    if (kept.cluster) {
        appendf(out, "cluster=0x%04x\n", kept.cluster);
    }
    if (kept.hasLayout) {
        const HudLayout& l = kept.learned.layout;
        const char* const keys[] = {"frame=0x%02x\n", "street=0x%02x\n", "distance=0x%02x\n", "unit=0x%02x\n"};
        const uint16_t fields[] = {l.frame, l.street, l.distance, l.unit};
        for (int i = 0; i < 4; ++i) {
            if (fields[i]) {
                appendf(out, keys[i], fields[i]);
            }
        }
        appendf(out, "pictures=0x%02x,0x%02x\n", l.pictureA, l.pictureB);
        if (known(kept.learned.picture)) {
            appendf(out, "picture=%ux%u\n", kept.learned.picture.w, kept.learned.picture.h);
        }
    }
    return out;
}

KeptRead readKeptLayout(const std::string& path, KeptLayout& out) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return errno == ENOENT ? kKeptNone : kKeptBad;
    }
    std::string text;
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0 && text.size() <= kFileMax) {
        text.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    if (n < 0 || text.size() > kFileMax || !parseKeptLayout(text, out)) {
        return kKeptBad;
    }
    return kKeptRead;
}

bool writeKeptLayout(const std::string& path, const KeptLayout& kept) {
    const std::string next = path + ".new", text = keptLayoutText(kept);
    const int fd = open(next.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        done += static_cast<size_t>(n);
    }
    const bool written = done == text.size() && fsync(fd) == 0;
    if (close(fd) != 0 || !written || rename(next.c_str(), path.c_str()) != 0) {
        unlink(next.c_str());
        return false;
    }
    return true;
}

}  // namespace kombi
}  // namespace evo
