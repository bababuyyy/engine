#include "gen.h"
#include <algorithm>
#include <vector>
#include <cmath>

namespace gen {

    // 1. RNG local puro encapsulado (splitmix64)
    static uint64_t next_u64(uint64_t& state) {
        state += 0x9e3779b97f4a7c15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    static uint32_t next_u32(uint64_t& state, uint32_t min_val, uint32_t max_val) {
        uint32_t range = max_val - min_val + 1;
        return min_val + (next_u64(state) % range);
    }

    struct Room {
        int x, z, w, h, cx, cz;
    };

    GridData gen_grid(uint64_t seed) {
        GridData gd = {};
        for (int z = 0; z < 96; ++z) {
            for (int x = 0; x < 96; ++x) {
                gd.cells[z][x] = false;
            }
        }

        uint64_t rng = seed;
        int target_rooms = next_u32(rng, 8, 12);
        gd.room_count = 0;

        std::vector<Room> rooms;

        // b. Posicionamento Aleatório com Rejeição de Sobreposição (Margem de 1)
        int attempts = 0;
        while (gd.room_count < target_rooms && attempts < 200) {
            attempts++;
            int w = next_u32(rng, 3, 6);
            int h = next_u32(rng, 3, 6);
            int x = next_u32(rng, 1, 96 - w - 1);
            int z = next_u32(rng, 1, 96 - h - 1);

            bool can_place = true;
            int check_min_x = std::max(0, x - 1);
            int check_max_x = std::min(95, x + w);
            int check_min_z = std::max(0, z - 1);
            int check_max_z = std::min(95, z + h);

            for (int nz = check_min_z; nz <= check_max_z; ++nz) {
                for (int nx = check_min_x; nx <= check_max_x; ++nx) {
                    if (gd.cells[nz][nx]) {
                        can_place = false;
                        break;
                    }
                }
                if (!can_place) break;
            }

            if (can_place) {
                for (int nz = z; nz < z + h; ++nz) {
                    for (int nx = x; nx < x + w; ++nx) {
                        gd.cells[nz][nx] = true;
                    }
                }
                Room r = { x, z, w, h, x + w / 2, z + h / 2 };
                rooms.push_back(r);
                gd.room_count++;
            }
        }

        if (gd.room_count > 0) {
            gd.spawn_x = rooms[0].cx;
            gd.spawn_z = rooms[0].cz;

            // c. Conexão Prim (Conectar à sala já conectada mais próxima)
            std::vector<bool> connected(gd.room_count, false);
            connected[0] = true;

            for (int step = 1; step < gd.room_count; ++step) {
                int best_c = -1, best_u = -1;
                int min_dist = 999999;

                for (int i = 0; i < gd.room_count; ++i) {
                    if (connected[i]) {
                        for (int j = 0; j < gd.room_count; ++j) {
                            if (!connected[j]) {
                                int dist = std::abs(rooms[i].cx - rooms[j].cx) + std::abs(rooms[i].cz - rooms[j].cz);
                                if (dist < min_dist) {
                                    min_dist = dist;
                                    best_c = i;
                                    best_u = j;
                                }
                            }
                        }
                    }
                }

                if (best_u != -1 && best_c != -1) {
                    connected[best_u] = true;
                    int cx1 = rooms[best_c].cx;
                    int cz1 = rooms[best_c].cz;
                    int cx2 = rooms[best_u].cx;
                    int cz2 = rooms[best_u].cz;

                    // Corredores Rasterizados na grade com 2 de largura
                    int min_x = std::min(cx1, cx2);
                    int max_x = std::max(cx1, cx2);
                    for (int nx = min_x; nx <= max_x; ++nx) {
                        gd.cells[cz1][nx] = true;
                        if (cz1 + 1 < 96) gd.cells[cz1 + 1][nx] = true;
                    }

                    int min_z = std::min(cz1, cz2);
                    int max_z = std::max(cz1, cz2);
                    for (int nz = min_z; nz <= max_z; ++nz) {
                        gd.cells[nz][cx2] = true;
                        if (cx2 + 1 < 96) gd.cells[nz][cx2 + 1] = true;
                    }
                }
            }
        }
        return gd;
    }

    uint64_t gen_layout_hash(const level::LevelData& data) {
        uint64_t hash = 14695981039346656037ULL;
        auto add_bytes = [&](const void* ptr, size_t size) {
            const uint8_t* p = (const uint8_t*)ptr;
            for (size_t i = 0; i < size; ++i) {
                hash ^= p[i];
                hash *= 1099511628211ULL;
            }
            };
        for (const auto& b : data.boxes) add_bytes(&b, sizeof(b));
        add_bytes(&data.spawn, sizeof(data.spawn));
        return hash;
    }

    GenResult gen_dungeon(uint64_t seed) {
        GridData gd = gen_grid(seed);
        GenResult res;
        res.level.ok = true;
        // e. Spawn configurado sobre o centro da primeira sala com Y = 1
        res.level.spawn = { (gd.spawn_x - 48.0f) * 2.0f, 1.0f, (gd.spawn_z - 48.0f) * 2.0f };
        res.room_count = gd.room_count;

        bool wall[96][96] = { false };
        for (int z = 0; z < 96; ++z) {
            for (int x = 0; x < 96; ++x) {
                if (!gd.cells[z][x]) {
                    int dx[] = { 1, -1, 0, 0 };
                    int dz[] = { 0, 0, 1, -1 };
                    bool has_neighbor = false;
                    for (int i = 0; i < 4; ++i) {
                        int nx = x + dx[i];
                        int nz = z + dz[i];
                        if (nx >= 0 && nx < 96 && nz >= 0 && nz < 96 && gd.cells[nz][nx]) {
                            has_neighbor = true;
                            break;
                        }
                    }
                    wall[z][x] = has_neighbor;
                }
            }
        }

        // d. Merge Guloso (Por linha - reduzindo caixas drasticamente e otimizando a colisão Jolt)
        auto merge_and_add = [&](bool grid[96][96], float cy, float h_y, float r, float g, float b) {
            for (int z = 0; z < 96; ++z) {
                for (int x = 0; x < 96; ++x) {
                    if (grid[z][x]) {
                        int start_x = x;
                        while (x < 96 && grid[z][x]) x++;
                        int end_x = x - 1;

                        float min_x = (start_x - 48) * 2.0f - 1.0f;
                        float max_x = (end_x - 48) * 2.0f + 1.0f;
                        float min_z = (z - 48) * 2.0f - 1.0f;
                        float max_z = (z - 48) * 2.0f + 1.0f;

                        level::LevelBox box;
                        box.cx = (min_x + max_x) * 0.5f;
                        box.cy = cy;
                        box.cz = (min_z + max_z) * 0.5f;
                        box.hx = (max_x - min_x) * 0.5f;
                        box.hy = h_y;
                        box.hz = (max_z - min_z) * 0.5f;
                        box.r = r; box.g = g; box.b = b;
                        res.level.boxes.push_back(box);
                    }
                }
            }
            };

        merge_and_add(gd.cells, -0.25f, 0.25f, 0.4f, 0.5f, 0.6f);
        merge_and_add(wall, 1.5f, 1.5f, 0.2f, 0.2f, 0.2f);

        res.box_count = (int)res.level.boxes.size();
        res.layout_hash = gen_layout_hash(res.level);
        return res;
    }
}