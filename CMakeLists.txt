cmake_minimum_required(VERSION 3.28)
project(engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Busca do SDL3 mantida conforme a etapa anterior
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.12
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

# Busca do repositório oficial do Dear ImGui via FetchContent
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8
)
FetchContent_MakeAvailable(imgui)

# Cria a lib estática do ImGui manualmente contendo a core API e os backends solicitados.
# Necessário pois o repositório original do Dear ImGui não provê um CMakeLists nativo.
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)

# Exporta os headers do ImGui para quem linkar esta biblioteca (a engine)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# Linka o ImGui à versão estática do SDL3 para resolver as chamadas do backend
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3-static)

add_executable(engine src/main.cpp)

# A engine agora linka com o SDL3 e o nosso target customizado do ImGui
target_link_libraries(engine PRIVATE SDL3::SDL3-static imgui_lib)

if(MSVC)
    target_compile_options(engine PRIVATE /W4)
endif()