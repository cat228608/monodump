#include "mono_api.h"

// EnumProcessModules / GetModuleBaseNameA live here. Must come after windows.h,
// which mono_api.h pulls in.
#include <psapi.h>

#include <cstdio>
#include <cstring>

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

bool initialize(char* out_module_name, size_t out_size) {
    HMODULE h = nullptr;
    const char* found = nullptr;

    for (const char* candidate : kCandidates) {
        h = GetModuleHandleA(candidate);
        if (h) { found = candidate; break; }
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
                    static char name[MAX_PATH];
                    GetModuleBaseNameA(GetCurrentProcess(), h, name, MAX_PATH);
                    found = name;
                    break;
                }
            }
        }
    }

    if (!h) return false;

    if (out_module_name && out_size) {
        strncpy_s(out_module_name, out_size, found ? found : "?", _TRUNCATE);
    }

    g_missing = 0;
#define MONO_RESOLVE(ret, name, args)                                          \
    name = reinterpret_cast<t_##name>(GetProcAddress(h, #name));               \
    if (!name) ++g_missing;
    MONO_FUNCTION_LIST(MONO_RESOLVE)
#undef MONO_RESOLVE

    // The absolute minimum needed to walk metadata.
    if (!mono_get_root_domain || !mono_thread_attach ||
        !mono_domain_assembly_foreach || !mono_assembly_get_image ||
        !mono_image_get_table_rows || !mono_class_get) {
        return false;
    }

    // Every thread that touches Mono must be attached or the GC will kill you.
    mono_thread_attach(mono_get_root_domain());
    return true;
}

int missing_count() { return g_missing; }

} // namespace mono
