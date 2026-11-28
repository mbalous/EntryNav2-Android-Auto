// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The app's posted tasks run marked, and iDrive's per-thread lookups are refused inside one, on that thread only.
#include <pthread.h>

#include <cstdio>
#include <cstdlib>

#include "../hmiapp/posted_task.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace {

using evo::app::idriveLookupSafe;
using evo::app::inPostedTask;
using evo::app::PostedTask;
using evo::app::runPostedTask;

// Laid out as the app's runnable (hmiapp/bmw_framework.hpp AppRunnable): what BMW reads, then the task's function.
struct Task {
    void** vptr;
    void* vtbl[5];
    void (*run)(void*);
    int runs;
    bool marked, safe, otherThreadSafe;
};

void* otherThread(void*) {
    static bool safe;
    safe = idriveLookupSafe();
    return &safe;
}

void taskBody(void* self) {
    Task* t = static_cast<Task*>(self);
    ++t->runs;
    t->marked = inPostedTask();
    t->safe = idriveLookupSafe();
    // Another thread meanwhile, iDrive's handlers for instance: not marked by this task.
    pthread_t th;
    void* result = nullptr;
    CHECK(pthread_create(&th, nullptr, &otherThread, nullptr) == 0 && pthread_join(th, &result) == 0);
    t->otherThreadSafe = *static_cast<bool*>(result);
    {
        PostedTask inner;  // one task inside another: still marked when the inner one ends
    }
    CHECK(inPostedTask());
}

}  // namespace

int main() {
    CHECK(idriveLookupSafe() && !inPostedTask());  // onInit, iDrive's handlers
    Task t = Task();
    t.run = &taskBody;
    // As BMW's dispatcher calls a runnable's run(): the function in its vtable, given the runnable.
    void (*dispatched)(void*) = &runPostedTask<Task>;
    dispatched(&t);
    CHECK(t.runs == 1 && t.marked && !t.safe && t.otherThreadSafe);
    CHECK(idriveLookupSafe() && !inPostedTask());  // after the task
    dispatched(&t);
    CHECK(t.runs == 2 && !t.safe && idriveLookupSafe());
    puts("PASS posted tasks: marked while they run, iDrive lookups refused inside, other threads and after unaffected");
    return 0;
}
