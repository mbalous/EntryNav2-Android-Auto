// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Actual EvoVideo lifecycle with fake hardware boundaries. No DRM, compositor or vehicle connection.
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

#include "evo_video.hpp"

static long long nowUs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<long long>(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}

static long long nowMs() {
    return nowUs() / 1000;
}

// The frames handed to EvoVideo. The last byte tells the frames apart at the decoder, SPS/PPS in front or not.
static const uint8_t kSpsPps[] = {0, 0, 0, 1, 0x67, 1, 2, 3, 0, 0, 0, 1, 0x68, 4};
static const uint8_t kIdr[] = {0, 0, 0, 1, 0x65, 9, 9, 9, 9};
static const uint8_t kSlice[] = {0, 0, 0, 1, 0x41, 5, 5};
static const uint8_t kSliceA[] = {0, 0, 0, 1, 0x41, 0xa, 0xa};
static const uint8_t kSliceB[] = {0, 0, 0, 1, 0x41, 0xb, 0xb};
static const uint8_t kSliceC[] = {0, 0, 0, 1, 0x41, 0xc, 0xc};

static unsigned height = 0, opacityCommands = 0;
static std::atomic<unsigned> decodeFailures(0);  // how many of the next frames the fake decoder refuses
static bool visible = false;
static float opacity = 1.f;
static bool decoderHoldsAll = false;  // a stalled decoder: no buffer ever comes back
static const unsigned kFakeBuffers = 4;
static const uint32_t kBufBytes = 8192;  // room for a codec config of kMaxCodecConfig and an IDR
static uint8_t bufferMemory[kFakeBuffers][kBufBytes];
static unsigned poolSize = 1;  // buffers of the next init(); the car has 8
// Bit i: buffer i went to the fake decoder, which gives it back at once, as its DMABUF_COMPLETE would. The pool marks
// it free at its next acquireFree(), under EvoVideo's lock as the event thread does.
static std::atomic<unsigned> givenBack(0);
static unsigned keptAtDestroy = 0;  // buffers still out when the pool went, that the decoder never had

// What the fake decoder was handed, in order.
struct Handed {
    long long atUs;    // monotonic
    pthread_t thread;  // the caller's, or another
    uint32_t len;
    uint8_t tag;  // the frame's last byte
    bool refused;
};
static pthread_mutex_t handedMu = PTHREAD_MUTEX_INITIALIZER;
static const unsigned kHandedMax = 64;
static Handed handed[kHandedMax];
static unsigned handedCount = 0;
static uint32_t decoded = 0;                  // length of the last buffer handed to the decoder
static uint8_t decodedBytes[kBufBytes];       // its bytes, as the decoder got them
static std::atomic<bool> decoderOpen(false);  // from xsv_start to xsv_stop

namespace evo {
// poolSize buffers, fd 7, 8...: a buffer filled but not yet handed to the decoder (a held frame) stays out, one
// handed to it is free again, unless the decoder stalled.
DmaBufPool::DmaBufPool() : drmFd_(-1), dev_(0) {}
DmaBufPool::~DmaBufPool() {}
bool DmaBufPool::init(unsigned, uint32_t) {
    bufs_.clear();
    for (unsigned i = 0; i < poolSize; ++i) {
        Buf b = {nullptr, static_cast<int>(7 + i), bufferMemory[i], kBufBytes, false};
        bufs_.push_back(b);
    }
    givenBack = 0;
    return true;
}
void DmaBufPool::destroy() {
    const unsigned back = givenBack.exchange(0);
    keptAtDestroy = 0;
    for (size_t i = 0; i < bufs_.size(); ++i) {
        keptAtDestroy += bufs_[i].inFlight && !(back & (1u << i));
    }
    bufs_.clear();
}
int DmaBufPool::acquireFree() {
    if (bufs_.empty() || decoderHoldsAll) {
        return -1;
    }
    const unsigned back = givenBack.exchange(0);
    for (size_t i = 0; i < bufs_.size(); ++i) {
        if (back & (1u << i)) {
            bufs_[i].inFlight = false;
        }
    }
    for (size_t i = 0; i < bufs_.size(); ++i) {
        if (!bufs_[i].inFlight) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
int DmaBufPool::find(int fd) {
    return fd >= 7 && fd < 7 + static_cast<int>(bufs_.size()) ? fd - 7 : -1;
}
EvoIlm::EvoIlm() {}
bool EvoIlm::validateTarget(uint32_t, uint32_t, uint32_t, uint32_t, bool) {
    return true;
}
bool EvoIlm::attachSurface(uint32_t, uint32_t, uint32_t, uint32_t h, uint32_t, uint32_t) {
    height = h;
    opacity = 1.f;
    visible = false;
    return true;
}
bool EvoIlm::detachSurface(uint32_t, uint32_t) {
    return true;
}
bool EvoIlm::setSurfaceVisibility(uint32_t, bool v) {
    visible = v;
    return true;
}
bool EvoIlm::setSurfaceOpacity(uint32_t, float v) {
    ++opacityCommands;
    opacity = v;
    return true;
}
}  // namespace evo
// The decoder's event queue, a pipe as in the car's library: [0] read by the event thread, [1] written by the test.
static int queue[2] = {-1, -1};
static std::atomic<unsigned> receives(0);  // calls of xsv_receive_event
static void closeQueueEnd(int end) {
    if (queue[end] >= 0) {
        close(queue[end]);
        queue[end] = -1;
    }
}
extern "C" {
xsv_handle xsv_open(int, uint32_t, const char*) {
    assert(queue[0] < 0 && pipe(queue) == 0);
    return reinterpret_cast<void*>(1);
}
void xsv_close(xsv_handle) {
    decoderOpen = false;
    closeQueueEnd(0);
    closeQueueEnd(1);
}
int xsv_configure_stream(xsv_handle, uint32_t, uint32_t, float) {
    return 0;
}
int xsv_set_output_dimension(xsv_handle, uint32_t, uint32_t) {
    return 0;
}
int xsv_enable_frame_events(xsv_handle) {
    return 0;
}
int xsv_update(xsv_handle) {
    return 0;
}
int xsv_start(xsv_handle) {
    decoderOpen = true;
    return 0;
}
int xsv_stop(xsv_handle) {
    decoderOpen = false;
    return 0;
}
int xsv_decode_video_frame(xsv_handle, int fd, uint32_t len) {
    assert(decoderOpen);  // never once the decoder has stopped
    const unsigned i = static_cast<unsigned>(fd - 7);
    assert(fd >= 7 && i < poolSize && len && len <= kBufBytes);
    pthread_mutex_lock(&handedMu);
    decoded = len;
    memcpy(decodedBytes, bufferMemory[i], len);
    const bool refused = decodeFailures > 0;
    if (refused) {
        --decodeFailures;
    }
    if (handedCount < kHandedMax) {
        Handed& h = handed[handedCount++];
        h.atUs = nowUs();
        h.thread = pthread_self();
        h.len = len;
        h.tag = bufferMemory[i][len - 1];
        h.refused = refused;
    }
    pthread_mutex_unlock(&handedMu);
    if (refused) {
        return -1;
    }
    givenBack |= 1u << i;
    return 0;
}
// As the car's library does it: an event read when the pipe has one, 0 at once when it has hung up, -1 when poll fails.
int xsv_receive_event(xsv_handle, int* event, int timeoutMs) {
    ++receives;
    struct pollfd p = {queue[0], POLLIN, 0};
    int got = poll(&p, 1, timeoutMs);
    if (got > 0) {
        if (!(p.revents & POLLIN)) {
            return 0;
        }
        got = static_cast<int>(read(queue[0], event, 20));
    }
    return got < 0 ? -1 : got != 0;
}
int xsv_get_event_queue_fd(xsv_handle) {
    return queue[0];
}
}

// One decoder event, written as the library does: 20 bytes, [0] type.
static void decoderEvent(int type) {
    int event[5] = {type, 0, 0, 0, 0};
    assert(write(queue[1], event, sizeof(event)) == static_cast<ssize_t>(sizeof(event)));
}

static uint32_t lastDecoded() {
    pthread_mutex_lock(&handedMu);
    const uint32_t len = decoded;
    pthread_mutex_unlock(&handedMu);
    return len;
}

// The decoder received SPS/PPS followed by the IDR frame.
static bool configThenIdr() {
    pthread_mutex_lock(&handedMu);
    const bool got = decoded == sizeof(kSpsPps) + sizeof(kIdr) && !memcmp(decodedBytes, kSpsPps, sizeof(kSpsPps)) &&
                     !memcmp(decodedBytes + sizeof(kSpsPps), kIdr, sizeof(kIdr));
    pthread_mutex_unlock(&handedMu);
    return got;
}

static unsigned handedSoFar() {
    pthread_mutex_lock(&handedMu);
    const unsigned n = handedCount;
    pthread_mutex_unlock(&handedMu);
    return n;
}

static Handed handedAt(unsigned i) {
    pthread_mutex_lock(&handedMu);
    assert(i < handedCount);
    const Handed h = handed[i];
    pthread_mutex_unlock(&handedMu);
    return h;
}

static void forgetHanded() {
    pthread_mutex_lock(&handedMu);
    handedCount = 0;
    pthread_mutex_unlock(&handedMu);
}

// How long the tests wait for the other threads before they give up: QEMU on a busy machine can be slow.
static const long long kWaitMs = 5000;

// Until the decoder has had n frames, for up to kWaitMs.
static bool waitHanded(unsigned n) {
    const long long since = nowMs();
    while (handedSoFar() < n && nowMs() - since < kWaitMs) {
        usleep(500);
    }
    return handedSoFar() >= n;
}

static bool here(const Handed& h) {
    return pthread_equal(h.thread, pthread_self()) != 0;
}

// When a decoder that keeps every buffer counts as stalled, on a clock of our own.
static void decoderStallRule() {
    const uint32_t limit = evo::DecoderStall::kLimitMs;
    evo::DecoderStall stall;
    // On screen: reported once, when no buffer was free for the whole limit.
    const uint32_t t0 = 0xfffffc00u;  // across the clock's wrap-around
    assert(!stall.starved(t0, true) && !stall.stuck(t0));
    assert(!stall.starved(t0 + limit - 1, true) && !stall.stuck(t0 + limit - 1));
    assert(stall.starved(t0 + limit, true) && stall.stuck(t0 + limit));
    assert(!stall.starved(t0 + limit + 500, true) && stall.stuck(t0 + limit + 500));  // once
    stall.bufferBack();
    assert(!stall.stuck(t0 + limit + 600));
    assert(!stall.starved(t0 + limit + 700, true) && !stall.starved(t0 + 3 * limit, true));  // still once
    // A buffer back before the limit starts the count again.
    stall.restart();
    assert(!stall.starved(1000, true) && !stall.starved(1000 + limit - 1, true));
    stall.bufferBack();
    assert(!stall.starved(1000 + limit, true) && !stall.stuck(1000 + limit + limit - 1));
    assert(stall.starved(1000 + limit + limit, true));
    // Hidden: never counted, and the count starts when the picture is shown again.
    stall.restart();
    assert(!stall.starved(0, false) && !stall.starved(2 * limit, false) && !stall.stuck(2 * limit));
    assert(!stall.starved(2 * limit + 1, true) && !stall.starved(3 * limit, true));
    assert(!stall.starved(3 * limit + 1, false) && !stall.starved(3 * limit + 2, true));
    assert(!stall.starved(4 * limit + 1, true) && stall.starved(4 * limit + 2, true));
}

// A codec config bigger than any SPS/PPS is refused: in front of each IDR it left no room for the picture in the
// buffer, and every IDR was lost while it stayed. Up to kMaxCodecConfig it is taken.
static void oversizedCodecConfig(evo::EvoVideo& video, const evo::EvoVideoConfig& cfg) {
    typedef evo::FrameFate Fate;
    assert(video.init(cfg) && video.start());
    video.postCodecConfig(kSpsPps, sizeof(kSpsPps));
    static uint8_t huge[kBufBytes - sizeof(kIdr) + 1];  // with the IDR: 1 B too many
    memset(huge, 0x11, sizeof(huge));
    video.postCodecConfig(huge, sizeof(huge));
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && configThenIdr());
    static uint8_t largest[evo::EvoVideo::kMaxCodecConfig];
    memset(largest, 0x22, sizeof(largest));
    video.postCodecConfig(largest, sizeof(largest));
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding &&
           lastDecoded() == sizeof(largest) + sizeof(kIdr));
    assert(decodedBytes[0] == 0x22 && !memcmp(decodedBytes + sizeof(largest), kIdr, sizeof(kIdr)));
    video.stop();
}

// libxsvideo gives no buffer back: once the limit has passed on screen, the frame says so once, and the frames after
// it no longer wait 20 ms each for a buffer. A new start() feeds a decoder again.
static void stalledDecoder(evo::EvoVideo& video, const evo::EvoVideoConfig& cfg) {
    typedef evo::FrameFate Fate;
    assert(video.init(cfg) && video.start());
    video.setVisible(true);
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
    decoderHoldsAll = true;
    const long long since = nowMs();
    Fate fate = Fate::kLost;
    unsigned dropped = 0;
    while (fate == Fate::kLost && nowMs() - since < evo::DecoderStall::kLimitMs + 2000) {
        fate = video.postVideoFrame(kSlice, sizeof(kSlice), 0);
        ++dropped;
    }
    const long long lasted = nowMs() - since;
    assert(fate == Fate::kStalled && lasted >= evo::DecoderStall::kLimitMs && dropped > 1);
    const long long after = nowMs();
    for (int i = 0; i < 10; ++i) {
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kLost);  // reported once
    }
    assert(nowMs() - after < 50);  // 10 frames, no 20 ms wait each
    video.stop();
    decoderHoldsAll = false;
    assert(video.init(cfg) && video.start());
    video.setVisible(true);
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
    video.stop();
}

// The decoder's events reach EvoVideo; once its queue has hung up, the event thread ends instead of asking it again
// and again, each call returning at once without an event (a core at 100 %).
static void brokenEventQueue(evo::EvoVideo& video, const evo::EvoVideoConfig& cfg) {
    assert(video.init(cfg) && video.start());
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == evo::FrameFate::kDecoding && !video.firstFrameSeen());
    decoderEvent(XSV_EVENT_FRAME);
    long long since = nowMs();
    while (!video.firstFrameSeen() && nowMs() - since < 2000) {
        usleep(1000);
    }
    assert(video.firstFrameSeen());
    closeQueueEnd(1);  // the library's own end of the pipe goes
    usleep(100000);
    const unsigned before = receives;
    usleep(300000);
    assert(receives - before == 0);
    since = nowMs();
    video.stop();
    assert(nowMs() - since < 100);  // the event thread had already ended
}

// Which frames FrameGap holds, and for how long, on a clock of our own.
static void frameGapRule() {
    const uint32_t gap = evo::kFrameGapMs;
    evo::FrameGap frames;
    frames.restart(true);
    const uint32_t t = 0xffffff80u;             // wraps around between t + 103 and t + 150
    assert(frames.arrive(t) == 0);              // the first frame of a stream
    assert(frames.arrive(t + 33) == 0);         // evenly spaced
    assert(frames.arrive(t + 33 + gap) == 0);   // exactly the gap
    assert(frames.arrive(t + 100) == 0);        // a pair: the first goes at once,
    assert(frames.arrive(t + 102) == gap - 2);  // the second waits for the rest of the gap
    assert(frames.arrive(t + 103) == 0);        // never the frame after a held one
    assert(frames.arrive(t + 103) == gap);      // the longest hold: no time at all since the previous frame
    assert(frames.arrive(t + 150) == 0);
    assert(frames.arrive(t + 150 + gap - 1) == 1);
    evo::FrameGapStats seen = frames.take();
    assert(seen.frames == 9 && seen.early == 4 && seen.held == 3 && seen.longestHoldMs == gap);
    seen = frames.take();  // counted again from zero
    assert(seen.frames == 0 && seen.early == 0 && seen.held == 0 && seen.longestHoldMs == 0);
    // Whatever the arrivals, no hold exceeds the gap and two holds never follow each other.
    uint32_t now = 5, seed = 12345;
    bool heldBefore = false;
    for (int i = 0; i < 2000; ++i) {
        seed = seed * 1103515245u + 12345u;
        now += (seed >> 16) % 60;  // 0 to 59 ms apart
        const uint32_t hold = frames.arrive(now);
        assert(hold <= gap && !(hold && heldBefore));
        heldBefore = hold != 0;
    }
    // Without holding (USB): early frames are counted, none held.
    frames.restart(false);
    assert(frames.arrive(10) == 0 && frames.arrive(11) == 0 && frames.arrive(12) == 0);
    seen = frames.take();
    assert(seen.frames == 3 && seen.early == 2 && seen.held == 0 && seen.longestHoldMs == 0);
    // A new stream has no previous frame.
    frames.restart(true);
    assert(frames.arrive(12) == 0);
}

// The cases below run in real time: frames posted back to back reach postVideoFrame within kFrameGapMs of each other
// only when this machine is not busy elsewhere, so each case gets a few tries (FrameGap's own timings are checked above
// with a clock of its own). What holds whatever the timing is checked on every try.
static const int kTries = 5;

// The hold runs over Wi-Fi only, between the decoder and the frame before; its counts reach the caller.
static void framesSpread(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    poolSize = kFakeBuffers;  // a held frame keeps its buffer until it goes: the next one takes another
    bool spread = false;
    for (int i = 0; i < kTries && !spread; ++i) {
        cfg.holdCloseFrames = true;
        assert(video.init(cfg) && video.start());
        forgetHanded();
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
        const long long postedMs = nowMs();
        assert(video.postVideoFrame(kSliceA, sizeof(kSliceA), 0) == Fate::kDecoding);  // held
        // Queued behind the held frame: it goes right after it, and would not be held anyway.
        assert(video.postVideoFrame(kSliceB, sizeof(kSliceB), 0) == Fate::kDecoding);
        assert(waitHanded(3));
        const evo::FrameGapStats seen = video.takeFrameGaps();
        video.stop();
        const Handed idr = handedAt(0), held = handedAt(1), after = handedAt(2);
        const long long pair = (held.atUs - idr.atUs) / 1000, afterHeld = (after.atUs - held.atUs) / 1000;
        // Whatever the timing: three frames in their order from the submit thread, never two holds in a row, only an
        // early frame held, and the decoder had both slices no earlier than the hold allowed.
        assert(handedSoFar() == 3 && idr.tag == kIdr[sizeof(kIdr) - 1] && held.tag == 0xa && after.tag == 0xb);
        assert(!here(idr) && pthread_equal(idr.thread, held.thread) && pthread_equal(held.thread, after.thread));
        assert(seen.frames == 3 && seen.held <= 1 && seen.held <= seen.early);
        assert(seen.held == 0 || after.atUs / 1000 >= postedMs + seen.longestHoldMs);
        spread = pair >= evo::kFrameGapMs - 5 && afterHeld < 15 && seen.early == 1 && seen.held == 1 &&
                 seen.longestHoldMs >= evo::kFrameGapMs - 5;
    }
    assert(spread);
    // Over the cable: at once, only counted.
    bool counted = false;
    for (int i = 0; i < kTries && !counted; ++i) {
        cfg.holdCloseFrames = false;
        assert(video.init(cfg) && video.start());
        const long long since = nowMs();
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding);
        const long long took = nowMs() - since;
        const evo::FrameGapStats seen = video.takeFrameGaps();
        video.stop();
        assert(seen.frames == 2 && seen.held == 0);  // never held over the cable, whatever the timing
        counted = took < 15 && seen.early == 1;
    }
    assert(counted);
    poolSize = 1;
}

// Over Wi-Fi the submit thread is the only one that talks to the decoder: a frame with nothing ahead of it and no hold
// goes from there at once, and a held frame waits in its buffer, not on the caller's thread (the phone's). The call
// returns at once, and the decoder gets the frame at its time. The phone's thread acks each frame after the call and
// answers the phone's pings: with the hold on it, every held frame made both late by the hold.
static void heldFrameReturnsAtOnce(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    poolSize = kFakeBuffers;
    cfg.holdCloseFrames = true;
    bool atOnce = false;
    for (int i = 0; i < kTries && !atOnce; ++i) {
        assert(video.init(cfg) && video.start());
        forgetHanded();
        long long sinceUs = nowUs();
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
        const long long idrCallUs = nowUs() - sinceUs;
        assert(waitHanded(1));
        const Handed idr = handedAt(0);
        assert(!here(idr) && !idr.refused);  // from the submit thread, even with nothing ahead of it and no hold
        const long long idrLatencyUs = idr.atUs - sinceUs;
        const long long postedMs = nowMs();
        sinceUs = nowUs();
        assert(video.postVideoFrame(kSliceA, sizeof(kSliceA), 0) == Fate::kDecoding);
        const long long tookUs = nowUs() - sinceUs;
        const unsigned handedOnReturn = handedSoFar();
        assert(waitHanded(2));
        const evo::FrameGapStats seen = video.takeFrameGaps();
        video.stop();
        const Handed slice = handedAt(1);
        assert(handedSoFar() == 2 && slice.tag == 0xa && !slice.refused && seen.frames == 2);
        assert(pthread_equal(slice.thread, idr.thread));
        assert(seen.held == 0 || slice.atUs / 1000 >= postedMs + seen.longestHoldMs);  // never before its time
        atOnce = idrCallUs < 5000 && idrLatencyUs < 5000 && seen.held == 1 &&
                 seen.longestHoldMs >= evo::kFrameGapMs - 5 && tookUs < 5000 && handedOnReturn == 1 &&
                 slice.atUs / 1000 <= postedMs + seen.longestHoldMs + 15;
    }
    assert(atOnce);
    poolSize = 1;
}

// Frames that come while a held one waits queue behind it: the decoder gets them in their order, from one thread. The
// frame after the held one goes right behind it, the next one a gap later, as when the caller's thread waited for each
// hold: a burst stays spread. Every call returns at once.
static void queuedInOrder(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    poolSize = kFakeBuffers;
    cfg.holdCloseFrames = true;
    const uint8_t* const frames[] = {kSliceA, kSliceB, kSliceC};
    bool inOrder = false;
    for (int i = 0; i < kTries && !inOrder; ++i) {
        assert(video.init(cfg) && video.start());
        forgetHanded();
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && waitHanded(1));
        long long longestCallUs = 0;
        for (int f = 0; f < 3; ++f) {
            const long long sinceUs = nowUs();
            assert(video.postVideoFrame(frames[f], sizeof(kSliceA), 0) == Fate::kDecoding);
            const long long tookUs = nowUs() - sinceUs;
            longestCallUs = tookUs > longestCallUs ? tookUs : longestCallUs;
        }
        const unsigned handedOnReturn = handedSoFar();
        assert(waitHanded(4));
        const evo::FrameGapStats seen = video.takeFrameGaps();
        video.stop();
        // Whatever the timing: all four, in their order, none refused, all from the submit thread.
        assert(handedSoFar() == 4 && seen.frames == 4);
        const Handed idr = handedAt(0), a = handedAt(1), b = handedAt(2), c = handedAt(3);
        assert(idr.tag == kIdr[sizeof(kIdr) - 1] && a.tag == 0xa && b.tag == 0xb && c.tag == 0xc);
        assert(!idr.refused && !a.refused && !b.refused && !c.refused);
        assert(idr.atUs <= a.atUs && a.atUs <= b.atUs && b.atUs <= c.atUs);
        assert(!here(idr) && pthread_equal(idr.thread, a.thread) && pthread_equal(a.thread, b.thread) &&
               pthread_equal(b.thread, c.thread));
        inOrder = handedOnReturn == 1 && longestCallUs < 5000 && seen.held == 2 && b.atUs - a.atUs < 15000 &&
                  c.atUs - b.atUs >= (evo::kFrameGapMs - 5) * 1000;
    }
    assert(inOrder);
    poolSize = 1;
}

// A burst longer than the free buffers: queued frames keep theirs until they go, and a frame that finds none waits for
// them instead of being dropped, as it waited when the caller's thread held frames itself. The fake decoder gives a
// buffer back without waking the waiting frame: it finds it on its next look, 20 ms later at most.
static void burstLongerThanBuffers(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    poolSize = 2;
    cfg.holdCloseFrames = true;
    assert(video.init(cfg) && video.start());
    forgetHanded();
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
    assert(video.postVideoFrame(kSliceA, sizeof(kSliceA), 0) == Fate::kDecoding);  // held: one buffer
    assert(video.postVideoFrame(kSliceB, sizeof(kSliceB), 0) == Fate::kDecoding);  // behind it: the other one
    assert(video.postVideoFrame(kSliceC, sizeof(kSliceC), 0) == Fate::kDecoding);  // none free until they go
    assert(waitHanded(4));
    video.stop();
    assert(handedSoFar() == 4 && handedAt(1).tag == 0xa && handedAt(2).tag == 0xb && handedAt(3).tag == 0xc);
    for (unsigned f = 0; f < 4; ++f) {
        assert(!here(handedAt(f)) && !handedAt(f).refused);
    }
    poolSize = 1;
}

// Posts slices, kFrameGapMs and more apart (none held), until one says kLost: how many calls that took, 0 when none did
// within kWaitMs. Every other call says kDecoding.
static unsigned callsUntilLost(evo::EvoVideo& video) {
    const long long since = nowMs();
    for (unsigned calls = 1; nowMs() - since < kWaitMs; ++calls) {
        const evo::FrameFate fate = video.postVideoFrame(kSlice, sizeof(kSlice), 0);
        if (fate == evo::FrameFate::kLost) {
            return calls;
        }
        assert(fate == evo::FrameFate::kDecoding);
        usleep((evo::kFrameGapMs + 5) * 1000);
    }
    return 0;
}

// Over Wi-Fi the decoder refuses a frame after its call has returned: the next call says kLost, once (the session asks
// the phone for a keyframe). SPS/PPS that went with it go again in front of the next IDR; a refused frame without them
// leaves that alone. The fake decoder notes the refusal a moment before EvoVideo takes it in: the call right after may
// still miss it, and a later one tells it. That the very next call tells it is what the tries look for.
static void frameRefusedLater(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    poolSize = kFakeBuffers;
    cfg.holdCloseFrames = true;
    bool told = false, toldWithout = false;
    for (int i = 0; i < kTries && !(told && toldWithout); ++i) {
        assert(video.init(cfg) && video.start());
        forgetHanded();
        video.postCodecConfig(kSpsPps, sizeof(kSpsPps));
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding && waitHanded(1));
        // An IDR, SPS/PPS in front, refused after its call has returned.
        decodeFailures = 1;
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
        assert(waitHanded(2) && handedAt(1).refused && handedAt(1).len == sizeof(kSpsPps) + sizeof(kIdr));
        const unsigned calls = callsUntilLost(video);
        assert(calls);
        // Told once; the next IDR gets SPS/PPS in front again.
        unsigned handedNow = 2 + calls;
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
        assert(waitHanded(++handedNow) && configThenIdr() && !handedAt(handedNow - 1).refused);
        told = told || calls == 1;
        // A slice refused later, no SPS/PPS with it: the next IDR goes without them, they were sent.
        assert(video.postVideoFrame(kSliceB, sizeof(kSliceB), 0) == Fate::kDecoding && waitHanded(++handedNow));
        decodeFailures = 1;
        assert(video.postVideoFrame(kSliceC, sizeof(kSliceC), 0) == Fate::kDecoding);
        assert(waitHanded(++handedNow) && handedAt(handedNow - 1).refused && handedAt(handedNow - 1).tag == 0xc);
        const unsigned callsWithout = callsUntilLost(video);
        assert(callsWithout);
        handedNow += callsWithout;
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
        assert(waitHanded(++handedNow) && lastDecoded() == sizeof(kIdr) && !handedAt(handedNow - 1).refused);
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding);  // told once
        toldWithout = toldWithout || callsWithout == 1;
        video.stop();
    }
    assert(told && toldWithout);
    poolSize = 1;
}

// A refusal not told yet when the next frame is dropped (no free buffer): that frame's kLost tells both, and the call
// after it says nothing more (one keyframe request for both).
static void refusalToldByDroppedFrame(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    poolSize = kFakeBuffers;
    cfg.holdCloseFrames = true;
    bool once = false;
    for (int i = 0; i < kTries && !once; ++i) {
        assert(video.init(cfg) && video.start());
        forgetHanded();
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding && waitHanded(1));
        decodeFailures = 1;
        assert(video.postVideoFrame(kSliceA, sizeof(kSliceA), 0) == Fate::kDecoding);
        assert(waitHanded(2) && handedAt(1).refused);
        decoderHoldsAll = true;  // the next frame finds no buffer and is dropped
        const Fate dropped = video.postVideoFrame(kSliceB, sizeof(kSliceB), 0);
        decoderHoldsAll = false;
        const Fate next = video.postVideoFrame(kSliceC, sizeof(kSliceC), 0);
        const Fate then = video.postVideoFrame(kSlice, sizeof(kSlice), 0);
        video.stop();
        // Whatever the timing: the drop says kLost, and the refusal is told once at most after it (by then it had been
        // told by the drop or by one of these two calls).
        assert(dropped == Fate::kLost);
        assert((next == Fate::kDecoding || next == Fate::kLost) && (then == Fate::kDecoding || then == Fate::kLost));
        assert(next == Fate::kDecoding || then == Fate::kDecoding);
        once = next == Fate::kDecoding && then == Fate::kDecoding;
    }
    assert(once);
    poolSize = 1;
}

// stop() while a held frame waits: the decoder never gets it (the fake decoder checks that it is still open), its
// buffer is free again before the pool goes, and stop() does not hang.
static void stopWhileWaiting(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    poolSize = kFakeBuffers;
    cfg.holdCloseFrames = true;
    bool waited = false;
    for (int i = 0; i < kTries && !waited; ++i) {
        assert(video.init(cfg) && video.start());
        forgetHanded();
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && waitHanded(1));
        const long long sinceUs = nowUs();
        assert(video.postVideoFrame(kSliceA, sizeof(kSliceA), 0) == Fate::kDecoding);
        const long long tookUs = nowUs() - sinceUs;
        const unsigned beforeStop = handedSoFar();
        const long long since = nowMs();
        video.stop();
        const long long stopMs = nowMs() - since;
        const unsigned atStop = handedSoFar();
        usleep(2 * evo::kFrameGapMs * 1000);  // past the held frame's time
        assert(handedSoFar() == atStop && keptAtDestroy == 0);
        assert(stopMs < 1000);  // the event thread looks at its queue every 200 ms
        assert(video.postVideoFrame(kSliceB, sizeof(kSliceB), 0) == Fate::kNoStream);
        waited = tookUs < 5000 && beforeStop == 1 && atStop == 1;
    }
    assert(waited);
    poolSize = 1;
}

// Over Wi-Fi too, a decoder that keeps every buffer is reported once, kLimitMs after the first frame that found none on
// screen. A frame waiting for its time keeps a buffer that comes back once it has gone: the frames meanwhile wait for
// it, nothing is counted then, and the stall is not reported earlier. The waiting frame still reaches the decoder.
static void stalledDecoderWhileHolding(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    poolSize = kFakeBuffers;
    cfg.holdCloseFrames = true;
    assert(video.init(cfg) && video.start());
    video.setVisible(true);
    forgetHanded();
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && waitHanded(1));
    assert(video.postVideoFrame(kSliceA, sizeof(kSliceA), 0) == Fate::kDecoding);  // held when this machine is quick
    decoderHoldsAll = true;
    const long long since = nowMs();
    Fate fate = Fate::kLost;
    unsigned dropped = 0;
    while (fate == Fate::kLost && nowMs() - since < evo::DecoderStall::kLimitMs + kWaitMs) {
        fate = video.postVideoFrame(kSlice, sizeof(kSlice), 0);
        ++dropped;
    }
    const long long lasted = nowMs() - since;
    assert(fate == Fate::kStalled && lasted >= evo::DecoderStall::kLimitMs && dropped > 1);
    assert(waitHanded(2) && handedSoFar() == 2 && handedAt(1).tag == 0xa);  // no dropped frame reached it
    const long long after = nowMs();
    for (int i = 0; i < 10; ++i) {
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kLost);  // reported once
    }
    assert(nowMs() - after < 50);  // 10 frames, no 20 ms wait each
    video.stop();
    decoderHoldsAll = false;
    assert(video.init(cfg) && video.start());
    video.setVisible(true);
    forgetHanded();
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && waitHanded(1));
    video.stop();
    poolSize = 1;
}

// Over USB nothing is held, and nothing changes: every frame goes to the decoder from the caller's thread before its
// call returns, even frames right behind each other, and a refused frame says so itself.
static void usbUnchanged(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    cfg.holdCloseFrames = false;
    assert(video.init(cfg) && video.start());
    forgetHanded();
    const uint8_t* const frames[] = {kIdr, kSliceA, kSliceB, kSliceC};
    const size_t sizes[] = {sizeof(kIdr), sizeof(kSliceA), sizeof(kSliceB), sizeof(kSliceC)};
    for (unsigned f = 0; f < 4; ++f) {
        assert(video.postVideoFrame(frames[f], sizes[f], 0) == Fate::kDecoding);
        assert(handedSoFar() == f + 1 && here(handedAt(f)) && handedAt(f).tag == frames[f][sizes[f] - 1]);
    }
    decodeFailures = 1;
    assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kLost && here(handedAt(4)));
    assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding);
    const evo::FrameGapStats seen = video.takeFrameGaps();
    assert(seen.frames == 6 && seen.held == 0);
    video.stop();
}

int main() {
    typedef evo::FrameFate Fate;
    evo::EvoVideoConfig cfg;
    evo::EvoVideo video;
    assert(!video.isReady());
    assert(video.init(cfg) && video.start() && video.isReady() && height == 480);
    video.setVisible(true);
    assert(video.isVisible() && visible);
    assert(video.setOpacity(0.25f) && opacity == 0.25f);
    const unsigned n = opacityCommands;
    assert(video.setOpacity(0.25f) && opacityCommands == n);  // no repeated command on each main-loop tick
    assert(video.setOpacity(1.f) && opacity == 1.f);
    video.setVisible(false);  // camera/menu always hides the whole AA surface
    assert(!visible && !video.isVisible());
    video.setVisible(true);
    assert(visible);

    // SPS/PPS go in front of the first IDR; a frame the decoder refuses keeps them for the next IDR.
    video.postCodecConfig(kSpsPps, sizeof(kSpsPps));
    assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding && lastDecoded() == sizeof(kSlice));
    decodeFailures = 1;
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kLost && configThenIdr());
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && configThenIdr());
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && lastDecoded() == sizeof(kIdr));  // once

    video.stop();
    assert(!visible && !video.isVisible() && !video.isReady());
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kNoStream);  // stopped: nothing lost
    assert(video.init(cfg) && video.start() && video.isReady());
    assert(opacity == 1.f && video.setOpacity(0.25f) && opacity == 0.25f);  // fresh surface, fresh cache
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && configThenIdr());  // SPS/PPS again
    video.stop();
    puts("PASS video presentation: visibility, opacity cache, camera hide, SPS/PPS after a refused IDR, reconnect");
    oversizedCodecConfig(video, cfg);
    puts("PASS video presentation: a codec config over 4 KiB is refused, so that IDR frames keep their room");

    decoderStallRule();
    stalledDecoder(video, cfg);
    puts("PASS video presentation: a decoder that keeps every buffer 3 s on screen is reported once, then no waiting");
    brokenEventQueue(video, cfg);
    puts("PASS video presentation: decoder events read from its queue; a queue that hung up ends the event thread");
    frameGapRule();
    framesSpread(video, cfg);
    puts(
        "PASS video presentation: over Wi-Fi a frame within 25 ms of the previous one reaches the decoder the rest "
        "later, never two in a row; USB at once");
    heldFrameReturnsAtOnce(video, cfg);
    queuedInOrder(video, cfg);
    burstLongerThanBuffers(video, cfg);
    puts(
        "PASS video presentation: over Wi-Fi one thread feeds the decoder; a held frame waits in its buffer, the call "
        "returns at once; the frames behind it go in order, a burst stays spread, none dropped for want of a buffer");
    frameRefusedLater(video, cfg);
    refusalToldByDroppedFrame(video, cfg);
    puts(
        "PASS video presentation: a frame refused after its call makes a later call say lost, once, a dropped frame's "
        "included; SPS/PPS sent again");
    stopWhileWaiting(video, cfg);
    puts("PASS video presentation: stop() with a held frame waiting: never decoded, buffer freed, no hang");
    stalledDecoderWhileHolding(video, cfg);
    puts("PASS video presentation: over Wi-Fi a decoder that keeps every buffer is still reported once, after 3 s");
    usbUnchanged(video, cfg);
    puts("PASS video presentation: over USB every frame still goes to the decoder from the caller, before it returns");
}
