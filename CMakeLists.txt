cmake_minimum_required(VERSION 3.28)
project(engine CXX)

# Garante o padrão C++20 obrigatório para a engine
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Busca o SDL3 via FetchContent ancorado na tag exata solicitada
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.12
)

# Força a linkagem estática globalmente ANTES do MakeAvailable
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL3)

# Define o executável a partir do main clássico
add_executable(engine src/main.cpp)

# Linka diretamente a versão estática do SDL3
target_link_libraries(engine PRIVATE SDL3::SDL3-static)

# Habilita Nível 4 de warnings de forma isolada se estiver no MSVC
if(MSVC)
    target_compile_options(engine PRIVATE /W4)
endif()