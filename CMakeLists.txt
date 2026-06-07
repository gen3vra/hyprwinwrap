cmake_minimum_required(VERSION 3.27)

project(hyprwinwrap
    DESCRIPTION "hyprwinwrap plugin for Hyprland"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)

file(GLOB_RECURSE SRC "*.cpp")

add_library(hyprwinwrap SHARED ${SRC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
)

# Match the Lua version Hyprland is built against (5.5), falling back to others.
pkg_search_module(lua REQUIRED lua55 lua5.5 lua-5.5 lua5.4 lua)

# Use pkg-config include dirs/libs and add common Hyprland protocol fallbacks.
target_include_directories(hyprwinwrap PRIVATE ${deps_INCLUDE_DIRS} ${lua_INCLUDE_DIRS}
    /usr/include/x86_64-linux-gnu/hyprland/protocols
    /usr/include/hyprland/protocols
    /usr/local/include/hyprland/protocols
)

target_link_libraries(hyprwinwrap PRIVATE rt ${deps_LIBRARIES})

install(TARGETS hyprwinwrap)
