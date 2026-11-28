// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// hmiapp/vtable_check.hpp: the slots the app replaces must hold the exported functions, else no app is created.
#include <stdio.h>

#include "../hmiapp/vtable_check.hpp"

namespace {

using evo::app::VtableSlot;
using evo::app::vtableSlotsHold;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

int onInit = 0, onFinalize = 0, other = 0;  // stand-ins for the functions' addresses

}  // namespace

int main() {
    void* table[34] = {};
    table[22] = &onInit;
    table[23] = &onFinalize;
    const VtableSlot expected[] = {{22, &onInit}, {23, &onFinalize}};
    CHECK(vtableSlotsHold(table, expected, 2));  // the car's layout
    void* shifted[34] = {};                      // one virtual function added before them in another library
    shifted[23] = &onInit;
    shifted[24] = &onFinalize;
    shifted[22] = &other;
    CHECK(!vtableSlotsHold(shifted, expected, 2));
    const VtableSlot missing[] = {{22, &onInit}, {23, nullptr}};  // the library does not export onFinalize
    CHECK(!vtableSlotsHold(table, missing, 2));
    CHECK(!vtableSlotsHold(nullptr, expected, 2));
    if (g_failures) {
        printf("FAIL vtable check: %d checks failed\n", g_failures);
        return 1;
    }
    puts("PASS vtable check: the car's layout passes; a moved slot, a missing symbol or no vtable refuses the app");
    return 0;
}
