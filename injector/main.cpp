//
// monodump.exe - the launcher you asked for.
//
//   monodump.exe --process Game.exe [--filter money] [--assembly Assembly-CSharp]
//                [--compile] [--out C:\dumps]
//
// Resolves the process by name, checks bitness, writes an options file next to
// the payload DLL, then injects it with the classic
// VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryA) dance.
//

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

static bool EqualsNoCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && _stricmp(a.c_str(), b.c_str()) == 0;
}

struct ProcEntry { DWORD pid; std::string name; };

static std::vector<ProcEntry> Snapshot() {
    std::vector<ProcEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do { out.push_back({ pe.th32ProcessID, pe.szExeFile }); }
        while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

static DWORD FindPid(const std::string& name) {
    for (const auto& p : Snapshot())
        if (EqualsNoCase(p.name, name)) return p.pid;
    return 0;
}

// A 32-bit injector cannot inject a 64-bit process or vice versa. Catching this
// up front saves a lot of confusion - it is the single most common failure.
static bool BitnessMatches(HANDLE proc, bool* target_is_64) {
    BOOL target_wow64 = FALSE, self_wow64 = FALSE;
    IsWow64Process(proc, &target_wow64);
    IsWow64Process(GetCurrentProcess(), &self_wow64);

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const bool os64 = si.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_INTEL;

    const bool target64 = os64 && !target_wow64;
    const bool self64   = os64 && !self_wow64;
    if (target_is_64) *target_is_64 = target64;
    return target64 == self64;
}

static bool HasMonoModule(DWORD pid, std::string* found) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    bool result = false;
    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            std::string n = me.szModule;
            for (auto& c : n) c = (char)tolower(c);
            if (n.find("mono") != std::string::npos) {
                if (found) *found = me.szModule;
                result = true;
                break;
            }
            if (n.find("gameassembly") != std::string::npos) {
                if (found) *found = "GameAssembly.dll (IL2CPP - not Mono)";
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return result;
}

static bool Inject(DWORD pid, const std::string& dll) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        printf("[-] OpenProcess failed (%lu). Run as administrator.\n", GetLastError());
        return false;
    }

    bool target64 = false;
    if (!BitnessMatches(proc, &target64)) {
        printf("[-] bitness mismatch: target is %d-bit, injector is %d-bit.\n",
               target64 ? 64 : 32, (int)(sizeof(void*) == 8 ? 64 : 32));
        printf("    Rebuild both the injector and the payload for %d-bit.\n",
               target64 ? 64 : 32);
        CloseHandle(proc);
        return false;
    }

    const SIZE_T len = dll.size() + 1;
    LPVOID remote = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        printf("[-] VirtualAllocEx failed (%lu)\n", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote, dll.c_str(), len, nullptr)) {
        printf("[-] WriteProcessMemory failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    // kernel32 sits at the same base in every process of the same bitness,
    // so our LoadLibraryA address is valid in the target too.
    auto loadlib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadlib, remote, 0, nullptr);
    if (!thread) {
        printf("[-] CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, 10000);

    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    if (exit_code == 0)
        printf("[!] LoadLibraryA returned NULL - the DLL failed to load.\n"
               "    Usual cause: wrong bitness, or a missing MSVC runtime.\n"
               "    Build the payload with /MT to link the CRT statically.\n");

    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return exit_code != 0;
}

static void Usage() {
    printf(
        "monodump - enumerate Mono classes, fields and methods in a running game\n\n"
        "  monodump.exe --process <name.exe> [options]\n\n"
        "  --process <name>     target process (e.g. Game.exe)\n"
        "  --pid <n>            target by pid instead\n"
        "  --dll <path>         payload dll (default: monodump_payload.dll next to exe)\n"
        "  --assembly <substr>  only assemblies matching this (e.g. Assembly-CSharp)\n"
        "  --filter <substr>    only classes/fields/methods matching this\n"
        "  --out <dir>          output directory (default: next to the dll)\n"
        "  --compile            JIT every method to record its native address (slow)\n"
        "  --values             read current static field values; forces class\n"
        "                       initialisation and CAN CRASH the game (off by default)\n"
        "  --no-values          default; metadata only, safest\n"
        "  --no-props           skip properties and nested types (smaller json)\n"
        "  --no-text            json only, skip the human-readable dump.txt\n"
        "  --oneshot            unload after dumping instead of staying resident\n"
        "  --no-dump            skip the metadata walk, only start the invoker\n"
        "  --quiet              no console inside the game; log to monodump.log\n"
        "  --no-mainthread      run commands on the watcher thread (unsafe, debug only)\n"
        "  --list               list processes that have a mono module loaded\n"
        "\nAfter injecting the payload stays loaded and watches monodump_cmd.txt.\n"
        "Write a line into it and save to call a method or poke a static field:\n"
        "  call RecipeManager::UnlockAll()\n"
        "  set  PlayerStats::money 999999\n");
}

int main(int argc, char** argv) {
    std::string process, dll, out, assembly, filter;
    DWORD pid = 0;
    bool compile = false, values = false, list = false;
    bool oneshot = false, text = true, props = true;
    bool do_dump = true, console = true, mainthread = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--process")    process  = next();
        else if (a == "--pid")        pid      = strtoul(next().c_str(), nullptr, 10);
        else if (a == "--dll")        dll      = next();
        else if (a == "--out")        out      = next();
        else if (a == "--assembly")   assembly = next();
        else if (a == "--filter")     filter   = next();
        else if (a == "--compile")    compile  = true;
        else if (a == "--values")     values   = true;
        else if (a == "--no-values")  values   = false;
        else if (a == "--list")       list     = true;
        else if (a == "--oneshot")    oneshot  = true;
        else if (a == "--no-text")    text     = false;
        else if (a == "--no-props")   props    = false;
        else if (a == "--no-dump")    do_dump  = false;
        else if (a == "--quiet")      console  = false;
        else if (a == "--no-mainthread") mainthread = false;
        else { Usage(); return 1; }
    }

    if (list) {
        printf("%-8s %-32s %s\n", "PID", "PROCESS", "RUNTIME");
        for (const auto& p : Snapshot()) {
            std::string mod;
            if (HasMonoModule(p.pid, &mod))
                printf("%-8lu %-32s %s\n", p.pid, p.name.c_str(), mod.c_str());
        }
        return 0;
    }

    if (process.empty() && pid == 0) { Usage(); return 1; }

    if (pid == 0) {
        pid = FindPid(process);
        if (!pid) { printf("[-] process '%s' not found\n", process.c_str()); return 1; }
    }
    printf("[+] target pid %lu\n", pid);

    std::string mono_module;
    if (!HasMonoModule(pid, &mono_module)) {
        printf("[!] no mono module in that process.\n");
        if (!mono_module.empty()) printf("    found instead: %s\n", mono_module.c_str());
        printf("    Injecting anyway - the payload will re-check.\n");
    } else {
        printf("[+] runtime module: %s\n", mono_module.c_str());
    }

    if (dll.empty()) {
        char self[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string s(self);
        dll = s.substr(0, s.find_last_of('\\') + 1) + "monodump_payload.dll";
    }

    char full[MAX_PATH] = {};
    if (!GetFullPathNameA(dll.c_str(), MAX_PATH, full, nullptr)) {
        printf("[-] cannot resolve dll path\n");
        return 1;
    }
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        printf("[-] payload not found: %s\n", full);
        return 1;
    }
    printf("[+] payload: %s\n", full);

    // hand options to the payload through a file beside the dll
    std::string dll_dir(full);
    dll_dir = dll_dir.substr(0, dll_dir.find_last_of('\\'));
    const std::string ini = dll_dir + "\\monodump.ini";

    FILE* f = nullptr;
    if (fopen_s(&f, ini.c_str(), "w") == 0 && f) {
        fprintf(f, "out=%s\n", out.empty() ? dll_dir.c_str() : out.c_str());
        fprintf(f, "assembly=%s\n", assembly.c_str());
        fprintf(f, "filter=%s\n", filter.c_str());
        fprintf(f, "compile=%d\n", compile ? 1 : 0);
        fprintf(f, "values=%d\n", values ? 1 : 0);
        fprintf(f, "json=1\n");
        fprintf(f, "text=%d\n", text ? 1 : 0);
        fprintf(f, "properties=%d\n", props ? 1 : 0);
        fprintf(f, "interactive=%d\n", oneshot ? 0 : 1);
        fprintf(f, "dump=%d\n", do_dump ? 1 : 0);
        fprintf(f, "console=%d\n", console ? 1 : 0);
        fprintf(f, "log=1\n");
        fprintf(f, "mainthread=%d\n", mainthread ? 1 : 0);
        fclose(f);
    }

    if (!Inject(pid, full)) return 1;

    if (console)
        printf("[+] injected. A console window opened in the game process with the log.\n");
    else
        printf("[+] injected quietly. Log: %s\\monodump.log\n", dll_dir.c_str());
    printf("[+] handshake file: %s\\monodump_ready.txt (appears once the runtime is bound)\n",
           dll_dir.c_str());
    printf("[+] dump.txt / dump.json will appear in %s\n",
           out.empty() ? dll_dir.c_str() : out.c_str());
    if (!oneshot) {
        printf("[+] payload stays resident; edit %s\\monodump_cmd.txt to invoke methods.\n",
               dll_dir.c_str());
        printf("[+] results of each batch land in %s\\monodump_result.txt\n", dll_dir.c_str());
    }
    return 0;
}
