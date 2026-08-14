//
// dllmain.cpp - the payload. This half runs INSIDE the game process.
//
// Everything in mono_api.h can only be called from the game's address space,
// because those are ordinary C functions operating on the runtime's private
// heap. An external process cannot call them, so the "exe that takes a process
// name" has to inject this DLL and let it do the work.
//
// v1.1 changes, all driven by driving the payload from a tool instead of by hand:
//   * dump=0        - skip the metadata walk entirely and only start the invoker
//   * console=0     - no AllocConsole (no window over the game, and closing that
//                     window can no longer kill the game); output goes to files
//   * mainthread=1  - run commands on the game's main thread (see dispatch.h)
//   * a single-instance mutex, so a second injection cannot double-execute
//     every command
//   * monodump_ready.txt handshake and monodump_result.txt answers (see log.h)
//   * clean shutdown: the watcher thread is stopped and the hook removed BEFORE
//     the DLL unloads. v1.0 left a thread running inside freed code.
//

#include "dispatch.h"
#include "dumper.h"
#include "invoker.h"
#include "log.h"
#include "mono_api.h"

#include <cstdio>
#include <string>
#include <windows.h>

static std::string DirOfThisModule(HMODULE self) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path);
    const size_t slash = s.find_last_of('\\');
    return (slash == std::string::npos) ? "." : s.substr(0, slash);
}

struct PayloadConfig {
    DumpOptions dump;
    bool        run_dump    = true;   // dump=0 for tools that only want the invoker
    bool        interactive = true;   // keep the invoker running after the dump
    bool        console     = true;   // AllocConsole + printf
    bool        log_file    = true;   // monodump.log
    bool        main_thread = true;   // execute commands on the game's main thread
};

// The injector drops an options file next to the DLL before injecting, because
// CreateRemoteThread(LoadLibraryA) gives us no way to pass argv.
static PayloadConfig LoadOptions(const std::string& dir) {
    PayloadConfig cfg;
    cfg.dump.out_dir = dir;

    FILE* f = nullptr;
    if (fopen_s(&f, (dir + "\\monodump.ini").c_str(), "r") != 0 || !f) return cfg;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        if      (k == "out")         cfg.dump.out_dir = v;
        else if (k == "assembly")    cfg.dump.assembly_filter = v;
        else if (k == "filter")      cfg.dump.name_filter = v;
        else if (k == "compile")     cfg.dump.compile_methods = (v == "1");
        else if (k == "values")      cfg.dump.static_values   = (v == "1");
        else if (k == "json")        cfg.dump.json            = (v == "1");
        else if (k == "text")        cfg.dump.text            = (v == "1");
        else if (k == "properties")  cfg.dump.properties      = (v == "1");
        else if (k == "interactive") cfg.interactive          = (v == "1");
        else if (k == "dump")        cfg.run_dump             = (v == "1");
        else if (k == "console")     cfg.console              = (v == "1");
        else if (k == "log")         cfg.log_file             = (v == "1");
        else if (k == "mainthread")  cfg.main_thread          = (v == "1");
    }
    fclose(f);
    return cfg;
}

// Lets a controlling tool ask for a clean unload instead of killing the game or
// leaving the payload resident forever.
static std::string UnloadEventName() {
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Local\\monodump_unload_%lu", GetCurrentProcessId());
    return buf;
}

static std::string InstanceMutexName() {
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Local\\monodump_payload_%lu", GetCurrentProcessId());
    return buf;
}

static HANDLE g_unload = nullptr;

// Reads one line from the console. Returns false if there is no readable
// console input at all (EOF).
//
// This mattered a lot: AllocConsole only gave us stdout/stderr, stdin was left
// unbound, so getchar() returned EOF immediately instead of blocking. The
// waiter below then "saw an Enter" microseconds after injection and unloaded
// the payload - the invoker stopped before any command could be picked up.
static bool ReadEnter() {
    int c = getchar();
    if (c == EOF) return false;
    while (c != '\n' && c != EOF) c = getchar();
    return true;
}

static DWORD WINAPI ConsoleWaiter(LPVOID) {
    // Enter in the console means "unload", same as v1.0, but now it goes through
    // the same orderly shutdown path as a tool request.
    if (!ReadEnter()) {
        // No usable stdin: this console cannot ask for an unload. Stay resident
        // and let the controlling tool signal the unload event instead.
        mdlog::Printf("[monodump] console has no readable input; "
                      "unload only via the tool (Enter will not work here)\n");
        return 0;
    }
    mdlog::Printf("[monodump] unload requested from the console\n");
    if (g_unload) SetEvent(g_unload);
    return 0;
}

// The metadata walk as a dispatch job: dispatch::Run takes a plain function
// pointer and runs it on the game's main thread, so this is just a thin shim.
static void DumpJob(void* arg) {
    RunDump(*static_cast<const DumpOptions*>(arg));
}

static DWORD WINAPI Worker(LPVOID param) {
    auto self = static_cast<HMODULE>(param);

    // One payload per process. A second injection used to allocate a second
    // console and start a second watcher on the same command file, so every
    // command ran twice.
    HANDLE once = CreateMutexA(nullptr, TRUE, InstanceMutexName().c_str());
    if (!once || GetLastError() == ERROR_ALREADY_EXISTS) {
        FreeLibraryAndExitThread(self, 0);
        return 0;
    }

    const std::string dir = DirOfThisModule(self);
    const PayloadConfig cfg = LoadOptions(dir);

    if (cfg.console) {
        AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        // Without this stdin stays unbound and every getchar() returns EOF at
        // once, which used to look like "the user pressed Enter immediately".
        freopen_s(&dummy, "CONIN$", "r", stdin);
        SetConsoleTitleA("monodump");
    } else if (cfg.log_file) {
        // No console: the dumper's own printf output still has to land somewhere.
        FILE* dummy = nullptr;
        freopen_s(&dummy, (dir + "\\monodump_stdout.log").c_str(), "a", stdout);
        freopen_s(&dummy, (dir + "\\monodump_stdout.log").c_str(), "a", stderr);
    }

    mdlog::Init(dir, cfg.console, cfg.log_file);
    mdlog::Printf("[monodump] payload %s, %d-bit, pid %lu\n",
                  mdlog::Version(), (int)(sizeof(void*) * 8), GetCurrentProcessId());

    char module_name[MAX_PATH] = {};
    if (!mono::initialize(module_name, sizeof(module_name))) {
        mdlog::Printf("[monodump] no Mono runtime found in this process.\n");
        mdlog::Printf("[monodump] If this is Unity, check <Game>_Data\\il2cpp_data - if that\n");
        mdlog::Printf("           folder exists the game is IL2CPP and this tool does not\n");
        mdlog::Printf("           apply. Use Il2CppDumper instead.\n");
        if (cfg.console) {
            mdlog::Printf("\nPress Enter to unload.\n");
            ReadEnter();
        }
        ReleaseMutex(once);
        CloseHandle(once);
        FreeLibraryAndExitThread(self, 0);
    }

    mdlog::Printf("[monodump] runtime: %s\n", module_name);
    mdlog::Printf("[monodump] static values: %s\n",
                  cfg.dump.static_values ? "ON (--values; can run cctors)" : "off");
    mdlog::Printf("[monodump] %d exports could not be resolved (older runtime = more misses)\n",
                  mono::missing_count());
    mdlog::Printf("[monodump] out=%s assembly=%s filter=%s compile=%d dump=%d console=%d mainthread=%d\n",
                  cfg.dump.out_dir.c_str(),
                  cfg.dump.assembly_filter.empty() ? "*" : cfg.dump.assembly_filter.c_str(),
                  cfg.dump.name_filter.empty() ? "*" : cfg.dump.name_filter.c_str(),
                  (int)cfg.dump.compile_methods, (int)cfg.run_dump,
                  (int)cfg.console, (int)cfg.main_thread);

    if (cfg.run_dump) {
        const DWORD t0 = GetTickCount();
        bool on_main = false;

        // Since 1.1.2 the metadata walk itself goes through the main thread when
        // it can. Walking classes calls mono_class_init/mono_class_vtable, which
        // can run managed code; doing that from our own thread while Unity is
        // rendering is exactly how the game vanishes mid-dump. The game freezes
        // for the duration of the walk instead - that is the correct trade.
        if (cfg.main_thread && dispatch::Available()) {
            dispatch::IgnoreCurrentThread();
            if (dispatch::Install()) {
                for (int i = 0; i < 200 && !dispatch::Ready(); ++i) Sleep(50);
                if (dispatch::Ready()) {
                    mdlog::Printf("[monodump] dumping on the game main thread (tid=%lu), "
                                  "the game will freeze until it finishes\n",
                                  dispatch::MainThreadId());
                    // dispatch::Run speaks plain void*, and cfg is const here.
                    // DumpJob only reads the options back, so dropping const for
                    // the trip through the queue is safe.
                    on_main = dispatch::Run(DumpJob,
                                            const_cast<DumpOptions*>(&cfg.dump),
                                            900000);
                    if (!on_main)
                        mdlog::Printf("[monodump] the main thread never took the job - "
                                      "is the game paused or minimised?\n");
                } else {
                    mdlog::Printf("[monodump] no managed activity seen in 10 s - "
                                  "load into the world, then dump\n");
                }
            }
        }

        if (!on_main) {
            mdlog::Printf("[monodump] dumping on our own thread "
                          "(mainthread=0, no MinHook, or no main thread found)\n");
            RunDump(cfg.dump);
        }

        mdlog::Printf("[monodump] dump done in %lu ms\n", GetTickCount() - t0);
    } else {
        // A tool that only wants to fire commands pays nothing for metadata it
        // already has on disk. v1.0 always walked every class first.
        mdlog::Printf("[monodump] dump=0, skipping the metadata walk\n");
    }

    g_unload = CreateEventA(nullptr, TRUE, FALSE, UnloadEventName().c_str());

    if (cfg.interactive) {
        if (cfg.main_thread) dispatch::Install();

        // Stay resident so you can act on what the dump just told you without
        // rebuilding and re-injecting for every experiment.
        StartInvoker(dir, cfg.main_thread);
        mdlog::WriteReady(module_name, cfg.main_thread && dispatch::Available(), true);

        if (cfg.console) {
            mdlog::Printf("\n[monodump] payload stays loaded. Press Enter to unload.\n");
            mdlog::Printf("[monodump] do NOT close this console window with the X - that kills the game.\n");
            mdlog::Printf("[monodump] resident. waiting for commands.\n");
            CreateThread(nullptr, 0, ConsoleWaiter, nullptr, 0, nullptr);
        }

        WaitForSingleObject(g_unload, INFINITE);

        // Orderly teardown: stop the watcher, then remove the hook, then leave.
        StopInvoker();
        dispatch::Uninstall();
        mdlog::RemoveReady();
        mdlog::Printf("[monodump] payload unloading\n");
    } else {
        // --oneshot: the dump is done and we are about to unload. If the dump ran
        // on the main thread, our trampoline is sitting inside mono_runtime_invoke.
        // Leaving it there and unloading the DLL means the next managed call jumps
        // into freed memory. THIS is what killed the game AFTER dump.json was
        // already written.
        dispatch::Uninstall();
        mdlog::WriteReady(module_name, false, false);
        if (cfg.console) {
            mdlog::Printf("\nPress Enter to unload.\n");
            ReadEnter();
        }
        mdlog::RemoveReady();
    }

    if (g_unload) { CloseHandle(g_unload); g_unload = nullptr; }
    ReleaseMutex(once);
    CloseHandle(once);

    FreeLibraryAndExitThread(self, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // Never do real work in DllMain - the loader lock is held. Spawn a thread.
        CreateThread(nullptr, 0, Worker, hModule, 0, nullptr);
    }
    return TRUE;
}
