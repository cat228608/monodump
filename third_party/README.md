# third_party / MinHook

MinHook is needed for **main-thread dispatch** (`src/dispatch.h`): monodump hooks
the runtime's own `mono_runtime_invoke` so queued commands execute on the thread
that runs Unity's `Update()`. Without that, `call` runs on the watcher thread and
most UnityEngine APIs either do nothing or crash the game.

The build works without MinHook too - CMake prints a warning and
`dispatch::Available()` returns false - but do not ship that build.

## Layout

Download the sources from https://github.com/TsudaKageyu/minhook and lay them out
exactly like this:

```
third_party/minhook/include/MinHook.h
third_party/minhook/src/buffer.c
third_party/minhook/src/buffer.h
third_party/minhook/src/hook.c
third_party/minhook/src/trampoline.c
third_party/minhook/src/trampoline.h
third_party/minhook/src/hde/hde32.c
third_party/minhook/src/hde/hde32.h
third_party/minhook/src/hde/hde64.c
third_party/minhook/src/hde/hde64.h
third_party/minhook/src/hde/pstdint.h
third_party/minhook/src/hde/table32.h
third_party/minhook/src/hde/table64.h
```

License is BSD-2-Clause - fine to publish on GitHub. Keep MinHook's LICENSE file
next to the sources.

A quick way to get it:

```
git clone --depth 1 https://github.com/TsudaKageyu/minhook third_party/minhook
```
