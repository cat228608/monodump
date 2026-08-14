cmake_minimum_required(VERSION 3.15)
project(monodump CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "Build with MSVC. The payload uses SEH (__try/__except).")
endif()

# Static CRT so the payload does not need a redistributable inside the game.
# A missing MSVC runtime is the #1 reason LoadLibraryA returns NULL after injection.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ------------------------------------------------------------------- MinHook
# Optional. Needed for main-thread dispatch (see src/dispatch.h). Drop the
# sources into third_party/minhook (layout in third_party/README.md) and this
# picks them up automatically. Without it everything still builds; commands then
# run on the watcher thread, which is unsafe for most UnityEngine calls.
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook")
set(MONODUMP_HAVE_MINHOOK OFF)

if(EXISTS "${MINHOOK_DIR}/include/MinHook.h")
    add_library(minhook STATIC
        "${MINHOOK_DIR}/src/buffer.c"
        "${MINHOOK_DIR}/src/hook.c"
        "${MINHOOK_DIR}/src/trampoline.c"
        "${MINHOOK_DIR}/src/hde/hde32.c"
        "${MINHOOK_DIR}/src/hde/hde64.c"
    )
    target_include_directories(minhook PUBLIC "${MINHOOK_DIR}/include")
    set(MONODUMP_HAVE_MINHOOK ON)
else()
    message(WARNING
        "MinHook not found in third_party/minhook - building WITHOUT main-thread "
        "dispatch. Managed calls will run on the watcher thread, which crashes or "
        "silently no-ops for most UnityEngine APIs. See third_party/README.md.")
endif()

# ---------------------------------------------------------------- payload dll
add_library(monodump_payload SHARED
    src/dllmain.cpp
    src/dispatch.cpp
    src/dumper.cpp
    src/invoker.cpp
    src/log.cpp
    src/mono_api.cpp
)
target_include_directories(monodump_payload PRIVATE src)
target_link_libraries(monodump_payload PRIVATE psapi)
set_target_properties(monodump_payload PROPERTIES
    OUTPUT_NAME "monodump_payload"
    PREFIX ""
)

if(MONODUMP_HAVE_MINHOOK)
    target_link_libraries(monodump_payload PRIVATE minhook)
    target_compile_definitions(monodump_payload PRIVATE MONODUMP_WITH_MINHOOK)
endif()

# ------------------------------------------------------------------- injector
add_executable(monodump injector/main.cpp)
target_link_libraries(monodump PRIVATE psapi)

# both land in the same folder - the injector expects the payload beside it
set_target_properties(monodump monodump_payload PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
