# rexcore - Core utilities library
# Foundation types, data structures, and threading primitives

add_library(rexcore OBJECT
    arena.cpp
    bit_map.cpp
    bit_stream.cpp
    byte_stream.cpp
    clock.cpp
    cvar.cpp
    exception_handler.cpp
    filesystem.cpp
    filesystem_wildcard.cpp
    guest_presence.cpp
    logging.cpp
    memory.cpp
    mutex.cpp
    ring_buffer.cpp
    sha256.cpp
    string.cpp
    string_buffer.cpp
    threading.cpp
    timer_queue.cpp
    utf8.cpp
    vec128.cpp
)
add_library(rex::core ALIAS rexcore)

# Platform-specific sources
if(WIN32)
    target_sources(rexcore PRIVATE
        atomic_win.cpp
        clock_win.cpp
        dbg_win.cpp
        dynlib_win.cpp
        exception_handler_win.cpp
        filesystem_win.cpp
        fiber_win32.cpp
        mapped_memory_win.cpp
        math_msvc.cpp
        memory_win.cpp
        seh_win.cpp
        socket_win.cpp
        string_win.cpp
        system_win.cpp
        threading_win.cpp
    )
elseif(REX_SWITCH)
    # Horizon is not a UNIX and CMake knows it as Generic, so this branch has to
    # precede the UNIX one. Three of the POSIX files are portable enough to
    # reuse as they are - atomics, the monotonic clock and the string helpers
    # all sit on newlib and libstdc++ - and their platform assertions were
    # widened to say so. Everything else is written against libnx: there are no
    # signals, no mmap, no dlopen and no pthread_create here.
    enable_language(ASM)
    enable_language(C)
    target_sources(rexcore PRIVATE
        atomic_posix.cpp
        clock_posix.cpp
        math_gcc.cpp
        string_posix.cpp
        dbg_switch.cpp
        dynlib_switch.cpp
        exception_handler_switch.cpp
        execinfo_switch.cpp
        filesystem_switch.cpp
        main_switch.cpp
        mapped_memory_switch.cpp
        memory_switch.cpp
        seh_switch.cpp
        socket_switch.cpp
        system_switch.cpp
        threading_switch.cpp
        pthread_stack_switch.cpp
        # The guest's fibers are hand-written AArch64: no OS involvement, so the
        # same two files the phones use assemble unchanged here.
        fiber_aarch64.cpp
        fiber_aarch64.S
        # Newlib is missing a handful of symbols the Vulkan driver references.
        switch/nvk_compat.c
    )
elseif(UNIX)
    target_sources(rexcore PRIVATE
        atomic_posix.cpp
        clock_posix.cpp
        dbg_posix.cpp
        dynlib_posix.cpp
        exception_handler_posix.cpp
        filesystem_posix.cpp
        fiber_posix.cpp
        mapped_memory_posix.cpp
        math_gcc.cpp
        memory_posix.cpp
        main_android.cpp
        filesystem_android.cpp
        execinfo_android.cpp
        seh_posix.cpp
        socket_posix.cpp
        string_posix.cpp
        system_posix.cpp
        threading_posix.cpp
    )
    if(ANDROID OR REX_IOS)
        # Neither bionic nor the iOS SDK has makecontext/swapcontext;
        # fiber_posix.cpp compiles to nothing on both and this backend takes
        # over. The .S assembles for ELF and Mach-O alike.
        enable_language(ASM)
        target_sources(rexcore PRIVATE
            fiber_aarch64.cpp
            fiber_aarch64.S
        )
    endif()
endif()

target_include_directories(rexcore PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(rexcore PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/thirdparty/cli11/include
)

if(REX_SWITCH)
    # libnx is the whole platform library: no pthread, no dl, no rt.
    target_link_libraries(rexcore PRIVATE nx)
elseif(UNIX)
    if(ANDROID)
        # Bionic folds pthread, rt and dl into libc; there are no separate
        # libraries to link, and -lpthread/-lrt fail outright.
        target_link_libraries(rexcore PRIVATE log)
    else()
        target_link_libraries(rexcore PRIVATE pthread dl)
    endif()
    if(NOT APPLE AND NOT ANDROID)
        target_link_libraries(rexcore PRIVATE rt)
    endif()
elseif(WIN32)
    target_link_libraries(rexcore PUBLIC ws2_32)
endif()

target_link_libraries(rexcore
    PUBLIC
        fmt::fmt
        simde
        tomlplusplus
        spdlog::spdlog
    PRIVATE
        disruptorplus
        utf8cpp
)

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexcore PUBLIC TracyClient)
    target_compile_definitions(rexcore PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_sources(rexcore PRIVATE perf/counter.cpp)
    target_compile_definitions(rexcore PUBLIC REXGLUE_ENABLE_PERF_COUNTERS)
endif()
