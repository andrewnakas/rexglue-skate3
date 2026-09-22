set(REXSYSTEM_SOURCES
    runtime.cpp
    kernel_state.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    xevent.cpp
    xfile.cpp
    xmutant.cpp
    xsemaphore.cpp
    xtimer.cpp
    xiocompletion.cpp
    xnotifylistener.cpp
    xenumerator.cpp
    xsocket.cpp
    xsymboliclink.cpp
    xobject.cpp
    xmodule.cpp
    shared_library.cpp
    guest_path.cpp
    guest_pause.cpp

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)

# rexruntime aggregates rexcore/rexgraphics/rexui/rexaudio/rexinput/
# rexfilesystem/imgui/o1heap into one library. As a SHARED library every
# guest->kernel call crosses a dylib boundary that no optimizer can inline or
# devirtualize across, and it exports its whole symbol table.
#
# Nothing actually requires it to be shared: the only dlopen in the SDK
# (src/system/shared_library.cpp) loads GUEST modules, REX_WEAK_FUNC never
# appears in the generated sources, and guest->host dispatch goes through the
# PPCFuncMapping table by POINTER, not by name. Making it static is what lets
# -fvisibility=hidden and ThinLTO see the whole program.
#
# Default OFF so existing consumers are untouched; the macos-fast preset turns
# it on.
option(REXGLUE_RUNTIME_STATIC
       "Build rexruntime as a static library so the whole program links as one unit" OFF)
if(REXGLUE_RUNTIME_STATIC)
    add_library(rexruntime STATIC ${REXSYSTEM_SOURCES})
else()
    add_library(rexruntime SHARED ${REXSYSTEM_SOURCES})
endif()
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system ALIAS rexruntime)
add_library(rex::kernel ALIAS rexruntime)

set_target_properties(rexruntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND REXGLUE_USE_VULKAN)
    # iOS has no Vulkan loader to dlopen, so MoltenVK is linked in statically
    # here - this library carries rexui, which calls vkGetInstanceProcAddr
    # directly on iOS. Point REXGLUE_IOS_MOLTENVK_STATIC_LIB at the libMoltenVK.a
    # inside the *static* MoltenVK.xcframework of a release matching the Vulkan
    # headers in use.
    if(NOT REXGLUE_IOS_MOLTENVK_STATIC_LIB)
        message(FATAL_ERROR
            "iOS builds need -DREXGLUE_IOS_MOLTENVK_STATIC_LIB=<path to static ios-arm64 libMoltenVK.a>")
    endif()
    target_link_libraries(rexruntime PRIVATE
        "${REXGLUE_IOS_MOLTENVK_STATIC_LIB}"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework IOSurface"
        "-framework CoreGraphics"
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework UIKit"
    )
endif()

if(REX_SWITCH AND REXGLUE_USE_VULKAN)
    # NVK, Mesa's NVIDIA driver ported to Horizon. There is no loader and no
    # dlopen here: the driver is linked into the NRO and reached through the one
    # symbol it exports, vk_icdGetInstanceProcAddr (see vulkan_instance.cpp).
    #
    # REXGLUE_SWITCH_NVK_ROOT is a devkitPro-shaped prefix holding lib/ and
    # include/. It defaults to the portlibs directory, where "make install"
    # puts it, but can point anywhere - which is what a build that cannot write
    # to /opt uses.
    if(NOT REXGLUE_SWITCH_NVK_ROOT)
        set(REXGLUE_SWITCH_NVK_ROOT "${DEVKITPRO}/portlibs/switch")
    endif()
    set(_nvk_lib "${REXGLUE_SWITCH_NVK_ROOT}/lib")
    if(NOT EXISTS "${_nvk_lib}/libnvk.a")
        message(FATAL_ERROR
            "No libnvk.a under ${_nvk_lib}. Build NXVK (scripts/nxvk_build_on_linux.sh) and "
            "install it, or point -DREXGLUE_SWITCH_NVK_ROOT at the package.")
    endif()

    target_include_directories(rexruntime PUBLIC "${REXGLUE_SWITCH_NVK_ROOT}/include")

    # --whole-archive: the driver's entry point is reached only through the -u
    # below, so without it the linker would keep almost nothing. The support
    # archives resolve against each other in both directions, hence the group.
    target_link_libraries(rexruntime PUBLIC
        -Wl,-u,vk_icdGetInstanceProcAddr
        -Wl,--whole-archive "${_nvk_lib}/libnvk.a" -Wl,--no-whole-archive
        -Wl,--start-group "${_nvk_lib}/libnvk_support.a" -lz -lexpat -Wl,--end-group
        nx
    )
    # NVK ships two Rust archives that each carry their own copy of the Rust
    # runtime. Mesa's own link absorbs that internally; a downstream consumer
    # cannot, so it is allowed here and nowhere else.
    target_link_options(rexruntime PUBLIC "LINKER:--allow-multiple-definition")
endif()

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
    PRIVATE
        aes128
        mspack
)

# SDL comes in through the ui, audio and input libraries, which is where it is
# actually used; naming it here as well is only a convenience for consumers
# that link the runtime alone. On a build without SDL the target does not
# exist, and referencing a target that does not exist is a configure error
# rather than something the generator can skip.
if(REXGLUE_USE_SDL)
    target_link_libraries(rexruntime PUBLIC SDL3::SDL3)
elseif(REX_SWITCH)
    target_link_libraries(rexruntime PUBLIC nx)
endif()

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexruntime PUBLIC TracyClient)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime PUBLIC REXGLUE_ENABLE_PERF_COUNTERS)
endif()

if(WIN32)
    target_link_libraries(rexruntime PRIVATE ws2_32)

    # Version metadata for the runtime DLL. Unsigned binaries without any
    # version resource are prone to antivirus false positives; embedding
    # identity here also lets crash dumps and support logs name the build.
    # The embedding application may override these before including the SDK.
    enable_language(RC)
    if(NOT REXGLUE_RUNTIME_RC_VERSION)
        set(REXGLUE_RUNTIME_RC_VERSION "0,0,0,0")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_VERSION_STRING)
        set(REXGLUE_RUNTIME_RC_VERSION_STRING "0.0.0")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_PRODUCT_NAME)
        set(REXGLUE_RUNTIME_RC_PRODUCT_NAME "rexglue")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_COMPANY)
        set(REXGLUE_RUNTIME_RC_COMPANY "rexglue project")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_DESCRIPTION)
        set(REXGLUE_RUNTIME_RC_DESCRIPTION "rexglue guest runtime library")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_COPYRIGHT)
        set(REXGLUE_RUNTIME_RC_COPYRIGHT "(c) rexglue contributors")
    endif()
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/rexruntime_version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/rexruntime_version.rc
        @ONLY)
    target_sources(rexruntime PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/rexruntime_version.rc)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime PUBLIC dxgi dxguid dxc-headers)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime PUBLIC
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        SPIRV-Tools-static
    )
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()
