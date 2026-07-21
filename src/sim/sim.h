#pragma once
#include <cstdint>
#include <vector>

// Módulo completamente isolado (sem SDL, sem GLM, sem ImGui).
// Permite compilação headless estrita para os futuros testes unitários.
namespace sim {

    struct Vec3 { float x, y, z; };

    // Função pura e isolada de hash (SplitMix64).
    // Garante comportamento 100% reproduzível entre plataformas, diferente do std::rand().
    inline uint64_t splitmix64(uint64_t state) {
        state += 0x9e3779b97f4a7c15ULL;
        state = (state ^ (state >> 30)) * 0xbf58476d1ce4e5b9ULL;
        state = (state ^ (state >> 27)) * 0x94d049bb133111ebULL;
        return state ^ (state >> 31);
    }

    // Organização de dados em Estrutura de Arrays (SoA).
    // Otimiza o cache line da CPU durante a integração de massa.
    struct World {
        uint64_t seed;
        std::vector<Vec3> position;
        std::vector<Vec3> velocity;
        std::vector<Vec3> color;
        size_t count;
    };

    void init_world(World& w, uint64_t seed, size_t count);
    void wander_system(World& w, uint64_t tick);
    void step(World& w, uint64_t tick);

}