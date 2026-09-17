# rexinput - Input system library
# Input system and drivers for Xbox 360 controller emulation

add_library(rexinput OBJECT
    input_system.cpp
    mnk/mnk_input_driver.cpp
    nop/nop_input_driver.cpp
)
add_library(rex::input ALIAS rexinput)

target_include_directories(rexinput PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(WIN32)
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp xinput/xinput_input_driver.cpp)
elseif(REXGLUE_USE_SDL)
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)
elseif(REX_SWITCH)
    # hid speaks the pad directly. It is also closer to the guest's model than
    # SDL is: the Xbox 360 pad and the Switch pad differ mainly in button names.
    target_sources(rexinput PRIVATE hid/hid_input_driver.cpp)
endif()

# On-screen controls: only where there is a touchscreen and no other pad.
# The Switch has one, but it also always has physical buttons, so the overlay
# is off by default there and enabled from the settings menu.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR ANDROID)
    target_sources(rexinput PRIVATE touch/touch_input_driver.cpp)
endif()

target_link_libraries(rexinput PUBLIC rexcore rexui)
if(REXGLUE_USE_SDL)
    target_link_libraries(rexinput PUBLIC SDL3::SDL3)
elseif(REX_SWITCH)
    target_link_libraries(rexinput PUBLIC nx)
endif()
