#pragma once
#include <cstdint>
#include <vector>
#include "../core/jobs.h"

namespace sim {

    struct Vec3 { float x, y, z; };

    inline uint64_t splitmix64(uint64_t state) {
        state += 0x9e3779b97f4a7c15ULL;
        state = (state ^ (state >> 30)) * 0xbf58476d1ce4e5b9ULL;
        state = (state ^ (state >> 27)) * 0x94d049bb133111ebULL;
        return state ^ (state >> 31);
    }

    struct World {
        uint64_t seed;
        std::vector<Vec3> position;
        std::vector<Vec3> velocity;
        std::vector<Vec3> color;
        size_t count;
    };

    void init_world(World& w, uint64_t seed, size_t count);

    // Atualização das assinaturas com integração transparente ao JobSystem
    void wander_system(World& w, uint64_t tick, core::JobSystem& js, int num_threads, size_t chunk_size = 4096);
    void step(World& w, uint64_t tick, core::JobSystem& js, int num_threads);

}