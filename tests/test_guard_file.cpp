// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// hmiapp/guard_file.hpp: the guards' files count only recent dates, and are replaced whole.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "../hmiapp/guard_file.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace {

using evo::app::guardTurn;
using evo::app::readGuardDates;
using evo::app::writeGuardDates;

const long kWindow = 15 * 60;
const int kMax = 3;
const long kNow = 1795000000;  // a date in 2026

std::string g_dir;

std::string path(const char* name) {
    return g_dir + "/" + name;
}

void put(const std::string& file, const std::string& text) {
    FILE* f = fopen(file.c_str(), "w");
    CHECK(f && fputs(text.c_str(), f) >= 0);
    fclose(f);
}

std::string get(const std::string& file) {
    FILE* f = fopen(file.c_str(), "r");
    if (!f) {
        return "(missing)";
    }
    std::string text;
    char b[256];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) {
        text.append(b, n);
    }
    fclose(f);
    return text;
}

std::string lines(long a, long b = 0, long c = 0) {
    char text[96];
    snprintf(text, sizeof(text), "%ld\n", a);
    std::string all = text;
    for (long v : {b, c}) {
        if (v) {
            snprintf(text, sizeof(text), "%ld\n", v);
            all += text;
        }
    }
    return all;
}

bool exists(const std::string& file) {
    struct stat st;
    return stat(file.c_str(), &st) == 0;
}

// Code review of 25/11 (L1): every line counted, however old. Three short trips, even days apart, turned the app off
// until someone removed the file over SSH.
void oldLoadsDoNotCount() {
    const std::string f = path("app-guard");
    const long twoDays = 2 * 24 * 3600;
    put(f, lines(kNow - twoDays - 600, kNow - twoDays - 300, kNow - twoDays));
    int recent = -1;
    CHECK(guardTurn(f.c_str(), kNow, kWindow, kMax, &recent));
    CHECK(recent == 0);
    CHECK(get(f) == lines(kNow));  // the old ones dropped, this load counted
    CHECK(!exists(f + ".tmp"));
    puts("PASS loads of other days do not count");
}

void crashLoop() {
    const std::string f = path("loop");
    int recent = -1;
    unlink(f.c_str());
    CHECK(guardTurn(f.c_str(), kNow, kWindow, kMax, &recent) && recent == 0);  // no file yet
    CHECK(guardTurn(f.c_str(), kNow + 70, kWindow, kMax, &recent) && recent == 1);
    CHECK(guardTurn(f.c_str(), kNow + 140, kWindow, kMax, &recent) && recent == 2);
    CHECK(get(f) == lines(kNow, kNow + 70, kNow + 140));
    CHECK(!guardTurn(f.c_str(), kNow + 210, kWindow, kMax, &recent) && recent == 3);  // the third short run: off
    CHECK(get(f) == lines(kNow, kNow + 70, kNow + 140));                              // left as it is
    CHECK(!guardTurn(f.c_str(), kNow + kWindow - 1, kWindow, kMax, &recent) && recent == 3);
    // 15 minutes after the first load: it no longer counts, the app loads again
    CHECK(guardTurn(f.c_str(), kNow + kWindow, kWindow, kMax, &recent) && recent == 2);
    CHECK(get(f) == lines(kNow + 70, kNow + 140, kNow + kWindow));
    puts("PASS 3 loads within 15 minutes turn the app off, until the first is 15 minutes old");
}

void dateMovedBack() {
    const std::string f = path("back");
    // A line 5 minutes ahead (the date stepped back a little since) still counts; one a day ahead does not.
    put(f, lines(kNow + 300, kNow - 60, kNow + 24 * 3600));
    int recent = -1;
    CHECK(guardTurn(f.c_str(), kNow, kWindow, kMax, &recent) && recent == 2);
    CHECK(get(f) == lines(kNow + 300, kNow - 60, kNow));
    // On the car, long is 32 bits: a date far from now must not overflow the difference.
    put(f, "-2147483647\n2147483647\n");
    CHECK(guardTurn(f.c_str(), 2147483000L, kWindow, kMax, &recent) && recent == 1);
    puts("PASS a date a little ahead counts, one far ahead or far behind does not");
}

void notADate() {
    const std::string f = path("cut");
    char good[32];
    snprintf(good, sizeof(good), "%ld\n", kNow - 10);
    // A line cut by a power loss while an older version appended it, an empty line, text: none of them counts.
    put(f, std::string("17952") + "x\n\nhello\n" + good + good);
    int recent = -1;
    CHECK(guardTurn(f.c_str(), kNow, kWindow, kMax, &recent) && recent == 2);
    CHECK(get(f) == lines(kNow - 10, kNow - 10, kNow));
    long dates[evo::app::kGuardDatesMax];
    std::string many;
    for (int i = 0; i < 12; ++i) {
        many += good;
    }
    put(f, many);
    CHECK(readGuardDates(f.c_str(), kNow, kWindow, dates) == evo::app::kGuardDatesMax);  // at most 8 read
    puts("PASS a line that is not a date is ignored, 8 dates at most");
}

void replacedWhole() {
    const std::string f = path("whole");
    const long dates[] = {kNow - 5, kNow};
    put(f + ".tmp", "left by a power loss\n");  // a write cut before its rename
    put(f, lines(kNow - 900));
    CHECK(writeGuardDates(f.c_str(), dates, 2));
    CHECK(get(f) == lines(kNow - 5, kNow) && !exists(f + ".tmp"));
    // It cannot be written (no such folder): false, nothing left behind; the guard still lets the start go.
    const std::string nowhere = path("missing/app-guard");
    CHECK(!writeGuardDates(nowhere.c_str(), dates, 2) && !exists(nowhere));
    int recent = -1;
    CHECK(guardTurn(nowhere.c_str(), kNow, kWindow, kMax, &recent) && recent == 0);
    puts("PASS written beside, then renamed over the old file; a file that cannot be written stops nothing");
}

}  // namespace

int main() {
    char dir[] = "/tmp/guard-file-XXXXXX";
    CHECK(mkdtemp(dir));
    g_dir = dir;
    oldLoadsDoNotCount();
    crashLoop();
    dateMovedBack();
    notADate();
    replacedWhole();
    const char* names[] = {"app-guard", "loop", "back", "cut", "whole"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        unlink(path(names[i]).c_str());
    }
    rmdir(dir);
    puts("PASS guard file: recent dates only, replaced whole");
    return 0;
}
