// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The F-series cluster's guidance fields as the BMW navigation uses them: learned from its calls, kept on the car.

// Not every MOST cluster has the 6WA's fields (kombi_most.hpp). On a MINI F56 (#88) none of the handles 1 to 6 answered
// a size read; another project saw an F15 draw its arrow on 0x04/0x05 and its texts on 0x0C/0x10/0x0D, with picture
// buffers that do not answer size reads either. The BMW navigation knows its car's fields: while it guides, the copies
// of its calls on HUD_Terminal (FBlock 0x82) say which handles it uses and how. HudObserver keeps, handle by handle,
// what it did, and deduces:
//   - the pictures: the two handles it swaps (0x20E) or, without a swap, the two it draws polygons on (0x21D); of
//   several
//     pairs swapped, the one drawn on most. The one it left visible is the second (pictureB), as on the 6WA;
//   - the texts (0x210): each text is read in memory to sort its handle, then forgotten. Digits (with ',' '.' '/') are
//   a
//     distance, a short unit (m, km, ft, mi, yd) a unit, anything else a street; a handle takes the kind most of its
//     texts had. A street names a place: no text is ever logged or kept, only the handle's role;
//   - the frame: a handle it shows (0x204) that is neither a picture nor a text, nor swapped, nor drawn on;
//   - the pictures' size: the cluster's answer to a size read (0xC04), to the navigation's when the copies carry the
//     answers, else to ours when Android Auto guides. Never guessed from the polygons.
// Two pictures make a layout; a missing text or frame is a field not written. A layout other than the 6WA's shows its
// frame before writing the texts and hides it after emptying them: the navigation writes its texts under its frame.
//
// The layout and the cluster's address are kept in a small text file on the data partition (a temporary file, then
// renamed), read at the next start: one BMW route is enough. A file that does not read as one is ignored, and written
// anew. A layout learned later replaces it; with the same pictures, a field it did not see keeps the earlier handle.
// Only listening: nothing here sends anything.
#ifndef ENTRY_AAD_PLATFORM_KOMBI_MOST_LAYOUT_HPP
#define ENTRY_AAD_PLATFORM_KOMBI_MOST_LAYOUT_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace evo {
namespace kombi {

enum HudFunction : uint16_t {
    kHudShow = 0x204,     // [field][0|1]
    kHudSwap = 0x20E,     // [field to hide][field to show]
    kHudText = 0x210,     // [field][00][UTF-16BE][00 00]
    kHudLine = 0x219,     // [field][width]
    kHudColour = 0x21B,   // [field][colour]
    kHudPolygon = 0x21D,  // [field][index][03 filled][count][x y]...
    kHudEmpty = 0x220,    // [field]
    kHudSize = 0xC04,     // [field][01] -> [field][..][width][height]
};

// The fields' handles; 0: no such field (a frame or a text, never a picture). As on the 6WA cluster by default.
struct HudLayout {
    uint16_t frame, street, distance, unit, pictureA, pictureB;
    bool frameFirst;  // the frame goes up before the texts are written, down after they are emptied
    HudLayout();
};
struct FieldSize {
    uint16_t w, h;
};
const uint16_t kMinPictureW = 80, kMinPictureH = 60;  // smaller, no arrow fits
bool pictureFits(FieldSize s);

// What a text written into a field looks like.
enum TextKind { kTextEmpty, kTextDistance, kTextUnit, kTextStreet, kTextOther, kTextKinds };
// The kind of the text a text call (0x210) writes: its parameters, [field][a format byte][the characters], read as
// UTF-16BE when they pair up, else one byte each. "-" and a time ("12:30") are kTextOther.
TextKind textKind(const uint8_t* data, size_t len);
const char* textKindName(TextKind kind);

// A layout learned from the BMW navigation.
struct LearnedLayout {
    HudLayout layout;   // frameFirst is not learned: the drawing sets it
    FieldSize picture;  // 0x0: not known
    LearnedLayout();
    bool operator==(const LearnedLayout& o) const;
    bool operator!=(const LearnedLayout& o) const { return !(*this == o); }
};
// The 6WA's own fields, as far as this layout names them: drawn as the 6WA is.
bool within(const HudLayout& learned, const HudLayout& sixWa);
// "frame 0x01, street 0x0c, distance 0x10, unit 0x0d, pictures 0x04/0x05, 120x90" (none for a field not there).
std::string describeLayout(const LearnedLayout& l);

// What another client does with the cluster's fields, from the copies of its calls. Counts and sizes only: a text is
// sorted and forgotten. Single-threaded.
class HudObserver {
public:
    HudObserver();
    // A copy of another client's call on the fields (a request, op <= 8).
    void onCall(uint16_t fkt, const uint8_t* data, size_t len, uint32_t now);
    // A size the cluster gave, to whoever asked; the last one wins.
    void onSize(uint16_t field, FieldSize size);
    // A stretch of calls: from the first call after a quiet spell to the last one. The fields keep what every stretch
    // showed.
    bool open() const { return open_; }
    uint32_t firstAt() const { return firstAt_; }
    uint32_t lastAt() const { return lastAt_; }
    unsigned calls() const { return calls_; }
    void closeStretch() { open_ = false; }
    // The layout every call seen so far shows. False: why says what is missing. A picture size not known is 0x0.
    bool deduce(LearnedLayout& out, std::string& why) const;
    // The fields and what was done with them, for the log: handles, counts, kinds and sizes only.
    std::string describe() const;

private:
    struct Field {
        uint16_t handle;
        unsigned shownOn, shownOff, swapped, polygons, drawn, texts, emptied, sizeReads, created, released, other;
        unsigned kinds[kTextKinds];
    };
    struct Pair {
        uint16_t low, high;
        unsigned swaps;
        uint16_t shown;  // the one the last swap of the two showed
    };
    Field* field(uint16_t handle);
    const Field* find(uint16_t handle) const;
    bool sizeOf(uint16_t handle, FieldSize& size) const;
    TextKind kindOf(const Field& f) const;
    bool inPair(uint16_t handle) const;

    std::vector<Field> fields_;
    std::vector<Pair> pairs_;
    std::vector<std::pair<uint16_t, FieldSize>> sizes_;
    bool open_;
    uint32_t firstAt_, lastAt_;
    unsigned calls_;  // in this stretch
};

// What the file keeps.
struct KeptLayout {
    uint16_t cluster;  // HUD_Terminal's MOST address; 0: not known
    bool hasLayout;
    LearnedLayout learned;
    KeptLayout();
};
// A newer layout over the kept one: the newer wins; with the same two pictures, a field it does not name keeps the
// kept handle (when no other field took it), and an unknown size keeps the kept size.
LearnedLayout mergeLearned(const KeptLayout& kept, const LearnedLayout& fresh);
bool parseKeptLayout(const std::string& text, KeptLayout& out);
std::string keptLayoutText(const KeptLayout& kept);
enum KeptRead { kKeptNone, kKeptRead, kKeptBad };
KeptRead readKeptLayout(const std::string& path, KeptLayout& out);
// Into path + ".new", synced, then renamed over path.
bool writeKeptLayout(const std::string& path, const KeptLayout& kept);

}  // namespace kombi
}  // namespace evo

#endif
