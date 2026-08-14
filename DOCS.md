# monodump

Dumps every Mono class, field, property and method from a running game into
`dump.json`, then lets you call those methods without rebuilding anything.

The intended workflow:

```
inject  ->  dump.json  ->  monofind --hunt  ->  read it in dnSpy  ->  call it
```

---

## The one architectural thing to understand

**An external exe cannot call `mono_*` functions.** They are ordinary C
functions operating on the runtime's private heap inside the game's address
space. `GetProcAddress` on another process's module returns an address that is
meaningless in your own.

So the tool is two pieces:

| Piece | Runs where | Job |
|---|---|---|
| `monodump.exe` | your desktop | find pid, check bitness, inject |
| `monodump_payload.dll` | inside the game | walk metadata, write JSON, invoke methods |

Options travel through `monodump.ini` beside the DLL, because
`CreateRemoteThread(LoadLibraryA)` has no way to pass argv.

---

## Quick start

```bat
monodump.exe --list
monodump.exe --process Game.exe --assembly Assembly-CSharp
python tools\monofind.py dump.json --hunt
```

`--hunt` ranks every method by how likely it is to be the thing you want:

```
RecipeManager   0x02000011
  score 285  matched: unlockall, unlock, recipe
    Void UnlockAll() [static]
      dnSpy token: 0x06000101
      call RecipeManager::UnlockAll()

GalleryController   0x02000012
  score 285  matched: unlockeverything, unlock, gallery
    Void UnlockEverything() [static]
      dnSpy token: 0x06000202
      call GalleryController::UnlockEverything()

Game.Economy.Inventory   0x02000013
  score 145  matched: addresource, resource, add
    Void AddResource(String kind, Int32 amount) [static]
      dnSpy token: 0x06000301
      call Game.Economy.Inventory::AddResource("kind", 9999)
```

Each hit gives you both halves: the **dnSpy token** to go read the source, and
the **exact command line** to fire it.

---

## The dnSpy bridge: metadata tokens

Every method and class in the JSON carries its ECMA-335 metadata token.

In dnSpy, open the game's `Assembly-CSharp.dll`, then **Edit -> Go to MD Token**
(`Ctrl+D`) and paste `0x06000101`. You land directly on that method's decompiled
C#. No searching by name, no ambiguity between overloads or between classes that
share a name.

- `0x02......` = a type (class)
- `0x06......` = a method

This is the whole reason the token is in the JSON. Name-based searching falls
apart on obfuscated builds and on games with forty classes called `Manager`;
token lookup never does.

---

## Calling and modifying things

The payload stays resident after dumping and watches `monodump_cmd.txt` next to
the DLL. Write a line, save the file, it runs immediately:

```
call RecipeManager::UnlockAll()
call GalleryController::UnlockGallery(3) on GalleryController::Instance
call Game.Economy.Inventory::AddResource("wood", 9999)
get  Game.Economy.Inventory::totalEarned
set  Game.Economy.Inventory::totalEarned 999999
```

Three verbs:

| Verb | Does |
|---|---|
| `call Class::Method(args)` | `mono_runtime_invoke` on a static method |
| `call ... on Class::field` | same, but for an instance method; `field` is a static field holding the object |
| `get Class::field` | read a static field's current value |
| `set Class::field value` | write a static field |

The `on` form exists because most Unity game state hangs off a singleton
(`public static GameManager Instance`). `--hunt` already suggests
`on Class::Instance` for instance methods; confirm the real field name from the
class's field list first.

Argument types are read from the actual signature, not guessed from how you
typed them. `int`, `long`, `float`, `double`, `bool` and `string` marshal;
anything else needs a hook instead of a call, and monofind tells you so.

---

## What is in the JSON

```jsonc
{
  "schema": 2,
  "assemblies": [{
    "name": "Assembly-CSharp",
    "classes": [{
      "full_name": "Game.Economy.Inventory",
      "token": "0x02000013",
      "parent": "UnityEngine.MonoBehaviour",
      "interfaces": [], "nested": [],
      "instance_size": 96,
      "is_enum": false, "is_valuetype": false, "is_interface": false,
      "fields": [
        {"name":"money","type":"System.Int32","offset":36,"offset_hex":"0x24",
         "static":false,"const":false,"readonly":false,"public":true,
         "static_addr":"0x0","value":""}
      ],
      "properties": [
        {"name":"Money","type":"System.Int32","get":true,"set":true,
         "get_token":"0x06000302","set_token":"0x06000304"}
      ],
      "methods": [
        {"name":"AddResource","token":"0x06000301","return":"System.Void",
         "params":[{"name":"kind","type":"System.String"},
                   {"name":"amount","type":"System.Int32"}],
         "param_count":2,"static":true,"virtual":false,"abstract":false,
         "public":true,"special":false,"jit_addr":"0x0"}
      ]
    }]
  }]
}
```

Things worth knowing about specific fields:

- **`offset`** is `mono_field_get_offset`, the offset from the object base.
  Unlike an absolute address this is stable for the whole run and across runs of
  the same build. This is the number worth recording for signatures.
- **`static_addr`** is only valid for the current run - the static data block is
  allocated per domain load.
- **`value`** is the live value at dump time, for static primitives only.
- **`const: true`** fields have no storage at all. Their offset is meaningless.
- **`special: true`** marks compiler-generated methods: `get_X`, `set_X`,
  `op_Equality`, `.ctor`. Properties show up here as method pairs.
- **`properties`** matters because a lot of Unity state is
  `public int Money { get; set; }` - there is no `money` field to poke, only
  `get_Money`/`set_Money` to call.

---

## monofind

```bat
python tools\monofind.py dump.json --hunt              rank likely cheat functions
python tools\monofind.py dump.json --grep "unlock|gallery"
python tools\monofind.py dump.json --grep money --callable
python tools\monofind.py dump.json --class Inventory   one class in full
python tools\monofind.py dump.json --stats             per-assembly counts
```

`--hunt` scores on keyword groups (`unlockall` > `unlock` > `add`) and then
adjusts for whether you can actually invoke the thing: static `+30`, zero params
`+25`, all-primitive params `+10`, object params `-15`, abstract disqualified.
`--min-score` and `--limit` tune it.

`--grep` also prints ready-made `set Class::field value` lines for static fields.

---

## Files

```
src/mono_api.h      opaque types + X-macro table of ~60 exports
src/mono_api.cpp    runtime resolution, multi-name module detection
src/dumper.h/.cpp   metadata walk, static value reading, json/txt writers
src/invoker.h/.cpp  command-file watcher: call / get / set
src/dllmain.cpp     payload entry, console, ini parsing
injector/main.cpp   the exe: pid lookup, bitness check, injection
tools/monofind.py   query and rank the JSON
```

## Build

MSVC required - the payload uses SEH (`__try`/`__except`) to survive faulting
classes.

```bat
build.bat
```

Outputs in `build64\bin\Release` and `build32\bin\Release`. **Injector and
payload must match the game's bitness.** That mismatch is the most common
failure; the injector detects and reports it.

The CRT links statically (`/MT`) on purpose. A `/MD` payload fails to load in
games lacking the matching redistributable, and all you get is an opaque
`LoadLibraryA returned NULL`.

## All flags

```
--process <name>     target process
--pid <n>            target by pid instead
--assembly <substr>  only matching assemblies (use Assembly-CSharp)
--filter <substr>    only classes matching, or classes containing a match
--out <dir>          output directory
--compile            JIT every method to record its native address (slow)
--no-values          do not read live static field values
--no-props           skip properties and nested types (smaller json)
--no-text            json only
--oneshot            unload after dumping instead of staying resident
--list               list processes with a mono module loaded
```

Note that `--filter` keeps the **whole class** when anything in it matches. A
bare field name is useless; you need the surrounding class to find it in dnSpy.

---

## Bugs in the UnknownCheats writeup this is based on

**1. x64 truncation.** Silently corrupts every static field address on 64-bit
games:

```cpp
DWORD addr = (DWORD)GetStaticFieldData(pKlass);   // truncates to 32 bits
```

`DWORD` is always 32-bit, even in a 64-bit build. Use `uintptr_t`.

**2. Wrong return type.** `mono_field_get_value` returns `void`, not `void*`.

**3. The `exc` out-parameter is ignored.** `mono_runtime_invoke` does not throw
at you - a managed exception comes back through the fourth argument. Ignore it
and the call looks like it silently did nothing. The invoker here reports it.

**4. `mono_domain_assembly_open` used for lookups.** It takes a path or assembly
name and will try to load from disk if not already loaded. Use
`mono_domain_assembly_foreach` over what is already loaded.

**5. No enumeration at all.** The post only does lookup by known name. To dump,
you walk the metadata tables:

```cpp
int rows = mono_image_get_table_rows(image, MONO_TABLE_TYPEDEF);
for (int i = 2; i <= rows; ++i)                 // row 1 is <Module>
    MonoClass* k = mono_class_get(image, MONO_TOKEN_TYPE_DEF | i);
```

**6. Mono SDK headers.** Not needed. A dumper never dereferences a `MonoClass*`,
it only hands it back to Mono. Opaque forward declarations remove the dependency
and the entire version-mismatch bug class.

**7. Hardcoded `mono.dll`.** Unity ships `mono-2.0-bdwgc.dll`,
`monobdwgc-2.0.dll`, `mono-2.0-sgen.dll` depending on version. All are probed,
with a fallback scan for any module exporting `mono_get_root_domain`.

**8. Hardcoded `Assembly-CSharp` in every helper.** The invoker searches all
loaded assemblies, so you never have to know where a class lives.

---

## If it reports no Mono runtime

Check for `<Game>_Data\il2cpp_data`. If that folder exists the game is
**IL2CPP**: the C# was transpiled to C++ and compiled natively into
`GameAssembly.dll`. There is no Mono runtime to query. Use Il2CppDumper against
`GameAssembly.dll` + `global-metadata.dat` instead.

## Notes

- `mono_thread_attach` on every thread that touches Mono, every time, or the GC
  will eventually collect out from under you.
- Never do work in `DllMain` - the loader lock is held.
- Obfuscated or broken types can fault inside `mono_class_init`. Each class is
  wrapped in SEH, skipped on fault, and the count lands in
  `skipped_classes` in the JSON.
- Calling a method mid-frame from another thread can race with the game's own
  logic. If a call crashes reproducibly, hook the method instead of invoking it.
- Single-player only. Anything with anti-cheat, or anything multiplayer, is a
  completely different situation.

## What changed in v1.1

v1.0 was written for a human with a console open. v1.1 is written so a **tool**
can drive the payload, without losing any of the manual workflow.

### Build

* Every `__try` now lives in its own POD-only helper. MSVC rejects SEH in a
  function that also owns objects needing unwinding (`error C2712`), which is
  exactly what `invoker.cpp` and `dllmain.cpp` used to do.
* MinHook is an optional dependency in `third_party/minhook` (see its README).
  Without it the project still builds, but main-thread dispatch is compiled out.

### Commands run on the game's main thread

Unity is not thread safe: most of `UnityEngine` must run on the thread that runs
`Update()`. v1.0 invoked from its watcher thread, which is why some calls did
nothing and others crashed the game.

v1.1 hooks the runtime's own `mono_runtime_invoke` (MinHook). Unity drives every
managed `Update`/coroutine through it, on the main thread, many times per frame,
so the hook doubles as a reliable "run my queue on the main thread" callback.
Disable with `mainthread=0` / `--no-mainthread` when debugging.

### New `monodump.ini` keys

| key | default | meaning |
| --- | --- | --- |
| `dump` | `1` | `0` skips the metadata walk completely and only starts the invoker |
| `console` | `1` | `0` = no `AllocConsole` in the game; output goes to `monodump.log` |
| `log` | `1` | write `monodump.log` next to the payload |
| `mainthread` | `1` | execute commands on the game's main thread |

CLI equivalents: `--no-dump`, `--quiet`, `--no-mainthread`.

### File protocol

| file | direction | meaning |
| --- | --- | --- |
| `monodump.ini` | in | options, read once at load |
| `monodump_cmd.txt` | in | command batch; truncated after it runs |
| `monodump_ready.txt` | out | handshake: version, pid, bitness, runtime module, `mainthread` |
| `monodump_result.txt` | out | result of the last batch |
| `monodump.log` | out | everything, appended |

A batch is executed only once the command file **ends with a newline**, so a
reader can never catch a half-written file. Polling is size-based, not
timestamp-based: v1.0 compared `ftLastWriteTime` every 400 ms and silently lost a
second batch written inside the same timestamp tick.

An optional first line `#seq <token>` is echoed back, which is how a tool matches
an answer to the batch it sent:

```
seq=7
[+] RecipeManager::UnlockAll invoked
[-] class 'Nope' not found
end seq=7 ok=1 fail=1
```

### Invoker

* **Instance fields** work: `get Player::health on Player::local`,
  `set Player::health 999 on Player::local`. v1.0 could only touch statics.
* **Enum arguments** work: an enum parameter is marshalled as its underlying
  integer instead of being rejected as an unknown type.
* Also accepted now: `short`, `byte`, `char`, `IntPtr`, `null` for reference
  parameters, string field reads/writes, and typed return values.
* `const` fields are refused with an explanation instead of pretending to write.

### Lifetime

* A per-process mutex means a second injection cannot start a second console and
  a second watcher on the same command file - in v1.0 that ran every command
  twice.
* Shutdown is orderly: the watcher thread is stopped and the hook removed before
  the DLL unloads. v1.0 left the watcher running inside freed code.
* A tool can request a clean unload by signalling the event
  `Local\monodump_unload_<pid>`.
* With `console=1`, closing that console window with the X still kills the game -
  that is Windows, not monodump. Press Enter to unload instead, or run `--quiet`.

### Release layout

The zip published as `monodump.zip` should contain all four files:

```
monodump.exe            x64 injector
monodump_payload.dll    x64 payload
monodump32.exe          x86 injector
monodump_payload32.dll  x86 payload
```

Payload bitness must match the game.
