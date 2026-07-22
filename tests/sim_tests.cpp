#include "sim/sim.h"
#include "core/jobs.h"
#include "gen/gen.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <iomanip>

int main(int argc, char* argv[]) {
    bool no_golden = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-golden") == 0) no_golden = true;
    }

    if (!no_golden) {
        sim::World world;
        sim::init_world(world, 42, 100000);
        core::JobSystem js;
        for (uint64_t tick = 0; tick < 60; ++tick) {
            sim::step(world, tick, js, 4);
        }
        uint64_t hash = sim::state_hash(world, 60);
        std::cout << "Teste Headless base concluido. Hash Golden: " << std::hex << hash << std::dec << std::endl;
    }

    // --- TESTE 4: Masmorra Procedural ---
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        gen::GridData gd = gen::gen_grid(seed);

        // (c) 8-12 salas
        if (gd.room_count < 8 || gd.room_count > 12) {
            std::cerr << "FAIL com a seed culpada " << seed << ": " << gd.room_count << " salas geradas (fora do escopo 8-12)." << std::endl;
            return 1;
        }

        // (b) spawn gerado dentro de sala (validar piso = true)
        if (!gd.cells[gd.spawn_z][gd.spawn_x]) {
            std::cerr << "FAIL com a seed culpada " << seed << ": spawn fora do piso." << std::endl;
            return 1;
        }

        // (a) Validação de Conectividade Extrema (Flood-Fill garantindo 100% de alcance para agentes e jogador)
        int total_floor = 0;
        for (int z = 0; z < 96; ++z) {
            for (int x = 0; x < 96; ++x) {
                if (gd.cells[z][x]) total_floor++;
            }
        }

        std::vector<std::pair<int, int>> q;
        bool vis[96][96] = { false };

        q.push_back({ gd.spawn_x, gd.spawn_z });
        vis[gd.spawn_z][gd.spawn_x] = true;
        int reached = 0;

        while (!q.empty()) {
            auto [cx, cz] = q.back();
            q.pop_back();
            reached++;

            int dx[] = { 1, -1, 0, 0 };
            int dz[] = { 0, 0, 1, -1 };
            for (int i = 0; i < 4; ++i) {
                int nx = cx + dx[i];
                int nz = cz + dz[i];
                if (nx >= 0 && nx < 96 && nz >= 0 && nz < 96) {
                    if (gd.cells[nz][nx] && !vis[nz][nx]) {
                        vis[nz][nx] = true;
                        q.push_back({ nx, nz });
                    }
                }
            }
        }

        if (reached != total_floor) {
            std::cerr << "FAIL com a seed culpada " << seed << ": salas orfas! Flood-fill alcancou apenas " << reached << " de " << total_floor << " celulas." << std::endl;
            return 1;
        }

        // (d e e) Contagem sadia e Determinismo Bit-Perfect 
        gen::GenResult res1 = gen::gen_dungeon(seed);
        gen::GenResult res2 = gen::gen_dungeon(seed);

        if (res1.box_count <= 0 || res1.box_count >= 5000) {
            std::cerr << "FAIL com a seed culpada " << seed << ": Quantidade de caixas gerada fora do budget (" << res1.box_count << ")." << std::endl;
            return 1;
        }

        if (res1.layout_hash != res2.layout_hash) {
            std::cerr << "FAIL com a seed culpada " << seed << ": Quebra de determinismo no hash de construcao procedural." << std::endl;
            return 1;
        }
    }

    std::cout << "Teste 4 (Masmorra) aprovado: Flood-fill assegura mapas conectados. Golden intocado." << std::endl;
    return 0;
}