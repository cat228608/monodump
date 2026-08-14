#include "dispatch.h"
#include "log.h"
#include "mono_api.h"

#include <windows.h>

#ifdef MONODUMP_WITH_MINHOOK
#include "MinHook.h"
#endif

namespace dispatch {

namespace {

const int kSlots = 16;
const int kIgnored = 16;

struct Slot {
    Job    fn   = nullptr;
    void*  arg  = nullptr;
    HANDLE done = nullptr;
    volatile LONG state = 0;   // 0 free, 1 queued, 2 running
};

Slot            g_slots[kSlots];
CRITICAL_SECTION g_cs;
bool            g_cs_ready = false;
bool            g_installed = false;

DWORD           g_ignored[kIgnored] = {};
volatile LONG   g_ignored_count = 0;

volatile LONG   g_main_tid = 0;
volatile LONG   g_draining = 0;
volatile LONG64 g_invokes = 0;

bool IsIgnored(DWORD tid) {
    const LONG n = g_ignored_count;
    for (LONG i = 0; i < n && i < kIgnored; ++i)
        if (g_ignored[i] == tid) return true;
    return false;
}

void Drain() {
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        if (InterlockedCompareExchange(&s.state, 2, 1) != 1) continue;
        Job fn = s.fn;
        void* arg = s.arg;
        if (fn) fn(arg);
        if (s.done) SetEvent(s.done);
        InterlockedExchange(&s.state, 0);
    }
}

#ifdef MONODUMP_WITH_MINHOOK
typedef MonoObject* (*t_invoke)(MonoMethod*, void*, void**, MonoObject**);
t_invoke g_orig_invoke = nullptr;

MonoObject* Hooked(MonoMethod* method, void* obj, void** args, MonoObject** exc) {
    const DWORD tid = GetCurrentThreadId();
    InterlockedIncrement64(&g_invokes);

    if (!IsIgnored(tid)) {
        // First non-worker thread to enter the runtime is the main thread: Unity
        // drives Update() through here every single frame.
        if (g_main_tid == 0) InterlockedCompareExchange(&g_main_tid, (LONG)tid, 0);

        if ((DWORD)g_main_tid == tid &&
            InterlockedCompareExchange(&g_draining, 1, 0) == 0) {
            // Re-entrancy guard: jobs invoke managed code themselves and would
            // otherwise recurse into their own queue.
            Drain();
            InterlockedExchange(&g_draining, 0);
        }
    }
    return g_orig_invoke(method, obj, args, exc);
}
#endif

} // namespace

bool Available() {
#ifdef MONODUMP_WITH_MINHOOK
    return true;
#else
    return false;
#endif
}

void IgnoreCurrentThread() {
    const DWORD tid = GetCurrentThreadId();
    if (IsIgnored(tid)) return;
    const LONG idx = InterlockedIncrement(&g_ignored_count) - 1;
    if (idx >= 0 && idx < kIgnored) g_ignored[idx] = tid;
}

bool Install() {
#ifndef MONODUMP_WITH_MINHOOK
    return false;
#else
    if (g_installed) return true;
    if (!mono_runtime_invoke) {
        mdlog::Printf("[monodump] main-thread dispatch off: mono_runtime_invoke not resolved\n");
        return false;
    }

    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = true; }
    for (int i = 0; i < kSlots; ++i)
        if (!g_slots[i].done) g_slots[i].done = CreateEventA(nullptr, TRUE, FALSE, nullptr);

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        mdlog::Printf("[monodump] MinHook init failed - main-thread dispatch off\n");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(mono_runtime_invoke),
                      reinterpret_cast<void*>(&Hooked),
                      reinterpret_cast<void**>(&g_orig_invoke)) != MH_OK) {
        mdlog::Printf("[monodump] could not hook mono_runtime_invoke - dispatch off\n");
        return false;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(mono_runtime_invoke)) != MH_OK) {
        mdlog::Printf("[monodump] could not enable the mono_runtime_invoke hook\n");
        return false;
    }

    g_installed = true;
    mdlog::Printf("[monodump] main-thread dispatch armed (hook on mono_runtime_invoke)\n");
    return true;
#endif
}

void Uninstall() {
#ifdef MONODUMP_WITH_MINHOOK
    if (!g_installed) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_installed = false;
#endif
}

bool Ready() { return g_installed && g_main_tid != 0; }

unsigned long MainThreadId() { return (unsigned long)g_main_tid; }
unsigned long long InvokeCount() { return (unsigned long long)g_invokes; }

bool Run(Job fn, void* arg, unsigned timeout_ms) {
    if (!Ready() || !fn) return false;

    Slot* slot = nullptr;
    if (g_cs_ready) EnterCriticalSection(&g_cs);
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].state != 0 || !g_slots[i].done) continue;
        g_slots[i].fn = fn;
        g_slots[i].arg = arg;
        ResetEvent(g_slots[i].done);
        InterlockedExchange(&g_slots[i].state, 1);
        slot = &g_slots[i];
        break;
    }
    if (g_cs_ready) LeaveCriticalSection(&g_cs);

    if (!slot) return false;   // queue full: caller falls back to its own thread

    if (WaitForSingleObject(slot->done, timeout_ms) == WAIT_OBJECT_0) return true;

    // Timed out. If the job has not started yet we can safely take it back.
    if (InterlockedCompareExchange(&slot->state, 0, 1) == 1) return false;

    // It is already running on the main thread - let it finish, do not race it.
    if (WaitForSingleObject(slot->done, 5000) == WAIT_OBJECT_0) return true;
    return false;
}

} // namespace dispatch
