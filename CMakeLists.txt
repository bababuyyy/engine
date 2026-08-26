cmake_minimum_required(VERSION 3.28)
project(engine CXX)

enable_testing()

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

FetchContent_Declare(
    JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    SOURCE_SUBDIR "Build"
)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(JoltPhysics)

FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.15
)
FetchContent_Populate(cgltf)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 31c1ad374564
)
FetchContent_Populate(stb)

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

# M2.5-11: A cadeia de compilacao FXC agora integra o Sky Shader
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/basic_vs.h ${CMAKE_CURRENT_BINARY_DIR}/basic_ps.h
           ${CMAKE_CURRENT_BINARY_DIR}/fullscreen_vs.h ${CMAKE_CURRENT_BINARY_DIR}/fullscreen_ps.h
           ${CMAKE_CURRENT_BINARY_DIR}/outline_vs.h ${CMAKE_CURRENT_BINARY_DIR}/outline_ps.h
           ${CMAKE_CURRENT_BINARY_DIR}/sky_vs.h ${CMAKE_CURRENT_BINARY_DIR}/sky_ps.h
    COMMAND ${FXC} /T vs_5_1 /E VSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/basic_vs.h /Vn g_basic_vs ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/basic.hlsl
    COMMAND ${FXC} /T ps_5_1 /E PSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/basic_ps.h /Vn g_basic_ps ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/basic.hlsl
    COMMAND ${FXC} /T vs_5_1 /E VSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/fullscreen_vs.h /Vn g_fullscreen_vs ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/fullscreen.hlsl
    COMMAND ${FXC} /T ps_5_1 /E PSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/fullscreen_ps.h /Vn g_fullscreen_ps ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/fullscreen.hlsl
    COMMAND ${FXC} /T vs_5_1 /E VSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/outline_vs.h /Vn g_outline_vs ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/outline.hlsl
    COMMAND ${FXC} /T ps_5_1 /E PSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/outline_ps.h /Vn g_outline_ps ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/outline.hlsl
    COMMAND ${FXC} /T vs_5_1 /E VSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/sky_vs.h /Vn g_sky_vs ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/sky.hlsl
    COMMAND ${FXC} /T ps_5_1 /E PSMain /Fh ${CMAKE_CURRENT_BINARY_DIR}/sky_ps.h /Vn g_sky_ps ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/sky.hlsl
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/basic.hlsl ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/fullscreen.hlsl ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/outline.hlsl ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/sky.hlsl
)

add_executable(engine src/main.cpp src/sim/sim.cpp src/core/jobs.h src/sim/sim.h src/phys/physics.h src/phys/physics.cpp src/level/level.cpp src/gen/gen.cpp src/assets/assets.cpp src/social/social.cpp)
target_sources(engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/basic_vs.h ${CMAKE_CURRENT_BINARY_DIR}/basic_ps.h ${CMAKE_CURRENT_BINARY_DIR}/fullscreen_vs.h ${CMAKE_CURRENT_BINARY_DIR}/fullscreen_ps.h ${CMAKE_CURRENT_BINARY_DIR}/outline_vs.h ${CMAKE_CURRENT_BINARY_DIR}/outline_ps.h ${CMAKE_CURRENT_BINARY_DIR}/sky_vs.h ${CMAKE_CURRENT_BINARY_DIR}/sky_ps.h)
target_include_directories(engine PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${cgltf_SOURCE_DIR} ${stb_SOURCE_DIR})
target_link_libraries(engine PRIVATE SDL3::SDL3-static imgui_lib glm::glm Jolt)
target_compile_definitions(engine PRIVATE 
    LEVELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/levels/"
    ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/"
    SCHEDULES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/schedules/"
    SOCIAL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/social/"
)

if(MSVC)
    target_compile_options(engine PRIVATE /W4)
endif()

add_executable(sim_tests tests/sim_tests.cpp src/sim/sim.cpp src/level/level.cpp src/gen/gen.cpp src/social/social.cpp)
target_include_directories(sim_tests PRIVATE src)
if(MSVC)
    target_compile_options(sim_tests PRIVATE /W4)
endif()
target_compile_definitions(sim_tests PRIVATE 
    SOCIAL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/social/"
)

if(CMAKE_BUILD_TYPE MATCHES "Rel")
    add_test(NAME sim_headless 
             COMMAND sim_tests
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else()
    add_test(NAME sim_headless 
             COMMAND sim_tests --no-golden
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

add_test(NAME regression_lint 
         COMMAND ${CMAKE_COMMAND} 
         -DSRC_FILE=${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp 
         -DJOBS_FILE=${CMAKE_CURRENT_SOURCE_DIR}/src/core/jobs.h
         -DTESTS_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/sim_tests.cpp
         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lint.cmake
)