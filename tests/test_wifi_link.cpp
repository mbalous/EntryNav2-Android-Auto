// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The Wi-Fi link's figures: station messages as the kernel builds them, the log line, TCP on loopback, and the copy
// of the phone's connection the log keeps and gives back. No Wi-Fi interface is read.
#include <arpa/inet.h>
#include <linux/nl80211.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#include "wifi_link.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace {

// Netlink attributes: length (header included), type, value, padded to 4 bytes.
struct Attrs {
    std::vector<uint8_t> bytes;
    void put(uint16_t type, const void* data, size_t len) {
        const uint16_t total = static_cast<uint16_t>(4 + len);
        const uint8_t* d = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&total),
                     reinterpret_cast<const uint8_t*>(&total) + 2);
        bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&type), reinterpret_cast<const uint8_t*>(&type) + 2);
        bytes.insert(bytes.end(), d, d + len);
        bytes.resize((bytes.size() + 3) & ~static_cast<size_t>(3), 0);
    }
    void u8(uint16_t type, uint8_t v) { put(type, &v, 1); }
    void u16(uint16_t type, uint16_t v) { put(type, &v, 2); }
    void u32(uint16_t type, uint32_t v) { put(type, &v, 4); }
    void nested(uint16_t type, const Attrs& inside) {
        put(static_cast<uint16_t>(type | 0x8000), inside.bytes.empty() ? nullptr : &inside.bytes[0],
            inside.bytes.size());
    }
};

const uint8_t kPhone[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

Attrs rate(uint16_t tenths) {
    Attrs r;
    r.u16(NL80211_RATE_INFO_BITRATE, tenths);
    r.u8(NL80211_RATE_INFO_MCS, 7);
    return r;
}

Attrs stationMessage(uint32_t frames, uint32_t retries, uint32_t failed, uint32_t connected) {
    Attrs info;
    info.u32(NL80211_STA_INFO_INACTIVE_TIME, 12);
    info.u32(NL80211_STA_INFO_RX_BYTES, 123456);
    info.u8(NL80211_STA_INFO_SIGNAL, static_cast<uint8_t>(-52));
    info.u8(NL80211_STA_INFO_SIGNAL_AVG, static_cast<uint8_t>(-51));
    info.nested(NL80211_STA_INFO_TX_BITRATE, rate(1500));
    info.nested(NL80211_STA_INFO_RX_BITRATE, rate(1350));
    info.u32(NL80211_STA_INFO_TX_PACKETS, frames);
    info.u32(NL80211_STA_INFO_TX_RETRIES, retries);
    info.u32(NL80211_STA_INFO_TX_FAILED, failed);
    info.u32(NL80211_STA_INFO_CONNECTED_TIME, connected);
    Attrs message;
    message.u32(NL80211_ATTR_IFINDEX, 4);
    message.put(NL80211_ATTR_MAC, kPhone, sizeof(kPhone));
    message.nested(NL80211_ATTR_STA_INFO, info);
    return message;
}

evo::WifiStation station(uint32_t frames, uint32_t retries, uint32_t failed, uint32_t connected) {
    const Attrs m = stationMessage(frames, retries, failed, connected);
    evo::WifiStation s;
    CHECK(evo::parseWifiStation(&m.bytes[0], m.bytes.size(), s));
    return s;
}

void stationMessages() {
    const evo::WifiStation s = station(5000, 300, 2, 125);
    CHECK(!memcmp(s.mac, kPhone, sizeof(kPhone)));
    CHECK(s.signalDbm == -52 && s.signalAvgDbm == -51 && s.rxRate == 1350);
    // The retry count the driver gives (always 0 on the WL18xx) is not kept.
    CHECK(s.txFrames == 5000 && s.txFailed == 2 && s.connectedS == 125);

    // A driver that gives little: what is missing stays "no figure".
    Attrs info;
    info.u8(NL80211_STA_INFO_SIGNAL, static_cast<uint8_t>(-70));
    info.u16(NL80211_STA_INFO_TX_PACKETS, 9);  // not the size nl80211 gives it: ignored
    Attrs bare;
    bare.put(NL80211_ATTR_MAC, kPhone, sizeof(kPhone));
    bare.nested(NL80211_ATTR_STA_INFO, info);
    evo::WifiStation little;
    CHECK(evo::parseWifiStation(&bare.bytes[0], bare.bytes.size(), little));
    CHECK(little.signalDbm == -70 && little.signalAvgDbm == evo::kNoFigure && little.rxRate == evo::kNoFigure &&
          little.txFrames == evo::kNoFigure && little.connectedS == evo::kNoFigure);

    // A level of 0 dBm is the WL18xx's "not measured" (seen in every car log): no figure.
    Attrs zero;
    Attrs zeroInfo;
    zeroInfo.u8(NL80211_STA_INFO_SIGNAL, 0);
    zeroInfo.u8(NL80211_STA_INFO_SIGNAL_AVG, static_cast<uint8_t>(-68));
    zero.put(NL80211_ATTR_MAC, kPhone, sizeof(kPhone));
    zero.nested(NL80211_ATTR_STA_INFO, zeroInfo);
    CHECK(evo::parseWifiStation(&zero.bytes[0], zero.bytes.size(), little));
    CHECK(little.signalDbm == evo::kNoFigure && little.signalAvgDbm == -68);

    // No station address: not a station.
    Attrs anonymous;
    anonymous.nested(NL80211_ATTR_STA_INFO, info);
    CHECK(!evo::parseWifiStation(&anonymous.bytes[0], anonymous.bytes.size(), little));

    // Cut anywhere, or with a length that lies: never read past the end.
    const Attrs whole = stationMessage(1, 2, 3, 4);
    for (size_t cut = 0; cut < whole.bytes.size(); ++cut) {
        std::vector<uint8_t> part(whole.bytes.begin(), whole.bytes.begin() + cut);
        part.reserve(cut + 1);
        evo::parseWifiStation(part.empty() ? nullptr : &part[0], part.size(), little);
    }
    std::vector<uint8_t> lying = whole.bytes;
    lying[0] = 0xff;
    lying[1] = 0xff;
    CHECK(!evo::parseWifiStation(&lying[0], lying.size(), little));
    lying = whole.bytes;
    lying[0] = 2;  // shorter than its own header
    lying[1] = 0;
    CHECK(!evo::parseWifiStation(&lying[0], lying.size(), little));

    Attrs interface;
    interface.u32(NL80211_ATTR_IFINDEX, 4);
    interface.u32(NL80211_ATTR_WIPHY_FREQ, 5180);
    unsigned mhz = 0;
    CHECK(evo::parseWifiFrequency(&interface.bytes[0], interface.bytes.size(), mhz) && mhz == 5180);
    Attrs silent;
    silent.u32(NL80211_ATTR_IFINDEX, 4);
    CHECK(!evo::parseWifiFrequency(&silent.bytes[0], silent.bytes.size(), mhz));
}

evo::WifiSample sample(uint32_t atMs, uint32_t frames, uint32_t retries, uint32_t failed, uint32_t connected,
                       uint32_t roundTripUs, uint32_t resent) {
    evo::WifiSample s;
    s.atMs = atMs;
    s.radio = true;
    s.mhz = 5180;
    s.stations = 1;
    s.hasStation = true;
    s.station = station(frames, retries, failed, connected);
    s.hasTcp = true;
    s.tcp.roundTripUs = roundTripUs;
    s.tcp.resent = resent;
    return s;
}

void logLines() {
    const evo::WifiSample first = sample(1000, 5000, 300, 2, 125, 4250, 3);
    CHECK(evo::describeWifiLink(first, nullptr) ==
          "5180 MHz; signal -52 dBm (average -51); from the phone 135.0 Mbit/s; "
          "since it joined: 5000 frames sent, 2 lost; on the Wi-Fi for 125 s; "
          "TCP round trip 4.2 ms, 3 segment(s) sent again since it opened");
    // Ten seconds later: what happened in between.
    const evo::WifiSample second = sample(11040, 5812, 335, 2, 135, 38900, 9);
    CHECK(evo::describeWifiLink(second, &first) ==
          "5180 MHz; signal -52 dBm (average -51); from the phone 135.0 Mbit/s; "
          "in 10 s: 812 frames sent, 0 lost; on the Wi-Fi for 135 s; "
          "TCP round trip 38.9 ms, 6 segment(s) sent again");
    // The phone left the Wi-Fi and came back: its counters started again, nothing is subtracted.
    const evo::WifiSample back = sample(21000, 40, 1, 0, 3, 4000, 9);
    const std::string again = evo::describeWifiLink(back, &second);
    CHECK(again.find("the phone joined the Wi-Fi again; since it joined: 40 frames sent, 0 lost") != std::string::npos);
    CHECK(again.find("on the Wi-Fi for 3 s") != std::string::npos);
    // Another phone's entry is never subtracted from either.
    evo::WifiSample other = sample(31000, 9000, 900, 9, 600, 4000, 9);
    other.station.mac[5] ^= 1;
    CHECK(evo::describeWifiLink(other, &back).find("joined the Wi-Fi again") != std::string::npos);

    // The phone is not among the stations (two phones on the access point, neither recognised).
    evo::WifiSample lost = first;
    lost.hasStation = false;
    lost.stations = 2;
    CHECK(evo::describeWifiLink(lost, &first) ==
          "5180 MHz; phone not found among the 2 station(s) of the access point; "
          "TCP round trip 4.2 ms, 0 segment(s) sent again");
    // The kernel gave no Wi-Fi figure: TCP alone. Neither: no line.
    evo::WifiSample tcpOnly;
    tcpOnly.hasTcp = true;
    tcpOnly.tcp.roundTripUs = 950;
    tcpOnly.tcp.resent = 0;
    CHECK(evo::describeWifiLink(tcpOnly, nullptr) == "TCP round trip 0.9 ms, 0 segment(s) sent again since it opened");
    CHECK(evo::describeWifiLink(evo::WifiSample(), nullptr).empty());
    // A driver without counters: no "frames" part at all.
    evo::WifiSample bare = first;
    bare.station.txFrames = bare.station.txFailed = bare.station.connectedS = evo::kNoFigure;
    bare.station.signalAvgDbm = bare.station.rxRate = evo::kNoFigure;
    bare.hasTcp = false;
    CHECK(evo::describeWifiLink(bare, nullptr) == "5180 MHz; signal -52 dBm");
    // The chip did not measure the last frame: the average alone.
    evo::WifiSample unmeasured = first;
    unmeasured.station.signalDbm = evo::kNoFigure;
    unmeasured.hasTcp = false;
    CHECK(evo::describeWifiLink(unmeasured, nullptr).find("5180 MHz; average signal -51 dBm; from the phone") == 0);
    // BMW Apps' Bluetooth slot, when the service reads it: last, after the TCP figures.
    const std::string tcpLine = "TCP round trip 0.9 ms, 0 segment(s) sent again since it opened";
    evo::WifiSample apps = tcpOnly;
    apps.apps.read = apps.apps.connected = true;
    apps.apps.state = apps.apps.transport = 1;
    CHECK(evo::describeWifiLink(apps, nullptr) == tcpLine + "; BMW Apps Bluetooth link connected");
    apps.apps.connected = false;
    apps.apps.transport = 0;
    CHECK(evo::describeWifiLink(apps, nullptr) == tcpLine + "; BMW Apps Bluetooth link idle");
    apps.apps.state = 2;
    apps.apps.transport = 1;
    CHECK(evo::describeWifiLink(apps, nullptr) == tcpLine + "; BMW Apps Bluetooth slot state 2, link state 1");
    apps.apps.read = false;
    CHECK(evo::describeWifiLink(apps, nullptr) == tcpLine);
    evo::WifiSample appsAlone;
    appsAlone.apps.read = true;
    appsAlone.apps.state = 1;
    CHECK(evo::describeWifiLink(appsAlone, nullptr) == "BMW Apps Bluetooth link idle");
}

evo::ChipCounters chip(int64_t recoveries) {
    evo::ChipCounters c;
    c.recoveries = recoveries;
    return c;
}

// The WL18xx driver's count of firmware restarts: its driver_state file, and its part of the line, after TCP and
// before BMW Apps.
void chipCounters() {
    int64_t value = -1;
    CHECK(evo::parseChipCounter("1234\n", value) && value == 1234);
    CHECK(evo::parseChipCounter(" 0 ", value) && value == 0);
    CHECK(!evo::parseChipCounter("", value) && !evo::parseChipCounter("\n", value));
    CHECK(!evo::parseChipCounter("12a\n", value) && !evo::parseChipCounter("-3\n", value));
    CHECK(!evo::parseChipCounter("1 2\n", value) && !evo::parseChipCounter("99999999999999999999\n", value));
    // driver_state as wlcore writes it: one "name = value" per line.
    const std::string state = "tx_blocks_available = 23\nchip.id = 0x1030000\nrecovery_count = 4\nrole_id = 1\n";
    CHECK(evo::parseRecoveryCount(state, value) && value == 4);
    CHECK(!evo::parseRecoveryCount("tx_blocks_available = 23\n", value));
    CHECK(!evo::parseRecoveryCount("recovery_count = many\n", value));
    CHECK(!evo::parseRecoveryCount("recovery_count_old = 2\n", value));
    // No such interface: nothing read, no counter given.
    evo::ChipCounters none = chip(1);
    CHECK(!evo::readChipCounters("entry-aad-no-such-interface", none) && none.recoveries == evo::kNoFigure);

    const std::string tcpLine = "TCP round trip 0.9 ms, 0 segment(s) sent again since it opened";
    evo::WifiSample first;
    first.atMs = 1000;
    first.hasTcp = true;
    first.tcp.roundTripUs = 950;
    first.tcp.resent = 0;
    first.hasChip = true;
    first.chip = chip(0);
    // No restart: nothing written about the chip.
    CHECK(evo::describeWifiLink(first, nullptr) == tcpLine);
    // Ten seconds later, one restart in between: written, before BMW Apps.
    evo::WifiSample second = first;
    second.atMs = 11000;
    second.chip = chip(1);
    second.apps.read = true;
    second.apps.state = 1;
    CHECK(evo::describeWifiLink(second, &first) ==
          "TCP round trip 0.9 ms, 0 segment(s) sent again; chip firmware restarted 1 time(s); BMW Apps Bluetooth "
          "link idle");
    // None since: nothing.
    evo::WifiSample third = second;
    third.atMs = 21000;
    CHECK(evo::describeWifiLink(third, &second).find("restarted") == std::string::npos);
    // Restarts before the first line: written once, with their total.
    evo::WifiSample restarted = first;
    restarted.chip = chip(2);
    CHECK(evo::describeWifiLink(restarted, nullptr) ==
          tcpLine + "; chip firmware restarted 2 time(s) since the unit started");
    // The line before had no count (not read): the total again, as on a first line.
    evo::WifiSample unread = first;
    unread.hasChip = false;
    CHECK(evo::describeWifiLink(restarted, &unread).find("2 time(s) since the unit started") != std::string::npos);
    // Nothing read: no chip part at all.
    CHECK(evo::describeWifiLink(unread, nullptr) == tcpLine);
}

// Fake reads for the reader thread, which counts them.
std::atomic<unsigned> chipReads(0);
bool fastChipRead(const std::string& interface, evo::ChipCounters& out) {
    CHECK(interface == "wlan0");
    ++chipReads;
    out = chip(3);
    return true;
}
bool failingChipRead(const std::string&, evo::ChipCounters&) {
    ++chipReads;
    return false;
}
bool slowChipRead(const std::string&, evo::ChipCounters& out) {
    ++chipReads;
    usleep(1500000);  // the driver's lock held by a stuck firmware command, longer than its 2 s timeout here
    out = chip(5);
    return true;
}

uint32_t elapsedMs(const timespec& since) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>((now.tv_sec - since.tv_sec) * 1000 + (now.tv_nsec - since.tv_nsec) / 1000000);
}

// The main loop waits for the chip's counters no longer than it asks; a read still under way is never asked again.
void chipReader() {
    {
        // Waits of a few seconds: a new thread under QEMU and load may take a while to run.
        evo::ChipCounterReader reader(fastChipRead);
        evo::ChipCounters c;
        CHECK(reader.ask("wlan0", 3000000, c) && c.recoveries == 3);
        CHECK(reader.ask("wlan0", 3000000, c) && chipReads == 2);
    }
    {
        evo::ChipCounterReader reader(failingChipRead);
        evo::ChipCounters c;
        CHECK(!reader.ask("wlan0", 3000000, c));
    }
    {
        chipReads = 0;
        evo::ChipCounterReader reader(slowChipRead);
        evo::ChipCounters c;
        timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        CHECK(!reader.ask("wlan0", 100000, c));
        CHECK(elapsedMs(start) < 1000);  // back after its 100 ms, long before the read's 1500
        clock_gettime(CLOCK_MONOTONIC, &start);
        CHECK(!reader.ask("wlan0", 3000000, c));  // still under way: not asked again, and not waited for
        CHECK(elapsedMs(start) < 700 && chipReads <= 1);
        usleep(2500000);
        CHECK(reader.ask("wlan0", 5000000, c) && c.recoveries == 5 && chipReads == 2);
        CHECK(!reader.ask("wlan0", 1000, c));  // maybe under way at its end: the destructor waits for it
    }
    CHECK(chipReads == 2 || chipReads == 3);
}

unsigned appsReads = 0;
bool countAppsReads(void* user, evo::AppsSlot& out) {
    CHECK(user == &appsReads);
    ++appsReads;
    out.connected = true;
    return true;
}

// A TCP connection on loopback: the car's end in car, the phone's in phone.
void connectPair(int& car, int& phone) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    sockaddr_in at = sockaddr_in();
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(at);
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0 && listen(listener, 1) == 0);
    CHECK(getsockname(listener, reinterpret_cast<sockaddr*>(&at), &len) == 0);
    phone = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(phone >= 0 && connect(phone, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0);
    car = accept(listener, nullptr, nullptr);
    CHECK(car >= 0);
    close(listener);
}

// The phone's end reads "closed": every copy of the car's end is gone.
bool closedForPhone(int phone) {
    pollfd p;
    p.fd = phone;
    p.events = POLLIN;
    p.revents = 0;
    char byte;
    return poll(&p, 1, 0) == 1 && recv(phone, &byte, 1, MSG_DONTWAIT) == 0;
}

void tcpFigures() {
    int car = -1, phone = -1;
    connectPair(car, phone);
    CHECK(send(car, "hello", 5, 0) == 5);
    char buffer[8];
    CHECK(recv(phone, buffer, sizeof(buffer), 0) == 5);
    evo::TcpLink link;
    CHECK(evo::readTcpLink(car, link) && link.resent == 0 && link.roundTripUs < 1000000);
    close(car);
    close(phone);
    // A USB session's local socket has no TCP figures.
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(!evo::readTcpLink(pair[0], link) && !evo::readTcpLink(-1, link));
    close(pair[0]);
    close(pair[1]);
}

void connectionCopy() {
    {  // Not enabled (production): no copy is ever kept.
        int car = -1, phone = -1;
        connectPair(car, phone);
        evo::WifiLinkLog log;
        log.connected(car);
        close(car);
        CHECK(closedForPhone(phone));
        close(phone);
    }
    {  // A session: logged while it runs, the copy given back at the first turn after its end.
        int car = -1, phone = -1;
        connectPair(car, phone);
        evo::WifiLinkLog log;
        log.enable("entry-aad-no-such-interface");  // nl80211 has nothing to say here: the TCP figures remain
        log.connected(car);
        close(car);  // as the session does at its end; the log's copy keeps the socket open
        log.turn(1000, true);
        log.turn(1100, true);
        CHECK(!closedForPhone(phone));
        log.turn(1200, false);
        CHECK(closedForPhone(phone));
        log.turn(1300, false);  // nothing left to do
        close(phone);
    }
    {  // A connection whose session never starts: kept for the start's few seconds, then given back.
        int car = -1, phone = -1;
        connectPair(car, phone);
        evo::WifiLinkLog log;
        log.enable("entry-aad-no-such-interface");
        log.connected(car);
        close(car);
        log.turn(0xfffffff0u, false);  // the 32-bit clock wraps during the wait
        log.turn(0xfffffff0u + 4000, false);
        CHECK(!closedForPhone(phone));
        log.turn(0xfffffff0u + 5000, false);
        CHECK(closedForPhone(phone));
        close(phone);
    }
    {  // BMW Apps' slot: read once per line, not at every turn of the main loop.
        int car = -1, phone = -1;
        connectPair(car, phone);
        evo::WifiLinkLog log;
        log.enable("entry-aad-no-such-interface");
        log.setAppsProbe(countAppsReads, &appsReads);
        log.connected(car);
        close(car);
        log.turn(1000, true);
        log.turn(1100, true);
        CHECK(appsReads == 1);
        log.turn(11000, true);
        CHECK(appsReads == 2);
        log.turn(11100, false);
        CHECK(appsReads == 2 && closedForPhone(phone));
        close(phone);
    }
    {  // A new connection replaces the one before; the log's end gives the last copy back.
        int car = -1, phone = -1, car2 = -1, phone2 = -1;
        connectPair(car, phone);
        connectPair(car2, phone2);
        {
            evo::WifiLinkLog log;
            log.enable("entry-aad-no-such-interface");
            log.connected(car);
            close(car);
            log.connected(car2);
            close(car2);
            CHECK(closedForPhone(phone) && !closedForPhone(phone2));
        }
        CHECK(closedForPhone(phone2));
        close(phone);
        close(phone2);
    }
}

}  // namespace

int main() {
    stationMessages();
    logLines();
    chipCounters();
    chipReader();
    tcpFigures();
    connectionCopy();
    puts(
        "PASS Wi-Fi link: station messages, log lines, the chip's counters and their reader, TCP figures, the "
        "connection's copy, BMW Apps' slot");
    return 0;
}
