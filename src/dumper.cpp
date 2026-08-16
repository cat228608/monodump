#include "dumper.h"
#include "log.h"
#include "mono_api.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------- small utils

static std::string OwnedString(char* p) {
    if (!p) return {};
    std::string s(p);
    if (mono_free) mono_free(p);
    return s;
}

static std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); out += b; }
                else out += static_cast<char>(c);
        }
    }
    return out;
}

static bool Contains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return tolower(a) == tolower(b); });
    return it != hay.end();
}

static std::string Hex(uint32_t v) {
    char b[16];
    sprintf_s(b, "0x%08X", v);
    return b;
}

// ---------------------------------------------------------------- data model

struct ParamInfo {
    std::string name, type;
};

struct FieldInfo {
    std::string name, type;
    uint32_t    offset = 0;
    uint32_t    flags  = 0;
    bool        is_static = false;
    bool        is_const  = false;
    bool        is_readonly = false;
    bool        is_public = false;
    std::string static_value;   // rendered only for simple static primitives
    uintptr_t   static_addr = 0;
};

struct MethodInfo {
    std::string name, full_name, signature, ret;
    std::vector<ParamInfo> params;
    uint32_t    flags = 0;
    uint32_t    token = 0;
    bool        is_static = false;
    bool        is_virtual = false;
    bool        is_abstract = false;
    bool        is_public = false;
    bool        is_special = false;   // get_/set_/op_/.ctor
    uintptr_t   jit_addr = 0;         // only when --compile
};

struct PropertyInfo {
    std::string name, type;
    bool has_get = false, has_set = false;
    uint32_t get_token = 0, set_token = 0;
};

struct ClassInfo {
    std::string ns, name, full_name, parent;
    std::vector<std::string> interfaces;
    std::vector<std::string> nested;
    uint32_t    token = 0;
    uint32_t    flags = 0;
    int32_t     instance_size = 0;
    bool        is_enum = false, is_valuetype = false;
    bool        is_interface = false, is_abstract = false, is_public = false;
    std::vector<FieldInfo>    fields;
    std::vector<MethodInfo>   methods;
    std::vector<PropertyInfo> properties;
};

struct AssemblyInfo {
    std::string image_name, file, runtime_version;
    std::vector<ClassInfo> classes;
};

struct Collector {
    const DumpOptions*         opts;
    std::vector<AssemblyInfo>* out;
    int  errors = 0;
};

// ------------------------------------------------------- static field reading

// Reads the current value of a static primitive. This is the part the forum
// snippet gets wrong: it casts the static data pointer to DWORD, which
// truncates on x64 and hands you a garbage address. Use uintptr_t.
struct StaticSlot { uintptr_t addr; char raw[8]; bool ok; };

// mono_class_vtable initialises the class, and initialising a class runs its
// static constructor - arbitrary managed code - on OUR thread. In a Unity game
// plenty of cctors touch UnityEngine and take the whole process down. POD-only
// frame, so a fault costs one field instead of the game.
static void SehStaticSlot(MonoClass* klass, MonoClassField* field,
                          uintptr_t offset, StaticSlot* out) {
    out->addr = 0;
    out->ok   = false;
    memset(out->raw, 0, sizeof(out->raw));
    __try {
        MonoVTable* vt = mono_class_vtable(mono_get_root_domain(), klass);
        if (!vt) return;
        void* base = mono_vtable_get_static_field_data(vt);
        if (!base) return;
        out->addr = reinterpret_cast<uintptr_t>(base) + offset;
        if (mono_field_static_get_value)
            mono_field_static_get_value(vt, field, out->raw);
        else
            memcpy(out->raw, reinterpret_cast<void*>(out->addr), sizeof(out->raw));
        out->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->ok = false;
    }
}

static void ReadStaticField(MonoClass* klass, MonoClassField* field,
                            const std::string& type, FieldInfo& fi) {
    if (!mono_class_vtable || !mono_vtable_get_static_field_data) return;

    StaticSlot slot;
    SehStaticSlot(klass, field, static_cast<uintptr_t>(fi.offset), &slot);
    if (slot.addr) fi.static_addr = slot.addr;
    if (!slot.ok) return;

    char* buf = slot.raw;

    char rendered[64] = {};
    if      (type == "System.Int32")   sprintf_s(rendered, "%d",   *reinterpret_cast<int32_t*>(buf));
    else if (type == "System.UInt32")  sprintf_s(rendered, "%u",   *reinterpret_cast<uint32_t*>(buf));
    else if (type == "System.Int64")   sprintf_s(rendered, "%lld", *reinterpret_cast<int64_t*>(buf));
    else if (type == "System.Single")  sprintf_s(rendered, "%g",   *reinterpret_cast<float*>(buf));
    else if (type == "System.Double")  sprintf_s(rendered, "%g",   *reinterpret_cast<double*>(buf));
    else if (type == "System.Boolean") sprintf_s(rendered, "%s",   *buf ? "true" : "false");
    else return;

    fi.static_value = rendered;
}

// ------------------------------------------------------------ class walking

static std::string ClassFullName(MonoClass* k) {
    if (!k) return {};
    const char* ns = mono_class_get_namespace ? mono_class_get_namespace(k) : "";
    const char* n  = mono_class_get_name ? mono_class_get_name(k) : "?";
    std::string s;
    if (ns && *ns) { s = ns; s += "."; }
    s += (n ? n : "?");
    return s;
}

// --------------------------------------------------------------- SEH shells
// MSVC (C2712) refuses __try inside any function that owns objects needing
// unwinding - WalkClass and OnAssembly are full of std::string/std::vector, so
// every guarded runtime call has to live in its own POD-only shell.

static bool SehParamNames(MonoMethod* m, const char** names) {
    __try {
        mono_method_get_param_names(m, names);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uintptr_t SehCompile(MonoMethod* m) {
    __try {
        return reinterpret_cast<uintptr_t>(mono_compile_method(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void WalkClass(MonoClass* klass, const DumpOptions& opts, ClassInfo& ci) {
    const char* ns   = mono_class_get_namespace ? mono_class_get_namespace(klass) : "";
    const char* name = mono_class_get_name ? mono_class_get_name(klass) : "?";
    ci.ns        = ns   ? ns   : "";
    ci.name      = name ? name : "?";
    ci.full_name = ClassFullName(klass);

    if (mono_class_get_type_token) ci.token = mono_class_get_type_token(klass);
    if (mono_class_get_flags) {
        ci.flags        = mono_class_get_flags(klass);
        ci.is_interface = (ci.flags & TYPE_ATTR_INTERFACE) != 0;
        ci.is_abstract  = (ci.flags & TYPE_ATTR_ABSTRACT) != 0;
        ci.is_public    = (ci.flags & TYPE_ATTR_VISIBILITY_MASK) == TYPE_ATTR_PUBLIC;
    }
    if (mono_class_is_enum)      ci.is_enum      = mono_class_is_enum(klass) != 0;
    if (mono_class_is_valuetype) ci.is_valuetype = mono_class_is_valuetype(klass) != 0;
    if (mono_class_get_parent)   ci.parent       = ClassFullName(mono_class_get_parent(klass));
    if (mono_class_instance_size) ci.instance_size = mono_class_instance_size(klass);

    // --- interfaces --- (tells you at a glance if a class is serializable,
    // enumerable, or implements a game-specific contract like ISaveable)
    if (mono_class_get_interfaces) {
        void* iter = nullptr;
        while (MonoClass* itf = mono_class_get_interfaces(klass, &iter))
            ci.interfaces.push_back(ClassFullName(itf));
    }

    // --- nested types ---
    if (opts.properties && mono_class_get_nested_types) {
        void* iter = nullptr;
        while (MonoClass* nt = mono_class_get_nested_types(klass, &iter))
            ci.nested.push_back(ClassFullName(nt));
    }

    // --- fields ---
    if (mono_class_get_fields) {
        void* iter = nullptr;
        while (MonoClassField* f = mono_class_get_fields(klass, &iter)) {
            FieldInfo fi;
            const char* fname = mono_field_get_name ? mono_field_get_name(f) : nullptr;
            fi.name = fname ? fname : "?";

            if (mono_field_get_type && mono_type_get_name) {
                if (MonoType* t = mono_field_get_type(f))
                    fi.type = OwnedString(mono_type_get_name(t));
            }
            if (mono_field_get_offset) fi.offset = mono_field_get_offset(f);
            if (mono_field_get_flags)  fi.flags  = mono_field_get_flags(f);
            fi.is_static   = (fi.flags & FIELD_ATTR_STATIC) != 0;
            fi.is_const    = (fi.flags & FIELD_ATTR_LITERAL) != 0;
            fi.is_readonly = (fi.flags & FIELD_ATTR_INIT_ONLY) != 0;
            fi.is_public   = (fi.flags & FIELD_ATTR_FIELD_ACCESS_MASK) == FIELD_ATTR_PUBLIC;

            // Literals (const) have no storage - reading them is meaningless.
            if (opts.static_values && fi.is_static && !fi.is_const)
                ReadStaticField(klass, f, fi.type, fi);

            ci.fields.push_back(std::move(fi));
        }
    }

    // --- methods ---
    if (mono_class_get_methods) {
        void* iter = nullptr;
        while (MonoMethod* m = mono_class_get_methods(klass, &iter)) {
            MethodInfo mi;
            const char* mname = mono_method_get_name ? mono_method_get_name(m) : nullptr;
            mi.name = mname ? mname : "?";

            // The metadata token is the bridge to dnSpy: Edit > Go to MD Token.
            if (mono_method_get_token) mi.token = mono_method_get_token(m);
            if (mono_method_full_name) mi.full_name = OwnedString(mono_method_full_name(m, 1));

            if (mono_method_get_flags) {
                uint32_t iflags = 0;
                mi.flags       = mono_method_get_flags(m, &iflags);
                mi.is_static   = (mi.flags & METHOD_ATTR_STATIC) != 0;
                mi.is_virtual  = (mi.flags & METHOD_ATTR_VIRTUAL) != 0;
                mi.is_abstract = (mi.flags & METHOD_ATTR_ABSTRACT) != 0;
                mi.is_special  = (mi.flags & METHOD_ATTR_SPECIAL_NAME) != 0;
                mi.is_public   = (mi.flags & METHOD_ATTR_ACCESS_MASK) == METHOD_ATTR_PUBLIC;
            }

            if (mono_method_signature) {
                if (MonoMethodSignature* sig = mono_method_signature(m)) {
                    uint32_t pc = mono_signature_get_param_count
                                ? mono_signature_get_param_count(sig) : 0;
                    if (mono_signature_get_desc)
                        mi.signature = OwnedString(mono_signature_get_desc(sig, 1));
                    if (mono_signature_get_return_type && mono_type_get_name) {
                        if (MonoType* rt = mono_signature_get_return_type(sig))
                            mi.ret = OwnedString(mono_type_get_name(rt));
                    }

                    // Parameter types...
                    if (mono_signature_get_params && mono_type_get_name) {
                        void* pit = nullptr;
                        while (MonoType* pt = mono_signature_get_params(sig, &pit)) {
                            ParamInfo pi;
                            pi.type = OwnedString(mono_type_get_name(pt));
                            mi.params.push_back(std::move(pi));
                        }
                    }
                    // ...and their names, which is what makes the JSON readable.
                    // The buffer must hold exactly param_count entries.
                    if (pc > 0 && pc == mi.params.size() && mono_method_get_param_names) {
                        std::vector<const char*> names(pc, nullptr);
                        if (SehParamNames(m, names.data())) {
                            for (uint32_t i = 0; i < pc; ++i)
                                if (names[i]) mi.params[i].name = names[i];
                        }
                    }
                }
            }

            // Forcing a JIT compile of every method is slow and can throw for
            // abstract/generic methods. Off by default, and guarded.
            if (opts.compile_methods && mono_compile_method && !mi.is_abstract) {
                mi.jit_addr = SehCompile(m);
            }

            ci.methods.push_back(std::move(mi));
        }
    }

    // --- properties ---
    // Worth having: a lot of Unity game state is exposed as `public int Money
    // { get; set; }`, which shows up here as get_Money/set_Money rather than a
    // field you can poke directly.
    if (opts.properties && mono_class_get_properties) {
        void* iter = nullptr;
        while (MonoProperty* p = mono_class_get_properties(klass, &iter)) {
            PropertyInfo pi;
            const char* pn = mono_property_get_name ? mono_property_get_name(p) : nullptr;
            pi.name = pn ? pn : "?";

            if (mono_property_get_get_method) {
                if (MonoMethod* g = mono_property_get_get_method(p)) {
                    pi.has_get = true;
                    if (mono_method_get_token) pi.get_token = mono_method_get_token(g);
                    if (mono_method_signature && mono_signature_get_return_type && mono_type_get_name) {
                        if (MonoMethodSignature* s = mono_method_signature(g))
                            if (MonoType* rt = mono_signature_get_return_type(s))
                                pi.type = OwnedString(mono_type_get_name(rt));
                    }
                }
            }
            if (mono_property_get_set_method) {
                if (MonoMethod* s = mono_property_get_set_method(p)) {
                    pi.has_set = true;
                    if (mono_method_get_token) pi.set_token = mono_method_get_token(s);
                }
            }
            ci.properties.push_back(std::move(pi));
        }
    }
}

// Filtering happens after the walk so that --filter money still shows you the
// whole class the match lives in. A field name alone is useless; you need the
// surrounding class to find it in dnSpy.
// Broken/obfuscated types can fault inside mono_class_init or deeper; the
// __try has to sit here because OnAssembly itself owns unwindable objects.
static bool SehWalkClass(MonoClass* klass, const DumpOptions& opts, ClassInfo& ci) {
    __try {
        if (mono_class_init) mono_class_init(klass);
        WalkClass(klass, opts, ci);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ClassMatches(const ClassInfo& ci, const std::string& f) {
    if (f.empty()) return true;
    if (Contains(ci.full_name, f)) return true;
    for (const auto& x : ci.fields)     if (Contains(x.name, f)) return true;
    for (const auto& x : ci.methods)    if (Contains(x.name, f)) return true;
    for (const auto& x : ci.properties) if (Contains(x.name, f)) return true;
    return false;
}

// Compiler-generated types always carry angle brackets in their name:
//   <WaitForSceneSwapClient>d__40   async/iterator state machine
//   <>c, <>c__DisplayClass3_0       lambda caches
//   <PrivateImplementationDetails>  static data blob
// None of them are useful for a trainer and they are the most crash-prone
// classes in the image, so 1.1.5 skips them unless --include-generated is set.
static bool IsGeneratedName(const char* name) {
    if (!name) return false;
    for (const char* p = name; *p; ++p)
        if (*p == '<' || *p == '>') return true;
    return false;
}

// --skip "Foo,Bar": manual escape hatch when monodump_last.txt names a class
// that kills the game and it is not a generated one.
static bool NameInSkipList(const std::string& name, const std::string& list) {
    if (list.empty()) return false;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        std::string item = list.substr(start, comma - start);
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back()  == ' ') item.pop_back();
        if (!item.empty() && Contains(name, item)) return true;
        start = comma + 1;
    }
    return false;
}

// ------------------------------------------------------- assembly enumeration

static void OnAssembly(void* data, void* user) {
    auto* asmb = static_cast<MonoAssembly*>(data);
    auto* col  = static_cast<Collector*>(user);
    const DumpOptions& opts = *col->opts;

    MonoImage* image = mono_assembly_get_image(asmb);
    if (!image) return;

    AssemblyInfo ai;
    if (mono_image_get_name)     { const char* n = mono_image_get_name(image);     ai.image_name = n ? n : "?"; }
    if (mono_image_get_filename) { const char* f = mono_image_get_filename(image); ai.file = f ? f : ""; }
    if (mono_image_get_version)  { const char* v = mono_image_get_version(image);  ai.runtime_version = v ? v : ""; }

    if (!Contains(ai.image_name, opts.assembly_filter)) return;

    const int rows = mono_image_get_table_rows(image, MONO_TABLE_TYPEDEF);
    // Row 1 is <Module>, a pseudo-type. Real types start at 2.
    int skipped = 0;
    for (int i = 2; i <= rows; ++i) {
        MonoClass* klass = mono_class_get(image, MONO_TOKEN_TYPE_DEF | i);
        if (!klass) continue;

        // Name first, decide second: skipping happens before mono_class_init, which
        // is the call that actually crashes on state machines.
        const char* nm = mono_class_get_name ? mono_class_get_name(klass) : nullptr;
        const std::string cname = nm ? nm : "?";

        if (opts.skip_generated && IsGeneratedName(nm)) { ++skipped; continue; }
        if (NameInSkipList(cname, opts.skip_types))     { ++skipped; continue; }

        // Breadcrumb before we touch the class: SEH cannot catch a GC crash, so
        // when the game dies anyway monodump_last.txt still names the culprit and
        // you can skip past it with --skip / --filter / --assembly.
        {
            char crumb[192];
            sprintf_s(crumb, "%s : type %d/%d : %s", ai.image_name.c_str(), i, rows,
                      cname.c_str());
            mdlog::Crumb(crumb);
        }

        ClassInfo ci;
        if (!SehWalkClass(klass, opts, ci)) {
            ++col->errors;
            continue;
        }

        if (!ClassMatches(ci, opts.name_filter)) continue;
        ai.classes.push_back(std::move(ci));
    }

    if (skipped)
        mdlog::Printf("[monodump] %s: %d generated/skipped types not walked\n",
                      ai.image_name.c_str(), skipped);

    col->out->push_back(std::move(ai));
}

// ------------------------------------------------------------------- writers

static void WriteText(FILE* f, const std::vector<AssemblyInfo>& asms) {
    for (const auto& a : asms) {
        fprintf(f, "================================================================\n");
        fprintf(f, "assembly %s\n", a.image_name.c_str());
        if (!a.file.empty()) fprintf(f, "  file %s\n", a.file.c_str());
        fprintf(f, "  %zu classes\n\n", a.classes.size());

        for (const auto& c : a.classes) {
            fprintf(f, "class %s", c.full_name.c_str());
            if (!c.parent.empty()) fprintf(f, " : %s", c.parent.c_str());
            for (size_t i = 0; i < c.interfaces.size(); ++i)
                fprintf(f, "%s %s", (c.parent.empty() && i == 0) ? " :" : ",", c.interfaces[i].c_str());
            fprintf(f, "   // token %s, instance size %d%s\n",
                    Hex(c.token).c_str(), c.instance_size, c.is_enum ? ", enum" : "");

            for (const auto& fl : c.fields) {
                fprintf(f, "    [0x%04X] %-9s %-40s %s",
                        fl.offset,
                        fl.is_const ? "const" : (fl.is_static ? "static" : ""),
                        fl.type.c_str(),
                        fl.name.c_str());
                if (fl.static_addr) fprintf(f, "   @0x%llX", (unsigned long long)fl.static_addr);
                if (!fl.static_value.empty()) fprintf(f, " = %s", fl.static_value.c_str());
                fprintf(f, "\n");
            }
            for (const auto& p : c.properties) {
                fprintf(f, "    prop     %-40s %s { %s%s}\n",
                        p.type.c_str(), p.name.c_str(),
                        p.has_get ? "get; " : "", p.has_set ? "set; " : "");
            }
            for (const auto& m : c.methods) {
                fprintf(f, "    %-8s %-24s %s(", m.is_static ? "static" : "",
                        m.ret.c_str(), m.name.c_str());
                for (size_t i = 0; i < m.params.size(); ++i)
                    fprintf(f, "%s%s %s", i ? ", " : "",
                            m.params[i].type.c_str(), m.params[i].name.c_str());
                fprintf(f, ")   // %s", Hex(m.token).c_str());
                if (m.jit_addr) fprintf(f, " -> 0x%llX", (unsigned long long)m.jit_addr);
                fprintf(f, "\n");
            }
            fprintf(f, "\n");
        }
    }
}

static void WriteStringArray(FILE* f, const std::vector<std::string>& v) {
    fprintf(f, "[");
    for (size_t i = 0; i < v.size(); ++i)
        fprintf(f, "%s\"%s\"", i ? "," : "", JsonEscape(v[i]).c_str());
    fprintf(f, "]");
}

static void WriteJson(FILE* f, const std::vector<AssemblyInfo>& asms,
                      const DumpOptions& opts, int errors) {
    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": 2,\n");
    fprintf(f, "  \"generator\": \"monodump\",\n");
    fprintf(f, "  \"note\": \"method.token is an ECMA-335 metadata token; paste it into dnSpy via Edit > Go to MD Token\",\n");
    fprintf(f, "  \"filters\": {\"assembly\": \"%s\", \"name\": \"%s\", \"compiled\": %s},\n",
            JsonEscape(opts.assembly_filter).c_str(),
            JsonEscape(opts.name_filter).c_str(),
            opts.compile_methods ? "true" : "false");
    fprintf(f, "  \"skipped_classes\": %d,\n", errors);
    fprintf(f, "  \"assemblies\": [\n");

    for (size_t ai = 0; ai < asms.size(); ++ai) {
        const auto& a = asms[ai];
        fprintf(f, "    {\n");
        fprintf(f, "      \"name\": \"%s\",\n", JsonEscape(a.image_name).c_str());
        fprintf(f, "      \"file\": \"%s\",\n", JsonEscape(a.file).c_str());
        fprintf(f, "      \"runtime\": \"%s\",\n", JsonEscape(a.runtime_version).c_str());
        fprintf(f, "      \"classes\": [\n");

        for (size_t ci = 0; ci < a.classes.size(); ++ci) {
            const auto& c = a.classes[ci];
            fprintf(f, "        {\n");
            fprintf(f, "          \"namespace\": \"%s\",\n", JsonEscape(c.ns).c_str());
            fprintf(f, "          \"name\": \"%s\",\n", JsonEscape(c.name).c_str());
            fprintf(f, "          \"full_name\": \"%s\",\n", JsonEscape(c.full_name).c_str());
            fprintf(f, "          \"token\": \"%s\",\n", Hex(c.token).c_str());
            fprintf(f, "          \"parent\": \"%s\",\n", JsonEscape(c.parent).c_str());
            fprintf(f, "          \"interfaces\": "); WriteStringArray(f, c.interfaces); fprintf(f, ",\n");
            fprintf(f, "          \"nested\": ");     WriteStringArray(f, c.nested);     fprintf(f, ",\n");
            fprintf(f, "          \"instance_size\": %d,\n", c.instance_size);
            fprintf(f, "          \"is_enum\": %s, \"is_valuetype\": %s, \"is_interface\": %s, \"is_abstract\": %s, \"is_public\": %s,\n",
                    c.is_enum ? "true" : "false", c.is_valuetype ? "true" : "false",
                    c.is_interface ? "true" : "false", c.is_abstract ? "true" : "false",
                    c.is_public ? "true" : "false");

            // fields
            fprintf(f, "          \"fields\": [\n");
            for (size_t i = 0; i < c.fields.size(); ++i) {
                const auto& x = c.fields[i];
                fprintf(f, "            {\"name\": \"%s\", \"type\": \"%s\", \"offset\": %u, \"offset_hex\": \"0x%X\", "
                           "\"static\": %s, \"const\": %s, \"readonly\": %s, \"public\": %s, "
                           "\"static_addr\": \"0x%llX\", \"value\": \"%s\"}%s\n",
                        JsonEscape(x.name).c_str(), JsonEscape(x.type).c_str(),
                        x.offset, x.offset,
                        x.is_static ? "true" : "false", x.is_const ? "true" : "false",
                        x.is_readonly ? "true" : "false", x.is_public ? "true" : "false",
                        (unsigned long long)x.static_addr,
                        JsonEscape(x.static_value).c_str(),
                        (i + 1 < c.fields.size()) ? "," : "");
            }
            fprintf(f, "          ],\n");

            // properties
            fprintf(f, "          \"properties\": [\n");
            for (size_t i = 0; i < c.properties.size(); ++i) {
                const auto& x = c.properties[i];
                fprintf(f, "            {\"name\": \"%s\", \"type\": \"%s\", \"get\": %s, \"set\": %s, "
                           "\"get_token\": \"%s\", \"set_token\": \"%s\"}%s\n",
                        JsonEscape(x.name).c_str(), JsonEscape(x.type).c_str(),
                        x.has_get ? "true" : "false", x.has_set ? "true" : "false",
                        Hex(x.get_token).c_str(), Hex(x.set_token).c_str(),
                        (i + 1 < c.properties.size()) ? "," : "");
            }
            fprintf(f, "          ],\n");

            // methods
            fprintf(f, "          \"methods\": [\n");
            for (size_t i = 0; i < c.methods.size(); ++i) {
                const auto& m = c.methods[i];
                fprintf(f, "            {\"name\": \"%s\", \"token\": \"%s\", \"return\": \"%s\", \"params\": [",
                        JsonEscape(m.name).c_str(), Hex(m.token).c_str(),
                        JsonEscape(m.ret).c_str());
                for (size_t p = 0; p < m.params.size(); ++p)
                    fprintf(f, "%s{\"name\": \"%s\", \"type\": \"%s\"}", p ? ", " : "",
                            JsonEscape(m.params[p].name).c_str(),
                            JsonEscape(m.params[p].type).c_str());
                fprintf(f, "], \"param_count\": %zu, \"static\": %s, \"virtual\": %s, \"abstract\": %s, "
                           "\"public\": %s, \"special\": %s, \"signature\": \"%s\", \"full_name\": \"%s\", "
                           "\"jit_addr\": \"0x%llX\"}%s\n",
                        m.params.size(),
                        m.is_static ? "true" : "false", m.is_virtual ? "true" : "false",
                        m.is_abstract ? "true" : "false", m.is_public ? "true" : "false",
                        m.is_special ? "true" : "false",
                        JsonEscape(m.signature).c_str(), JsonEscape(m.full_name).c_str(),
                        (unsigned long long)m.jit_addr,
                        (i + 1 < c.methods.size()) ? "," : "");
            }
            fprintf(f, "          ]\n");
            fprintf(f, "        }%s\n", (ci + 1 < a.classes.size()) ? "," : "");
        }
        fprintf(f, "      ]\n");
        fprintf(f, "    }%s\n", (ai + 1 < asms.size()) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
}

// Rename tmp -> final, replacing whatever was there. MoveFileEx is atomic on the
// same volume, which is what makes "the file exists" mean "the file is finished".
static void PublishFile(const std::string& tmp, const std::string& final_path) {
    if (MoveFileExA(tmp.c_str(), final_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return;

    // Very unlikely (different volume, AV holding the handle). Fall back to a copy
    // so the caller still gets its dump, then drop the temp file.
    if (CopyFileA(tmp.c_str(), final_path.c_str(), FALSE))
        DeleteFileA(tmp.c_str());
    else
        mdlog::Printf("[monodump] could not publish %s (error %lu)\n",
                      final_path.c_str(), GetLastError());
}

// ---------------------------------------------------------------------- entry

void RunDump(const DumpOptions& opts) {
    // Any thread calling into Mono must be attached, or the GC will not know
    // about it and you get random crashes during collection.
    mono_thread_attach(mono_get_root_domain());

    std::vector<AssemblyInfo> assemblies;
    Collector col{ &opts, &assemblies, 0 };

    mdlog::Crumb("enumerating assemblies");
    if (!mono::foreach_assembly(OnAssembly, &col)) {
        mdlog::Printf("[monodump] this runtime exports no assembly enumeration "
                      "function; nothing to dump\n");
        return;
    }
    mdlog::Crumb("walk finished, writing files");

    size_t nclass = 0, nfield = 0, nmethod = 0, nprop = 0;
    for (const auto& a : assemblies) {
        nclass += a.classes.size();
        for (const auto& c : a.classes) {
            nfield  += c.fields.size();
            nmethod += c.methods.size();
            nprop   += c.properties.size();
        }
    }

    FILE* f = nullptr;

    // Write to "<name>.tmp" and rename when finished. A reader polling for
    // dump.json used to see the file the moment it was CREATED, and then either
    // parsed half of it or hit "used by another process". After a rename the file
    // exists only when it is complete and no longer held open here.
    if (opts.text) {
        const std::string txt = opts.out_dir + "\\dump.txt";
        const std::string tmp = txt + ".tmp";
        if (fopen_s(&f, tmp.c_str(), "w") == 0 && f) {
            fprintf(f, "# monodump: %zu assemblies, %zu classes, %zu fields, %zu props, %zu methods\n",
                    assemblies.size(), nclass, nfield, nprop, nmethod);
            fprintf(f, "# %d classes skipped after faulting during init\n\n", col.errors);
            WriteText(f, assemblies);
            fclose(f);
            f = nullptr;
            PublishFile(tmp, txt);
        }
    }

    if (opts.json) {
        const std::string js  = opts.out_dir + "\\dump.json";
        const std::string tmp = js + ".tmp";
        if (fopen_s(&f, tmp.c_str(), "w") == 0 && f) {
            WriteJson(f, assemblies, opts, col.errors);
            fclose(f);
            f = nullptr;
            PublishFile(tmp, js);
        }
    }

    printf("[monodump] %zu assemblies, %zu classes, %zu fields, %zu props, %zu methods\n",
           assemblies.size(), nclass, nfield, nprop, nmethod);
    printf("[monodump] %d classes faulted and were skipped\n", col.errors);
    printf("[monodump] written to %s\n", opts.out_dir.c_str());
}
