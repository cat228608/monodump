#pragma once
//
// dispatch.h - run work on the game's main thread.
//
// WHY THIS EXISTS
// Unity is not thread safe. Most of UnityEngine (instantiating objects, touching
// components, UI, scene loading) must run on the thread that runs Update().
// v1.0 invoked managed methods straight from its watcher thread, which is why
// some calls did nothing and others crashed the game at random.
//
// HOW IT WORKS
// We hook the runtime's own mono_runtime_invoke. Unity calls every managed
// Update/Coroutine through it, on the main thread, dozens of times per frame.
// So the hook is a free, guaranteed-correct "once per frame on the main thread"
// callback: when it fires on the main thread we drain a small job queue and then
// call the original. No managed assembly is injected, nothing is patched
// permanently, and removing the hook restores the runtime exactly.
//
// Needs MinHook (BSD-2). If it is not present the project still builds:
// Available() returns false and the invoker falls back to its own thread with a
// loud warning.
//

namespace dispatch {

typedef void (*Job)(void* arg);

// True when this build has MinHook compiled in.
bool Available();

// Installs the mono_runtime_invoke hook. Safe to call twice.
bool Install();
void Uninstall();

// True when the hook is installed AND a main thread has been identified.
bool Ready();

// Threads that must never be mistaken for the main thread (our own workers).
void IgnoreCurrentThread();

// Queues fn(arg) and blocks until the main thread has run it.
// Returns false on timeout (game paused, minimised, or no managed activity).
bool Run(Job fn, void* arg, unsigned timeout_ms);

// Diagnostics.
unsigned long MainThreadId();
unsigned long long InvokeCount();

} // namespace dispatch
