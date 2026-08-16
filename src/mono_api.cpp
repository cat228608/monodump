#include "mono_api.h"

// EnumProcessModules / GetModuleBaseNameA live here. Must come after windows.h,
// which mono_api.h pulls in.
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdarg>

// define the function pointers
#define MONO_DEFINE(ret, name, args) t_##name name = nullptr;
MONO_FUNCTION_LIST(MONO_DEFINE)
#undef MONO_DEFINE

namespace mono {

static int g_missing = 0;

// Unity has shipped the runtime under all of these names over the years.
static const char* kCandidates[] = {
    "mono-2.0-bdwgc.dll",   // Unity 2018+
    "monobdwgc-2.0.dll",
    "mono-2.0-sgen.dll",
    "mono.dll",             // older Unity / standalone Mono
    "libmono.so",
    "mono-2.0.dll",
};

static void SetDetail(char* out, size_t size, const char* fmt, ...) {
    if (!out || !size) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(out, size, _TRUNCATE, fmt, ap);
    va_end(ap);
}

BindStatus initialize_ex(char* out_module_name, size_t out_size,
                         char* out_detail, size_t detail_size) {
    HMODULE h = nullptr;
    static char resolved_name[MAX_PATH];
    resolved_name[0] = 0;

    // A name match is not proof: a module can be called mono-2.0-bdwgc.dll and
    // still not export what we need (proxy/stub dlls, and Unity ships a few
    // builds with a differently exporting runtime). Require the export too, and
    // fall through to the full scan when the named module does not have it.
    for (const char* candidate : kCandidates) {
        HMODULE c = GetModuleHandleA(candidate);
        if (c && GetProcAddress(c, "mono_get_root_domain")) {
            h = c;
            strncpy_s(resolved_name, candidate, _TRUNCATE);
            break;
        }
        if (c && !h) {
            // Remember it so the failure message can name the impostor.
            strncpy_s(resolved_name, candidate, _TRUNCATE);
        }
    }

    // Fallback: scan every loaded module for one exporting mono_get_root_domain.
    // Catches renamed/repacked runtimes that none of the names above match.
    if (!h) {
        HMODULE mods[1024];
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
            const size_t count = needed / sizeof(HMODULE);
            for (size_t i = 0; i < count; ++i) {
                if (GetProcAddress(mods[i], "mono_get_root_domain")) {
                    h = mods[i];
                    GetModuleBaseNameA(GetCurrentProcess(), h, resolved_name, MAX_PATH);
                    break;
                }
            }
        }
    }

    if (!h) {
        if (resolved_name[0])
            SetDetail(out_detail, detail_size,
                      "%s is loaded but does not export mono_get_root_domain "
                      "(stub/proxy dll, or a runtime this build cannot use)",
                      resolved_name);
        else
            SetDetail(out_detail, detail_size,
                      "no loaded module exports mono_get_root_domain");
        return BindStatus::NoModule;
    }

    if (out_module_name && out_size)
        strncpy_s(out_module_name, out_size, resolved_name[0] ? resolved_name : "?", _TRUNCATE);

    g_missing = 0;
#define MONO_RESOLVE(ret, name, args)                                          \
    name = reinterpret_cast<t_##name>(GetProcAddress(h, #name));               \
    if (!name) ++g_missing;
    MONO_FUNCTION_LIST(MONO_RESOLVE)
#undef MONO_RESOLVE

    // The absolute minimum needed to walk metadata. Report every missing name:
    // guessing which one it was is what made the old message useless.
    {
        char miss[512];
        miss[0] = 0;
        size_t used = 0;
        auto note = [&](const char* n, void* p) {
            if (p) return;
            const int w = _snprintf_s(miss + used, sizeof(miss) - used, _TRUNCATE,
                                      "%s%s", used ? ", " : "", n);
            if (w > 0) used += (size_t)w;
        };
        note("mono_get_root_domain",         (void*)mono_get_root_domain);
        note("mono_thread_attach",           (void*)mono_thread_attach);
        // Either enumeration export is fine; only the absence of BOTH is fatal.
        if (!mono_domain_assembly_foreach && !mono_assembly_foreach)
            note("mono_domain_assembly_foreach/mono_assembly_foreach", nullptr);
        note("mono_assembly_get_image",      (void*)mono_assembly_get_image);
        note("mono_image_get_table_rows",    (void*)mono_image_get_table_rows);
        note("mono_class_get",              (void*)mono_class_get);

        if (used) {
            SetDetail(out_detail, detail_size,
                      "%s is missing required exports: %s (%d of the full table absent)",
                      resolved_name, miss, g_missing);
            return BindStatus::MissingExports;
        }
    }

    // Module and exports are fine, but the runtime may not have created its root
    // domain yet. Attaching to a null domain is how you crash a game on startup.
    MonoDomain* domain = mono_get_root_domain();
    if (!domain) {
        SetDetail(out_detail, detail_size,
                  "%s bound, but mono_get_root_domain() is still null "
                  "(runtime not initialised yet)", resolved_name);
        return BindStatus::NoRootDomain;
    }

    // Every thread that touches Mono must be attached or the GC will kill you.
    mono_thread_attach(domain);
    SetDetail(out_detail, detail_size, "%s, enum via %s, %d optional exports absent",
              resolved_name,
              mono_domain_assembly_foreach ? "mono_domain_assembly_foreach"
                                           : "mono_assembly_foreach",
              g_missing);
    return BindStatus::Ok;
}

bool foreach_assembly(MonoGFunc fn, void* user) {
    if (mono_domain_assembly_foreach) {
        mono_domain_assembly_foreach(mono_get_root_domain(), fn, user);
        return true;
    }
    if (mono_assembly_foreach) {
        mono_assembly_foreach(fn, user);
        return true;
    }
    return false;
}

bool initialize(char* out_module_name, size_t out_size) {
    return initialize_ex(out_module_name, out_size, nullptr, 0) == BindStatus::Ok;
}

const char* modules_summary() {
    static char buf[1024];
    buf[0] = 0;

    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return "<module list unavailable>";

    const size_t count = needed / sizeof(HMODULE);
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        char name[MAX_PATH] = {};
        if (!GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;

        char lower[MAX_PATH];
        strncpy_s(lower, name, _TRUNCATE);
        for (char* p = lower; *p; ++p) *p = (char)tolower((unsigned char)*p);

        const bool interesting = strstr(lower, "mono")   || strstr(lower, "unity") ||
                                 strstr(lower, "player") || strstr(lower, "gameassembly") ||
                                 strstr(lower, "coreclr");
        if (!interesting) continue;

        const int wrote = _snprintf_s(buf + used, sizeof(buf) - used, _TRUNCATE,
                                      "%s%s", used ? ", " : "", name);
        if (wrote <= 0) break;
        used += (size_t)wrote;
        if (used > sizeof(buf) - 64) break;
    }

    if (!used)
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "nothing runtime-like among %zu modules", count);
    else
        _snprintf_s(buf + used, sizeof(buf) - used, _TRUNCATE,
                    " (of %zu modules total)", count);
    return buf;
}

int missing_count() { return g_missing; }

} // namespace mono
