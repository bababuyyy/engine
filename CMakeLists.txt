cmake_minimum_required(VERSION 3.28)
project(engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.12
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3-static)

find_program(FXC fxc HINTS 
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/basic_vs.h ${CMAKE_CURRENT_BINARY_DIR}/basic_ps.h
    COMMAND ${FXC} /T vs_5_1 /E VSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/basic_vs.h /Vn g_basic_vs ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/basic.hlsl
    COMMAND ${FXC} /T ps_5_1 /E PSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/basic_ps.h /Vn g_basic_ps ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/basic.hlsl
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/basic.hlsl
)

# Adiciona o novo módulo sim isolado da Engine na compilação
add_executable(engine src/main.cpp src/sim/sim.cpp)

target_sources(engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/basic_vs.h ${CMAKE_CURRENT_BINARY_DIR}/basic_ps.h)
target_include_directories(engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(engine PRIVATE SDL3::SDL3-static imgui_lib glm::glm)

if(MSVC)
    target_compile_options(engine PRIVATE /W4)
endif()