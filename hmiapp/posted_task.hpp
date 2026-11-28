// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Whether the app's code runs inside one of its own posted tasks, where iDrive's per-thread lookups are not safe.
#ifndef ENTRY_AAD_HMIAPP_POSTED_TASK_HPP
#define ENTRY_AAD_HMIAPP_POSTED_TASK_HPP

// iDrive finds its services through hmiplugin::get<...>(), whose instance cache (CPluginInstanceCacheTLS in HmiMain3D)
// is kept per thread. The app asks it in onInit and in iDrive's handlers (idrive_coding.cpp), where it has worked in
// cars. Another project asked it from a task posted to the app (bmw::PluginApp::post, run on a bmw::Dispatcher thread):
// that thread stopped, HmiMain3D died and the head unit restarted. So the app's tasks run marked, and a lookup asked
// inside one is refused: the caller keeps what it already knew.

namespace evo {
namespace app {

namespace posted_task_detail {
inline int& depth() {
    static __thread int d = 0;  // this thread's posted tasks running, one inside another counted
    return d;
}
}  // namespace posted_task_detail

// Marks this thread as running one of the app's posted tasks, for the scope.
class PostedTask {
public:
    PostedTask() { ++posted_task_detail::depth(); }
    ~PostedTask() { --posted_task_detail::depth(); }

private:
    PostedTask(const PostedTask&);
    PostedTask& operator=(const PostedTask&);
};

inline bool inPostedTask() {
    return posted_task_detail::depth() > 0;
}

// True when iDrive's per-thread lookups may be asked here: not inside a posted task.
inline bool idriveLookupSafe() {
    return !inPostedTask();
}

// The run() BMW's dispatcher calls for a posted task: the task's own function (its run member), on a marked thread.
template <typename Task>
void runPostedTask(void* self) {
    PostedTask marked;
    static_cast<Task*>(self)->run(self);
}

}  // namespace app
}  // namespace evo

#endif  // ENTRY_AAD_HMIAPP_POSTED_TASK_HPP
