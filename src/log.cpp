#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <windows.h>

namespace mdlog {

static std::string     g_dir;
static bool            g_console = true;
static bool            g_file    = true;
static FILE*           g_log     = nullptr;
static FILE*           g_result  = nullptr;
static FILE*           g_crumb   = nullptr;
static std::string     g_seq;
static int             g_ok = 0, g_fail = 0;
static CRITICAL_SECTION g_cs;
static bool            g_cs_ready = false;

const char* Version() { return "1.1.3"; }

static void Lock()   { if (g_cs_ready) EnterCriticalSection(&g_cs); }
static void Unlock() { if (g_cs_ready) LeaveCriticalSection(&g_cs); }

static void Sink(const char* text) {
    if (g_console) { fputs(text, stdout); fflush(stdout); }
    if (g_log)     { fputs(text, g_log);  fflush(g_log); }
    if (g_result)  { fputs(text, g_result); fflush(g_result); }
}

void Init(const std::string& dir, bool console, bool file) {
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = true; }
    g_dir = dir;
    g_console = console;
    g_file = file;

    if (g_file && !g_log) {
        // "a" and not "w": two injections into the same game should not erase
        // the evidence from the first one.
        fopen_s(&g_log, (dir + "\\monodump.log").c_str(), "a");
        if (g_log) {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            fprintf(g_log, "\n==== monodump %s payload attached %04d-%02d-%02d %02d:%02d:%02d pid=%lu ====\n",
                    Version(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    GetCurrentProcessId());
            fflush(g_log);
        }
    }

    // "w": only the last breadcrumb matters, and it must survive a hard crash,
    // so the handle stays open and every write is flushed.
    if (g_file && !g_crumb)
        fopen_s(&g_crumb, (dir + "\\monodump_last.txt").c_str(), "w");
}

void Crumb(const char* what) {
    if (!g_crumb || !what) return;
    Lock();
    fseek(g_crumb, 0, SEEK_SET);
    // Padded so leftovers of a longer previous name cannot trail the new one.
    fprintf(g_crumb, "%-160s\n", what);
    fflush(g_crumb);
    Unlock();
}

void Printf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    Lock();
    if (g_console) { fputs(buf, stdout); fflush(stdout); }
    if (g_log)     { fputs(buf, g_log);  fflush(g_log); }
    Unlock();
}

void BeginBatch(const std::string& seq) {
    Lock();
    g_seq = seq;
    g_ok = g_fail = 0;
    if (g_result) { fclose(g_result); g_result = nullptr; }
    // Written fresh per batch, so a caller never mistakes an old answer for a new one.
    fopen_s(&g_result, (g_dir + "\\monodump_result.txt").c_str(), "w");
    if (g_result) {
        fprintf(g_result, "seq=%s\n", seq.empty() ? "-" : seq.c_str());
        fflush(g_result);
    }
    Unlock();
}

void EndBatch() {
    Lock();
    char tail[256];
    _snprintf_s(tail, sizeof(tail), _TRUNCATE, "end seq=%s ok=%d fail=%d\n",
                g_seq.empty() ? "-" : g_seq.c_str(), g_ok, g_fail);
    Sink(tail);
    if (g_result) { fclose(g_result); g_result = nullptr; }
    g_seq.clear();
    Unlock();
}

static void Line(const char* prefix, const char* fmt, va_list ap) {
    char body[4096];
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);

    char full[4200];
    _snprintf_s(full, sizeof(full), _TRUNCATE, "%s %s\n", prefix, body);

    Lock();
    Sink(full);
    Unlock();
}

void Ok(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    Line("[+]", fmt, ap);
    va_end(ap);
    Lock(); ++g_ok; Unlock();
}

void Fail(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    Line("[-]", fmt, ap);
    va_end(ap);
    Lock(); ++g_fail; Unlock();
}

void WriteReady(const std::string& runtime, bool mainthread, bool interactive) {
    FILE* f = nullptr;
    if (fopen_s(&f, (g_dir + "\\monodump_ready.txt").c_str(), "w") != 0 || !f) return;
    fprintf(f, "version=%s\n", Version());
    fprintf(f, "pid=%lu\n", GetCurrentProcessId());
    fprintf(f, "bits=%d\n", (int)(sizeof(void*) * 8));
    fprintf(f, "runtime=%s\n", runtime.c_str());
    fprintf(f, "mainthread=%d\n", mainthread ? 1 : 0);
    fprintf(f, "interactive=%d\n", interactive ? 1 : 0);
    fclose(f);
}

void RemoveReady() {
    DeleteFileA((g_dir + "\\monodump_ready.txt").c_str());
}

} // namespace mdlog
