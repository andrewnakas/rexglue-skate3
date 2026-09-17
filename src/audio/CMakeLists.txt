# rexaudio - Audio subsystem library
# Audio processing and XMA decoding for Xbox 360 emulation

add_library(rexaudio OBJECT
    
    audio_driver.cpp
    audio_system.cpp
    xma_context.cpp
    xma_decoder.cpp
    xma_register_file.cpp
    # NOP backend (always available)
    nop/nop_audio_system.cpp
)
add_library(rex::audio ALIAS rexaudio)

target_include_directories(rexaudio PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(REXGLUE_USE_SDL)
    target_sources(rexaudio PRIVATE
        sdl/sdl_audio_system.cpp
        sdl/sdl_audio_driver.cpp
    )
elseif(REX_SWITCH)
    # audout rather than audren: the guest mixes to stereo itself, and audout is
    # the thinner of the two paths to the speakers.
    target_sources(rexaudio PRIVATE
        audout/audout_audio_system.cpp
        audout/audout_audio_driver.cpp
    )
endif()

target_link_libraries(rexaudio
    PUBLIC rexcore
    PRIVATE libavcodec libavutil
)
if(REXGLUE_USE_SDL)
    target_link_libraries(rexaudio PUBLIC SDL3::SDL3)
elseif(REX_SWITCH)
    target_link_libraries(rexaudio PUBLIC nx)
endif()
