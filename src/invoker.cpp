#include "invoker.h"
#include "dispatch.h"
#include "log.h"
#include "mono_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

// ------------------------------------------------------------------ parsing

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Splits "a, b, \"c, d\", e" respecting quotes, so string args with commas work.
static std::vector<std::string> SplitArgs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') { in_quotes = !in_quotes; cur += c; }
        else if (c == ',' && !in_quotes) { out.push_back(Trim(cur)); cur.clear(); }
        else cur += c;
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    return out;
}

// Splits "Namespace.Class::Member" into its two halves.
static bool SplitMember(const std::string& s, std::string& klass, std::string& member) {
    const size_t p = s.find("::");
    if (p == std::string::npos) return false;
    klass  = Trim(s.substr(0, p));
    member = Trim(s.substr(p + 2));
    return !klass.empty() && !member.empty();
}

// Splits "Namespace.Class" into namespace and name. Mono wants them separately.
static void SplitTypeName(const std::string& full, std::string& ns, std::string& name) {
    const size_t dot = full.find_last_of('.');
    if (dot == std::string::npos) { ns.clear(); name = full; }
    else { ns = full.substr(0, dot); name = full.substr(dot + 1); }
}

// ------------------------------------------------------------ class lookup

struct ClassSearch {
    std::string ns, name;
    MonoClass*  found = nullptr;
};

static void FindClassCb(void* data, void* user) {
    auto* s = static_cast<ClassSearch*>(user);
    if (s->found) return;

    MonoImage* img = mono_assembly_get_image(static_cast<MonoAssembly*>(data));
    if (!img) return;

    if (MonoClass* k = mono_class_from_name(img, s->ns.c_str(), s->name.c_str()))
        s->found = k;
}

// Searches every loaded assembly, so you do not have to know which one the
// class lives in.
static MonoClass* FindClass(const std::string& full_name) {
    ClassSearch s;
    SplitTypeName(full_name, s.ns, s.name);
    mono_domain_assembly_foreach(mono_get_root_domain(), FindClassCb, &s);
    return s.found;
}

// ----------------------------------------------------------- SEH shells
//
// MSVC refuses __try in any function that also holds objects needing unwinding
// (error C2712), and every interesting function here is full of std::string.
// So each guarded operation gets a POD-only shell.

struct InvokeReq {
    MonoMethod*  method;
    void*        instance;
    void**       args;
    MonoObject*  exception;
    MonoObject*  result;
    int          crashed;
};

static void SehInvoke(InvokeReq* r) {
    __try {
        r->result = mono_runtime_invoke(r->method, r->instance, r->args, &r->exception);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r->crashed = 1;
    }
}

struct FieldReq {
    MonoVTable*     vtable;    // static access
    MonoObject*     object;    // instance access
    MonoClassField* field;
    void*           buffer;
    int             crashed;
};

static void SehFieldGet(FieldReq* r) {
    __try {
        if (r->object) mono_field_get_value(r->object, r->field, r->buffer);
        else           mono_field_static_get_value(r->vtable, r->field, r->buffer);
    } __except (EXCEPTION_EXECUTE_HANDLER) { r->crashed = 1; }
}

static void SehFieldSet(FieldReq* r) {
    __try {
        if (r->object) mono_field_set_value(r->object, r->field, r->buffer);
        else           mono_field_static_set_value(r->vtable, r->field, r->buffer);
    } __except (EXCEPTION_EXECUTE_HANDLER) { r->crashed = 1; }
}

// --------------------------------------------------------------- type names

static std::string TypeName(MonoType* t) {
    if (!t || !mono_type_get_name) return {};
    char* n = mono_type_get_name(t);
    std::string s = n ? n : "";
    if (n && mono_free) mono_free(n);
    return s;
}

// An enum parameter is just its underlying integer as far as marshalling goes.
// v1.0 compared the printed type name against "System.Int32" and therefore
// rejected every enum argument in the game.
static std::string MarshalTypeName(MonoType* t) {
    const std::string raw = TypeName(t);
    if (!t || !mono_class_is_enum) return raw;

    MonoClass* k = nullptr;
    if (mono_class_from_mono_type) k = mono_class_from_mono_type(t);
    else if (mono_type_get_class)  k = mono_type_get_class(t);
    if (!k) return raw;

    if (!mono_class_is_enum(k) || !mono_class_enum_basetype) return raw;
    const std::string base = TypeName(mono_class_enum_basetype(k));
    return base.empty() ? raw : base;
}

static std::string FieldTypeName(MonoClassField* f) {
    if (!f || !mono_field_get_type) return {};
    return MarshalTypeName(mono_field_get_type(f));
}

static bool IsIntLike(const std::string& t) {
    return t == "System.Int32"  || t == "System.UInt32" ||
           t == "System.Int16"  || t == "System.UInt16" ||
           t == "System.SByte"  || t == "System.Byte";
}

// --------------------------------------------------------- argument marshal

// Storage that outlives the invoke call. Mono wants void** where each entry
// points at the value for value types, or IS the object pointer for refs.
struct ArgPack {
    std::vector<int64_t>  ints;      // int/uint/short/byte all fit here
    std::vector<float>    f32;
    std::vector<double>   f64;
    std::vector<uint8_t>  boolean;
    std::vector<void*>    slots;

    // Reserve up front so no reallocation invalidates the pointers we hand out.
    void reserve(size_t n) {
        ints.reserve(n); f32.reserve(n); f64.reserve(n);
        boolean.reserve(n); slots.reserve(n);
    }
};

static bool MarshalArg(const std::string& raw, const std::string& type, ArgPack& pack) {
    std::string v = raw;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);

    if (IsIntLike(type)) {
        // Widened once, then handed over as the exact width Mono expects: the
        // callee reads only the low bytes, and the rest of the slot stays zero.
        pack.ints.push_back(static_cast<int64_t>(strtoll(v.c_str(), nullptr, 0)));
        pack.slots.push_back(&pack.ints.back());
    } else if (type == "System.Int64" || type == "System.UInt64" ||
               type == "System.IntPtr" || type == "System.UIntPtr") {
        pack.ints.push_back(strtoll(v.c_str(), nullptr, 0));
        pack.slots.push_back(&pack.ints.back());
    } else if (type == "System.Single") {
        pack.f32.push_back(strtof(v.c_str(), nullptr));
        pack.slots.push_back(&pack.f32.back());
    } else if (type == "System.Double") {
        pack.f64.push_back(strtod(v.c_str(), nullptr));
        pack.slots.push_back(&pack.f64.back());
    } else if (type == "System.Boolean") {
        pack.boolean.push_back((v == "true" || v == "1") ? 1 : 0);
        pack.slots.push_back(&pack.boolean.back());
    } else if (type == "System.Char") {
        pack.ints.push_back(v.empty() ? 0 : (int64_t)(unsigned char)v[0]);
        pack.slots.push_back(&pack.ints.back());
    } else if (type == "System.String") {
        // Strings are reference types: pass the MonoString* itself, not its address.
        MonoString* s = mono_string_new(mono_get_root_domain(), v.c_str());
        pack.slots.push_back(s);
    } else if (v == "null" || v == "0") {
        // Any reference type can at least be passed as null.
        pack.slots.push_back(nullptr);
    } else {
        mdlog::Fail("cannot marshal parameter type '%s' - supported: int, long, float, double, bool, char, string, enum, null",
                    type.c_str());
        return false;
    }
    return true;
}

// ------------------------------------------------------------- field access

struct FieldTarget {
    MonoClass*      klass  = nullptr;
    MonoClassField* field  = nullptr;
    MonoVTable*     vtable = nullptr;   // static fields
    MonoObject*     object = nullptr;   // instance fields
};

static MonoObject* ResolveInstance(const std::string& spec);

// Resolves Class::field, plus the optional "on Class::staticInstanceField" that
// makes instance fields reachable (health, money, inventory - the useful ones).
static bool ResolveField(const std::string& target, const std::string& on_spec,
                         FieldTarget& out) {
    std::string cls, field;
    if (!SplitMember(target, cls, field)) {
        mdlog::Fail("expected Class::field, got '%s'", target.c_str());
        return false;
    }

    MonoClass* k = FindClass(cls);
    if (!k) { mdlog::Fail("class '%s' not found", cls.c_str()); return false; }
    if (mono_class_init) mono_class_init(k);

    MonoClassField* f = mono_class_get_field_from_name(k, field.c_str());
    if (!f) { mdlog::Fail("field '%s' not found on %s", field.c_str(), cls.c_str()); return false; }

    const uint32_t flags = mono_field_get_flags ? mono_field_get_flags(f) : 0;
    const bool is_static = (flags & FIELD_ATTR_STATIC) != 0;
    const bool is_const  = (flags & FIELD_ATTR_LITERAL) != 0;

    if (is_const) {
        mdlog::Fail("%s::%s is const - it has no storage, the value is baked into the callers",
                    cls.c_str(), field.c_str());
        return false;
    }

    out.klass = k;
    out.field = f;

    if (is_static) {
        if (!on_spec.empty())
            mdlog::Printf("    (note: %s::%s is static, 'on ...' ignored)\n", cls.c_str(), field.c_str());
        out.vtable = mono_class_vtable(mono_get_root_domain(), k);
        if (!out.vtable) { mdlog::Fail("no vtable for %s", cls.c_str()); return false; }
        return true;
    }

    if (on_spec.empty()) {
        mdlog::Fail("%s::%s is an instance field - add: on Class::staticInstanceField",
                    cls.c_str(), field.c_str());
        return false;
    }
    out.object = ResolveInstance(on_spec);
    return out.object != nullptr;
}

static void PrintValue(const std::string& label, const std::string& type, const uint8_t* buf) {
    if (IsIntLike(type) || type == "System.Int32")
        mdlog::Ok("%s (%s) = %d", label.c_str(), type.c_str(), *(const int32_t*)buf);
    else if (type == "System.Int64" || type == "System.UInt64")
        mdlog::Ok("%s (%s) = %lld", label.c_str(), type.c_str(), *(const int64_t*)buf);
    else if (type == "System.Single")
        mdlog::Ok("%s (%s) = %g", label.c_str(), type.c_str(), *(const float*)buf);
    else if (type == "System.Double")
        mdlog::Ok("%s (%s) = %g", label.c_str(), type.c_str(), *(const double*)buf);
    else if (type == "System.Boolean")
        mdlog::Ok("%s (%s) = %s", label.c_str(), type.c_str(), *buf ? "true" : "false");
    else if (type == "System.String") {
        MonoString* s = *(MonoString* const*)buf;
        if (!s) { mdlog::Ok("%s (System.String) = null", label.c_str()); return; }
        char* utf8 = mono_string_to_utf8 ? mono_string_to_utf8(s) : nullptr;
        mdlog::Ok("%s (System.String) = \"%s\"", label.c_str(), utf8 ? utf8 : "?");
        if (utf8 && mono_free) mono_free(utf8);
    } else
        mdlog::Ok("%s (%s) = 0x%llX (reference or unsupported type)",
                  label.c_str(), type.c_str(), *(const unsigned long long*)buf);
}

static void CmdGet(const std::string& target, const std::string& on_spec) {
    FieldTarget ft;
    if (!ResolveField(target, on_spec, ft)) return;

    const std::string type = FieldTypeName(ft.field);
    uint8_t buf[16] = {};

    FieldReq r{};
    r.vtable = ft.vtable;
    r.object = ft.object;
    r.field  = ft.field;
    r.buffer = buf;
    SehFieldGet(&r);
    if (r.crashed) { mdlog::Fail("native crash reading %s", target.c_str()); return; }

    PrintValue(target, type, buf);
}

static void CmdSet(const std::string& target, const std::string& value, const std::string& on_spec) {
    FieldTarget ft;
    if (!ResolveField(target, on_spec, ft)) return;

    const std::string type = FieldTypeName(ft.field);
    uint8_t buf[16] = {};

    if (IsIntLike(type))                      *(int32_t*)buf = (int32_t)strtoll(value.c_str(), nullptr, 0);
    else if (type == "System.Int64" ||
             type == "System.UInt64")         *(int64_t*)buf = strtoll(value.c_str(), nullptr, 0);
    else if (type == "System.Single")         *(float*)buf   = strtof(value.c_str(), nullptr);
    else if (type == "System.Double")         *(double*)buf  = strtod(value.c_str(), nullptr);
    else if (type == "System.Boolean")        *buf = (value == "true" || value == "1") ? 1 : 0;
    else if (type == "System.String") {
        std::string v = value;
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        MonoString* s = mono_string_new(mono_get_root_domain(), v.c_str());
        *(MonoString**)buf = s;
    } else {
        mdlog::Fail("cannot write type '%s'", type.c_str());
        return;
    }

    FieldReq r{};
    r.vtable = ft.vtable;
    r.object = ft.object;
    r.field  = ft.field;
    r.buffer = buf;
    SehFieldSet(&r);
    if (r.crashed) { mdlog::Fail("native crash writing %s", target.c_str()); return; }

    mdlog::Ok("%s = %s", target.c_str(), value.c_str());
}

// -------------------------------------------------------------------- invoke

// Reads a static field that holds an object reference - the standard Unity
// singleton pattern (public static GameManager Instance). That gives you the
// `this` pointer needed to call instance methods.
static MonoObject* ResolveInstance(const std::string& spec) {
    std::string cls, field;
    if (!SplitMember(spec, cls, field)) {
        mdlog::Fail("'on' expects Class::staticField holding the instance, got '%s'", spec.c_str());
        return nullptr;
    }

    MonoClass* k = FindClass(cls);
    if (!k) { mdlog::Fail("class '%s' not found (in 'on %s')", cls.c_str(), spec.c_str()); return nullptr; }

    MonoClassField* f = mono_class_get_field_from_name(k, field.c_str());
    if (!f) { mdlog::Fail("field '%s' not found on %s", field.c_str(), cls.c_str()); return nullptr; }

    MonoVTable* vt = mono_class_vtable(mono_get_root_domain(), k);
    if (!vt) { mdlog::Fail("no vtable for %s", cls.c_str()); return nullptr; }

    MonoObject* obj = nullptr;
    FieldReq r{};
    r.vtable = vt;
    r.field  = f;
    r.buffer = &obj;
    SehFieldGet(&r);
    if (r.crashed) { mdlog::Fail("native crash reading %s", spec.c_str()); return nullptr; }

    if (!obj)
        mdlog::Fail("%s is null - the singleton is not initialised yet (load a save / enter the world first)",
                    spec.c_str());
    return obj;
}

static void CmdCall(const std::string& expr, const std::string& on_spec) {
    const size_t open = expr.find('(');
    const size_t close = expr.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        mdlog::Fail("expected Class::Method(args), got '%s'", expr.c_str());
        return;
    }

    std::string cls, method;
    if (!SplitMember(Trim(expr.substr(0, open)), cls, method)) {
        mdlog::Fail("expected Class::Method(args), got '%s'", expr.c_str());
        return;
    }

    const std::vector<std::string> args = SplitArgs(expr.substr(open + 1, close - open - 1));

    MonoClass* k = FindClass(cls);
    if (!k) { mdlog::Fail("class '%s' not found", cls.c_str()); return; }
    if (mono_class_init) mono_class_init(k);

    MonoMethod* m = mono_class_get_method_from_name(k, method.c_str(), (int)args.size());
    if (!m) {
        mdlog::Fail("%s::%s with %d args not found - overloads differ by arity, check param_count in the dump",
                    cls.c_str(), method.c_str(), (int)args.size());
        return;
    }

    // Pull the real parameter types from the signature so we marshal correctly
    // instead of guessing from how the argument looks.
    std::vector<std::string> ptypes;
    MonoMethodSignature* sig = mono_method_signature(m);
    if (sig && mono_signature_get_params) {
        void* it = nullptr;
        while (MonoType* pt = mono_signature_get_params(sig, &it))
            ptypes.push_back(MarshalTypeName(pt));
    }
    if (ptypes.size() != args.size()) {
        mdlog::Fail("signature wants %d args, got %d", (int)ptypes.size(), (int)args.size());
        return;
    }

    ArgPack pack;
    pack.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i)
        if (!MarshalArg(args[i], ptypes[i], pack)) return;

    void* instance = nullptr;
    uint32_t iflags = 0;
    const uint32_t flags = mono_method_get_flags ? mono_method_get_flags(m, &iflags) : 0;
    const bool is_static = (flags & METHOD_ATTR_STATIC) != 0;

    if (!is_static) {
        if (on_spec.empty()) {
            mdlog::Fail("%s::%s is an instance method - add: on Class::staticInstanceField",
                        cls.c_str(), method.c_str());
            return;
        }
        instance = ResolveInstance(on_spec);
        if (!instance) return;
    }

    // Every thread touching Mono must be attached, every time.
    mono_thread_attach(mono_get_root_domain());

    InvokeReq r{};
    r.method   = m;
    r.instance = instance;
    r.args     = pack.slots.empty() ? nullptr : pack.slots.data();
    SehInvoke(&r);

    if (r.crashed) { mdlog::Fail("native crash inside %s::%s", cls.c_str(), method.c_str()); return; }

    if (r.exception) {
        // A managed exception comes back through the out parameter, it is NOT
        // thrown at us. Ignoring it is why people think the call "did nothing".
        const char* ename = "?";
        if (mono_object_get_class && mono_class_get_name) {
            if (MonoClass* ec = mono_object_get_class(r.exception)) ename = mono_class_get_name(ec);
        }
        mdlog::Fail("%s::%s threw a managed exception (%s)", cls.c_str(), method.c_str(), ename);
        return;
    }

    if (r.result && mono_object_unbox) {
        MonoType* rt = sig && mono_signature_get_return_type ? mono_signature_get_return_type(sig) : nullptr;
        const std::string rtype = MarshalTypeName(rt);
        if (rtype == "System.String") {
            char* utf8 = mono_string_to_utf8 ? mono_string_to_utf8((MonoString*)r.result) : nullptr;
            mdlog::Ok("%s::%s invoked -> \"%s\"", cls.c_str(), method.c_str(), utf8 ? utf8 : "?");
            if (utf8 && mono_free) mono_free(utf8);
            return;
        }
        if (void* boxed = mono_object_unbox(r.result)) {
            if (rtype == "System.Single")
                mdlog::Ok("%s::%s invoked -> %g", cls.c_str(), method.c_str(), *(float*)boxed);
            else if (rtype == "System.Double")
                mdlog::Ok("%s::%s invoked -> %g", cls.c_str(), method.c_str(), *(double*)boxed);
            else if (rtype == "System.Boolean")
                mdlog::Ok("%s::%s invoked -> %s", cls.c_str(), method.c_str(), *(uint8_t*)boxed ? "true" : "false");
            else if (rtype == "System.Int64" || rtype == "System.UInt64")
                mdlog::Ok("%s::%s invoked -> %lld", cls.c_str(), method.c_str(), *(int64_t*)boxed);
            else
                mdlog::Ok("%s::%s invoked -> %d", cls.c_str(), method.c_str(), *(int32_t*)boxed);
            return;
        }
    }

    mdlog::Ok("%s::%s invoked", cls.c_str(), method.c_str());
}

// ------------------------------------------------------------------ dispatch

static void ExecuteNow(const std::string& line) {
    // optional trailing " on Class::field"
    std::string body = line, on_spec;
    const size_t on = line.rfind(" on ");
    if (on != std::string::npos) {
        body    = Trim(line.substr(0, on));
        on_spec = Trim(line.substr(on + 4));
    }

    const size_t sp = body.find(' ');
    if (sp == std::string::npos) { mdlog::Fail("unknown command '%s'", body.c_str()); return; }

    const std::string verb = body.substr(0, sp);
    const std::string rest = Trim(body.substr(sp + 1));

    if (verb == "call") {
        CmdCall(rest, on_spec);
    } else if (verb == "get") {
        CmdGet(rest, on_spec);
    } else if (verb == "set") {
        const size_t s = rest.find(' ');
        if (s == std::string::npos) { mdlog::Fail("expected: set Class::field value"); return; }
        CmdSet(Trim(rest.substr(0, s)), Trim(rest.substr(s + 1)), on_spec);
    } else {
        mdlog::Fail("unknown verb '%s' (call / get / set)", verb.c_str());
    }
}

static bool g_main_thread = false;
static bool g_warned_thread = false;

static void MainThreadJob(void* arg) {
    ExecuteNow(*static_cast<const std::string*>(arg));
}

void RunCommand(const std::string& raw) {
    const std::string line = Trim(raw);
    if (line.empty() || line[0] == '#' || line.compare(0, 2, "//") == 0) return;

    mdlog::Printf("  > %s\n", line.c_str());

    // Preferred path: let the game's own main thread do the work. Unity API
    // called from any other thread either does nothing or crashes the process.
    if (g_main_thread && dispatch::Ready()) {
        if (dispatch::Run(&MainThreadJob, (void*)&line, 8000)) return;
        mdlog::Printf("    (main thread did not pick the job up in 8 s - is the game paused? running here instead)\n");
    } else if (g_main_thread && !g_warned_thread) {
        g_warned_thread = true;
        mdlog::Printf("    (main-thread dispatch not ready: %s)\n",
                      dispatch::Available() ? "no managed activity seen yet" : "built without MinHook");
    }

    mono_thread_attach(mono_get_root_domain());
    ExecuteNow(line);
}

// SEH shell again: RunCommand is full of unwindable objects, so the __try has
// to sit in a function that owns none of them.
static void RunCommandRaw(const char* line) {
    RunCommand(std::string(line ? line : ""));
}

static void SehRunCommand(const char* line, int* crashed) {
    __try { RunCommandRaw(line); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crashed = 1; }
}

// ------------------------------------------------------------- watcher thread

static HANDLE      g_stop   = nullptr;
static HANDLE      g_thread = nullptr;
static std::string g_cmd_path;

static bool ReadWholeFile(const std::string& path, std::string& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    char buf[4096];
    size_t n = 0;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static void TruncateFile(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") == 0 && f) fclose(f);
}

static void RunBatch(const std::string& text) {
    std::string seq;
    std::vector<std::string> cmds;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string line = Trim(text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos));
        if (nl == std::string::npos) { if (!line.empty() && line[0] != '#') cmds.push_back(line); break; }
        pos = nl + 1;

        if (line.empty()) continue;
        if (line.compare(0, 4, "#seq") == 0) { seq = Trim(line.substr(4)); continue; }
        if (line[0] == '#' || line.compare(0, 2, "//") == 0) continue;
        cmds.push_back(line);
    }

    if (cmds.empty()) return;

    mdlog::BeginBatch(seq);
    for (const std::string& c : cmds) {
        int crashed = 0;
        SehRunCommand(c.c_str(), &crashed);
        if (crashed) mdlog::Fail("command crashed: %s", c.c_str());
    }
    mdlog::EndBatch();
}

static DWORD WINAPI WatchThread(LPVOID) {
    // This thread must never be mistaken for the game's main thread, and it must
    // be attached before it touches anything Mono.
    dispatch::IgnoreCurrentThread();
    mono_thread_attach(mono_get_root_domain());

    mdlog::Printf("[monodump] invoker watching %s\n", g_cmd_path.c_str());
    mdlog::Printf("[monodump] write lines into that file and save; they run and the file clears.\n");

    for (;;) {
        if (WaitForSingleObject(g_stop, 200) == WAIT_OBJECT_0) break;

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExA(g_cmd_path.c_str(), GetFileExInfoStandard, &fad)) continue;
        if (fad.nFileSizeLow == 0 && fad.nFileSizeHigh == 0) continue;

        std::string text;
        if (!ReadWholeFile(g_cmd_path, text) || text.empty()) continue;

        // A batch is only complete once it ends with a newline. Without this the
        // watcher could read a half-flushed line and execute nonsense.
        if (text.back() != '\n' && text.back() != '\r') continue;

        RunBatch(text);
        TruncateFile(g_cmd_path);
    }

    mdlog::Printf("[monodump] invoker stopped\n");
    return 0;
}

void StartInvoker(const std::string& dir, bool main_thread) {
    g_main_thread = main_thread;
    g_cmd_path = dir + "\\monodump_cmd.txt";

    // Create the file so the user has something to open.
    FILE* f = nullptr;
    if (fopen_s(&f, g_cmd_path.c_str(), "w") == 0 && f) {
        fprintf(f, "# monodump command file. Write one command per line and save.\n");
        fprintf(f, "# call RecipeManager::UnlockAll()\n");
        fprintf(f, "# call Inventory::AddResource(\"wood\", 9999) on GameManager::Instance\n");
        fprintf(f, "# get PlayerStats::money\n");
        fprintf(f, "# set PlayerStats::money 999999\n");
        fprintf(f, "# set Player::health 999 on Player::local     (instance field)\n");
        fclose(f);
    }

    if (!g_stop) g_stop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
}

void StopInvoker() {
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        // Do not unload the module out from under a running thread.
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
}
