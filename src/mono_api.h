#pragma once
//
// mono_api.h - Mono runtime bindings resolved at runtime from the game's mono DLL.
//
// Deliberately does NOT include the Mono SDK headers. Every Mono type is opaque
// here because a dumper never dereferences them - it only passes them back into
// Mono. That removes the SDK dependency the forum post has, and removes a whole
// class of "my headers are a different Mono version than the game" bugs.
//

#include <windows.h>
#include <cstdint>

// ---------------------------------------------------------------- opaque types
struct MonoDomain;
struct MonoAssembly;
struct MonoImage;
struct MonoClass;
struct MonoMethod;
struct MonoMethodSignature;
struct MonoClassField;
struct MonoProperty;
struct MonoType;
struct MonoVTable;
struct MonoObject;
struct MonoString;
struct MonoThread;

// ------------------------------------------------------------- metadata tokens
constexpr uint32_t MONO_TOKEN_TYPE_DEF = 0x02000000;
constexpr int      MONO_TABLE_TYPEDEF  = 2;

// ECMA-335 field attributes
constexpr uint32_t FIELD_ATTR_FIELD_ACCESS_MASK = 0x0007;
constexpr uint32_t FIELD_ATTR_PRIVATE  = 0x0001;
constexpr uint32_t FIELD_ATTR_PUBLIC   = 0x0006;
constexpr uint32_t FIELD_ATTR_STATIC   = 0x0010;
constexpr uint32_t FIELD_ATTR_INIT_ONLY= 0x0020;  // readonly
constexpr uint32_t FIELD_ATTR_LITERAL  = 0x0040;  // const, has no storage

// ECMA-335 method attributes
constexpr uint32_t METHOD_ATTR_ACCESS_MASK = 0x0007;
constexpr uint32_t METHOD_ATTR_PRIVATE  = 0x0001;
constexpr uint32_t METHOD_ATTR_PUBLIC   = 0x0006;
constexpr uint32_t METHOD_ATTR_STATIC   = 0x0010;
constexpr uint32_t METHOD_ATTR_FINAL    = 0x0020;
constexpr uint32_t METHOD_ATTR_VIRTUAL  = 0x0040;
constexpr uint32_t METHOD_ATTR_ABSTRACT = 0x0400;
constexpr uint32_t METHOD_ATTR_SPECIAL_NAME = 0x0800;  // get_/set_/op_/.ctor

// ECMA-335 type attributes
constexpr uint32_t TYPE_ATTR_VISIBILITY_MASK = 0x0007;
constexpr uint32_t TYPE_ATTR_PUBLIC     = 0x0001;
constexpr uint32_t TYPE_ATTR_INTERFACE  = 0x0020;
constexpr uint32_t TYPE_ATTR_ABSTRACT   = 0x0080;
constexpr uint32_t TYPE_ATTR_SEALED     = 0x0100;

// glib callback shape used by mono_domain_assembly_foreach
typedef void (*MonoGFunc)(void* data, void* user_data);

// ------------------------------------------------------------------ the table
// X(return_type, name, (arg list))
#define MONO_FUNCTION_LIST(X)                                                        \
    /* domain + threading */                                                         \
    X(MonoDomain*,        mono_get_root_domain,             ())                      \
    X(MonoDomain*,        mono_domain_get,                  ())                      \
    X(MonoThread*,        mono_thread_attach,               (MonoDomain*))           \
    X(void,               mono_thread_detach,               (MonoThread*))           \
    /* assemblies + images */                                                        \
    X(void,               mono_domain_assembly_foreach,     (MonoDomain*, MonoGFunc, void*)) \
    X(MonoAssembly*,      mono_domain_assembly_open,        (MonoDomain*, const char*)) \
    X(MonoImage*,         mono_assembly_get_image,          (MonoAssembly*))         \
    X(const char*,        mono_image_get_name,              (MonoImage*))            \
    X(const char*,        mono_image_get_filename,          (MonoImage*))            \
    X(const char*,        mono_image_get_version,           (MonoImage*))            \
    X(int,                mono_image_get_table_rows,        (MonoImage*, int))       \
    /* classes */                                                                    \
    X(MonoClass*,         mono_class_get,                   (MonoImage*, uint32_t))  \
    X(MonoClass*,         mono_class_from_name,             (MonoImage*, const char*, const char*)) \
    X(void,               mono_class_init,                  (MonoClass*))            \
    X(const char*,        mono_class_get_name,              (MonoClass*))            \
    X(const char*,        mono_class_get_namespace,         (MonoClass*))            \
    X(MonoClass*,         mono_class_get_parent,            (MonoClass*))            \
    X(MonoImage*,         mono_class_get_image,             (MonoClass*))            \
    X(uint32_t,           mono_class_get_flags,             (MonoClass*))            \
    X(uint32_t,           mono_class_get_type_token,        (MonoClass*))            \
    X(int32_t,            mono_class_instance_size,         (MonoClass*))            \
    X(int,                mono_class_is_enum,               (MonoClass*))            \
    X(int,                mono_class_is_valuetype,          (MonoClass*))            \
    X(MonoClass*,         mono_class_get_nested_types,      (MonoClass*, void**))    \
    X(MonoClass*,         mono_class_get_interfaces,        (MonoClass*, void**))    \
    X(MonoClassField*,    mono_class_get_fields,            (MonoClass*, void**))    \
    X(MonoMethod*,        mono_class_get_methods,           (MonoClass*, void**))    \
    X(MonoProperty*,      mono_class_get_properties,        (MonoClass*, void**))    \
    X(MonoClassField*,    mono_class_get_field_from_name,   (MonoClass*, const char*)) \
    X(MonoMethod*,        mono_class_get_method_from_name,  (MonoClass*, const char*, int)) \
    X(MonoVTable*,        mono_class_vtable,                (MonoDomain*, MonoClass*)) \
    X(void*,              mono_vtable_get_static_field_data,(MonoVTable*))           \
    /* properties */                                                                 \
    X(const char*,        mono_property_get_name,           (MonoProperty*))         \
    X(MonoMethod*,        mono_property_get_get_method,     (MonoProperty*))         \
    X(MonoMethod*,        mono_property_get_set_method,     (MonoProperty*))         \
    /* fields */                                                                     \
    X(const char*,        mono_field_get_name,              (MonoClassField*))       \
    X(MonoType*,          mono_field_get_type,              (MonoClassField*))       \
    X(uint32_t,           mono_field_get_offset,            (MonoClassField*))       \
    X(uint32_t,           mono_field_get_flags,             (MonoClassField*))       \
    X(void,               mono_field_get_value,             (MonoObject*, MonoClassField*, void*)) \
    X(void,               mono_field_set_value,             (MonoObject*, MonoClassField*, void*)) \
    X(void,               mono_field_static_get_value,      (MonoVTable*, MonoClassField*, void*)) \
    X(void,               mono_field_static_set_value,      (MonoVTable*, MonoClassField*, void*)) \
    /* types */                                                                      \
    X(char*,              mono_type_get_name,               (MonoType*))             \
    X(int,                mono_type_get_type,               (MonoType*))             \
    X(MonoClass*,         mono_type_get_class,              (MonoType*))             \
    /* methods */                                                                    \
    X(const char*,        mono_method_get_name,             (MonoMethod*))           \
    X(MonoClass*,         mono_method_get_class,            (MonoMethod*))           \
    X(uint32_t,           mono_method_get_flags,            (MonoMethod*, uint32_t*)) \
    X(uint32_t,           mono_method_get_token,            (MonoMethod*))           \
    X(char*,              mono_method_full_name,            (MonoMethod*, int))      \
    X(void,               mono_method_get_param_names,      (MonoMethod*, const char**)) \
    X(MonoMethodSignature*, mono_method_signature,          (MonoMethod*))           \
    X(uint32_t,           mono_signature_get_param_count,   (MonoMethodSignature*))  \
    X(MonoType*,          mono_signature_get_return_type,   (MonoMethodSignature*))  \
    X(MonoType*,          mono_signature_get_params,        (MonoMethodSignature*, void**)) \
    X(char*,              mono_signature_get_desc,          (MonoMethodSignature*, int)) \
    X(void*,              mono_compile_method,              (MonoMethod*))           \
    /* enums: an enum parameter is really its underlying integer */              \
    X(MonoType*,          mono_class_enum_basetype,         (MonoClass*))            \
    X(MonoClass*,         mono_class_from_mono_type,        (MonoType*))             \
    /* invoking */                                                                   \
    X(MonoObject*,        mono_runtime_invoke,              (MonoMethod*, void*, void**, MonoObject**)) \
    X(MonoString*,        mono_string_new,                  (MonoDomain*, const char*)) \
    X(char*,              mono_string_to_utf8,              (MonoString*))           \
    X(void*,              mono_object_unbox,                (MonoObject*))           \
    X(MonoClass*,         mono_object_get_class,            (MonoObject*))           \
    X(void,               mono_free,                        (void*))

// declare one function pointer per entry
#define MONO_DECLARE(ret, name, args) using t_##name = ret(*) args; extern t_##name name;
MONO_FUNCTION_LIST(MONO_DECLARE)
#undef MONO_DECLARE

namespace mono {

// Finds the loaded mono module (name varies: mono.dll, mono-2.0-bdwgc.dll,
// monobdwgc-2.0.dll, mono-2.0-sgen.dll) and resolves every export above.
// Returns false if no mono module is present - that usually means the game is
// IL2CPP, not Mono.
bool initialize(char* out_module_name, size_t out_size);

// Number of exports that failed to resolve. Non-fatal: the dumper degrades.
int missing_count();

} // namespace mono
