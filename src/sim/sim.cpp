#include "sim.h"
#include <cmath>

namespace sim {

    void init_world(World& w, uint64_t seed, size_t count) {
        w.seed = seed;
        w.count = count;

        w.position.resize(count);
        w.velocity.resize(count);
        w.color.resize(count);

        int cols = 317;
        for (size_t i = 0; i < count; i++) {
            int row = (int)i / cols;
            int col = (int)i % cols;

            w.position[i] = { (col - (cols / 2.0f)) * 0.6f, 0.0f, (row - ((int)count / cols) / 2.0f) * 0.6f };
            w.velocity[i] = { 0.0f, 0.0f, 0.0f };

            w.color[i].x = 0.2f + 0.8f * ((i % 11) / 10.0f);
            w.color[i].y = 0.2f + 0.8f * ((i % 7) / 6.0f);
            w.color[i].z = 0.2f + 0.8f * ((i % 13) / 12.0f);
        }
    }

    void wander_system(World& w, uint64_t tick, core::JobSystem& js, int num_threads, size_t chunk_size) {
        js.parallel_for(w.count, chunk_size, num_threads, [&](size_t begin, size_t end) {
            const float dt = 1.0f / 60.0f;
            const float speed = 1.5f;

            for (size_t i = begin; i < end; i++) {
                if ((tick + i) % 30 == 0) {
                    uint64_t hash_input = w.seed ^ ((tick / 30) * 0x9E3779B97F4A7C15ULL) ^ (i * 0xBF58476D1CE4E5B9ULL);
                    uint64_t h = splitmix64(hash_input);

                    float angle = (h % 360000) / 360000.0f * 6.2831853f;
                    w.velocity[i].x = std::cos(angle) * speed;
                    w.velocity[i].z = std::sin(angle) * speed;
                }

                w.position[i].x += w.velocity[i].x * dt;
                w.position[i].z += w.velocity[i].z * dt;
                w.position[i].y = 0.0f;

                if (w.position[i].x > 95.0f) {
                    w.position[i].x = 95.0f;
                    w.velocity[i].x = -w.velocity[i].x;
                }
                else if (w.position[i].x < -95.0f) {
                    w.position[i].x = -95.0f;
                    w.velocity[i].x = -w.velocity[i].x;
                }

                if (w.position[i].z > 95.0f) {
                    w.position[i].z = 95.0f;
                    w.velocity[i].z = -w.velocity[i].z;
                }
                else if (w.position[i].z < -95.0f) {
                    w.position[i].z = -95.0f;
                    w.velocity[i].z = -w.velocity[i].z;
                }
            }
            });
    }

    void step(World& w, uint64_t tick, core::JobSystem& js, int num_threads) {
        wander_system(w, tick, js, num_threads);
    }

    // Algoritmo FNV-1a (64-bit) rodando estritamente em serial para garantir a ordem dos bytes
    uint64_t state_hash(const World& w, uint64_t tick) {
        const uint64_t fnv_offset_basis = 14695981039346656037ULL;
        const uint64_t fnv_prime = 1099511628211ULL;
        uint64_t hash = fnv_offset_basis;

        // Lambda para iterar byte a byte e acumular o hash
        auto hash_bytes = [&](const uint8_t* data, size_t size) {
            for (size_t i = 0; i < size; ++i) {
                hash ^= data[i];
                hash *= fnv_prime;
            }
            };

        // As std::vector garantem que os dados SoA sejam blocos contíguos perfeitos na memória
        hash_bytes(reinterpret_cast<const uint8_t*>(w.position.data()), w.position.size() * sizeof(Vec3));
        hash_bytes(reinterpret_cast<const uint8_t*>(w.velocity.data()), w.velocity.size() * sizeof(Vec3));
        hash_bytes(reinterpret_cast<const uint8_t*>(&tick), sizeof(tick));

        return hash;
    }
}