// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's touchscreen (UDP 30567) to Android Auto touch events.

// Observed on the car: HmiMain3D (CTouchDataClient) listens on 0.0.0.0:30567; there is no evdev touch device.
// Reading: a read-only AF_PACKET socket with a BPF filter on UDP 30567 copies each datagram, taking nothing away from
// iDrive and never sending. Enough to observe and decode.
// Forward mode: only while the Android Auto picture is on screen, a UDP 30567 socket (SO_REUSEADDR/SO_REUSEPORT) is
// connected to the touch sender (TouchSource). The kernel prefers a connected socket (exact match), so the hidden
// iDrive stops receiving the touches. The socket is closed as soon as the picture goes away, or on stop, and iDrive
// gets the touchscreen back. When the bind is refused, only the copy is used.
#ifndef ENTRY_AAD_PLATFORM_EVO_TOUCH_HPP
#define ENTRY_AAD_PLATFORM_EVO_TOUCH_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "aa_input.hpp"
#include "evo_touch_frame.hpp"
#include "input_mode.hpp"

namespace evo {

// IPv4 packet starting at the network header (AF_PACKET SOCK_DGRAM). Fragments are refused.
struct UdpView {
    uint32_t src;
    uint16_t sport;
    uint16_t dport;
    const uint8_t* data;
    size_t len;
};
bool parseIpv4Udp(const uint8_t* p, size_t n, UdpView& out);
bool isTouchPdu(const uint8_t* b, size_t n);
// A Wi-Fi interface by its name: the car's access point (wlan0), its Wi-Fi Direct group (wlan1, p2p-*). The
// touchscreen is never behind one.
bool isWifiInterface(const char* name);

// Which copied datagrams to UDP 30567 are the touchscreen's, no I/O. The first touch frame from a wired interface
// fixes that interface and its sender for the session (vbr0.73 on the G20). Nothing else fixes them: neither another
// PDU, nor a datagram on a bridge twin, nor anything from the car's Wi-Fi, where a phone could otherwise leave the
// real touchscreen ignored for the whole session.
class TouchSource {
public:
    enum Verdict {
        kTouch,        // a touch frame from the touchscreen
        kOther,        // not a touch frame (another PDU of the touchscreen, or anything before it is known)
        kTwin,         // from another interface once the touchscreen's is known: a bridge/VLAN twin, the Wi-Fi
        kOtherSender,  // a touch frame from another sender on the touchscreen's interface
        kUntrusted,    // a touch frame from a Wi-Fi interface (or one without a name) before the touchscreen is known
    };
    TouchSource() : ifindex_(0), ip_(0), port_(0) {}
    // u came in on interface ifindex; wired: that interface is neither Wi-Fi nor unknown.
    Verdict judge(int ifindex, bool wired, const UdpView& u);
    bool known() const { return ifindex_ > 0; }
    int ifindex() const { return ifindex_; }
    uint32_t ip() const { return ip_; }
    uint16_t port() const { return port_; }

private:
    int ifindex_;
    uint32_t ip_;
    uint16_t port_;
};

// Touch state machine, no I/O.
class TouchTracker {
public:
    enum { NS = aa::kTouchMaxContacts };
    TouchTracker(double sx = 1.0, double sy = 1.0, double ox = 0.0, double oy = 0.0, int maxX = 1279, int maxY = 719);
    // One UDP payload. Returns the number of events written, -1 when it is not a touch frame.
    int feed(const uint8_t* buf, size_t n, aa::TouchEventOut* out, int max);
    // Releases the fingers still down (lost "up" frame, picture taken away).
    int releaseAll(aa::TouchEventOut* out, int max);
    int activeCount() const;
    unsigned messages() const { return messages_; }
    int lastContacts(aa::TouchContact* out, int max) const;
    void reset();
    // False until the Android Auto picture has started (true by default).
    void setPictureStarted(bool started) { pictureStarted_ = started; }
    // The last pixel of the phone's display: no coordinate is sent past it.
    void setLimits(int maxX, int maxY) {
        maxX_ = maxX;
        maxY_ = maxY;
    }
    // Panel pixels to the phone's pixels, applied before the offset: 1 unless the picture is stretched.
    void setScale(double sx, double sy) {
        sx_ = sx;
        sy_ = sy;
    }
    // Added to every scaled coordinate before the limits.
    void setOffset(double ox, double oy) {
        ox_ = ox;
        oy_ = oy;
    }

private:
    void fillActive(aa::TouchEventOut& e, int which) const;
    void emitUp(aa::TouchEventOut& e, int k, int x, int y);
    double sx_, sy_, ox_, oy_;
    int maxX_, maxY_;
    bool active_[NS], needDown_[NS];
    bool blocked_[NS];  // this finger touched down without a picture: ignored until it lifts
    bool pictureStarted_;
    int cx_[NS], cy_[NS];
    aa::TouchReassembler reasm_;
    bool haveMsg_;
    unsigned char prevSeq0_, prevSeq1_;
    unsigned messages_;
    aa::TouchContact last_[NS];
    int lastN_;
};

// Which decoded events reach the phone (forward mode), no I/O. Everything while the picture is ours. Once it has gone,
// only the lift of a finger the phone saw go down: a lift decoded between the picture stepping aside (volume, popup,
// reverse, MENU) and the touch thread's next turn was dropped, the tracker forgot the finger, and the phone kept it
// down until the next touch.
class PhoneFingers {
public:
    PhoneFingers() : down_(0) {}
    // Whether ev goes to the phone, owned: the picture is on screen. Keeps track of the fingers down on the phone.
    bool admit(const aa::TouchEventOut& ev, bool owned);
    bool any() const { return down_ != 0; }
    void clear() { down_ = 0; }

private:
    unsigned down_;  // bit k: finger k went down on the phone and has not been lifted there yet
};

// The lifts the tracker gives when the picture leaves (or the service stops) with fingers down: how many of them go to
// the phone. All of them as soon as it saw a finger go down, even the lift of a finger that touched after the picture
// left, so that its gesture ends with a RELEASE (a lift sorted finger by finger would leave it a pointer down); none
// when it saw none. The phone's fingers are forgotten.
inline int phoneLifts(PhoneFingers& phone, int lifts) {
    const int sent = phone.any() ? lifts : 0;
    phone.clear();
    return sent;
}

class EvoTouch {
public:
    using Callback = void (*)(const aa::TouchEventOut&, void*);
    EvoTouch();
    ~EvoTouch();
    // Before start(): the phone's display (core/display_plan.hpp), which begins originX panel columns from the left
    // (its left margin) and is width x height; 1280x720 at 0 by default. Touches are sent in its pixels.
    // scale: panel pixels to the phone's pixels (DisplayPlan::touchScale), 1 when the picture is not stretched.
    void setPhoneDisplay(uint32_t originX, uint32_t width, uint32_t height, double scale = 1.0) {
        tracker_.setScale(scale, scale);
        tracker_.setOffset(-static_cast<double>(originX), 0.0);
        tracker_.setLimits(static_cast<int>(width) - 1, static_cast<int>(height) - 1);
    }
    bool start(InputMode mode, Callback cb, void* user);
    // Forward mode only: true while the Android Auto picture is on screen and the session is up.
    void setOwned(bool owned);
    // Main loop: whether the Android Auto picture has started. A gesture begun before stays ignored until it lifts.
    void setPictureStarted(bool started) { pictureStarted_ = started; }
    void stop();
    void summary() const;  // one summary log line
private:
    static void* threadMain(void* self);
    void run();
    bool openTap();
    void updateSteal(bool want);
    void handle(const UdpView& u, int ifindex, uint32_t now);
    void emit(const aa::TouchEventOut* ev, int n);
    InputMode mode_;
    Callback cb_;
    void* user_;
    pthread_t thread_;
    bool started_;
    std::atomic<bool> running_, owned_, pictureStarted_;
    int tap_, steal_;
    TouchSource source_;  // touch thread; read by summary() once it has stopped
    bool stealRefused_;
    uint32_t lastFrameMs_;
    TouchTracker tracker_;
    unsigned tapped_, touchPdus_, otherPdus_, events_, sent_, stolen_, rawLogged_, eventsLogged_;
    PhoneFingers phone_;  // touch thread only
};

}  // namespace evo

#endif
