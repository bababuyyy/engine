#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <string>

#include "core/jobs.h"
#include "sim/sim.h"
#include "gen/gen.h"
#include "level/level.h"
#include "social/social.h"

int main(int argc, char* argv[]) {
    std::cout << "Binario compilado em: " << __DATE__ << " " << __TIME__
        << " | WANDER_SPEED=" << sim::WANDER_SPEED << "\n";

    bool write_golden = false;
    bool no_golden = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--write-golden") write_golden = true;
        if (std::string(argv[i]) == "--no-golden") no_golden = true;
    }

    core::JobSystem js;
    int hw_threads = (int)std::thread::hardware_concurrency();
    if (hw_threads < 1) hw_threads = 1;

    const uint64_t SEED = 42;
    const size_t COUNT = 10000;
    const uint64_t MAX_TICKS = 10000;
    const uint64_t CHECK_INTERVAL = 1000;

    bool all_passed = true;

    // ==============================================================================
    // [PREPARO] MUNDO A (Serial) vs MUNDO B (Paralelo) + Integração de NPCs
    // ==============================================================================

    social::Schedule real_scheds[5];
    for (int i = 0; i < 5; ++i) {
        std::ifstream file("data/schedules/npc" + std::to_string(i) + ".txt");
        if (file) {
            std::stringstream buffer; buffer << file.rdbuf();
            real_scheds[i] = social::parse_schedule(buffer.str().c_str());
        }
    }

    social::NpcState npcsA[5], npcsB[5];
    for (int i = 0; i < 5; ++i) {
        social::init_npc(npcsA[i], real_scheds[i]);
        social::init_npc(npcsB[i], real_scheds[i]);
    }

    sim::World worldA;
    sim::init_world(worldA, SEED, COUNT);
    std::vector<uint64_t> hashesA;

    for (uint64_t t = 1; t <= MAX_TICKS; ++t) {
        sim::step(worldA, t - 1, js, 1);

        int minute = (int)(((t - 1) / 120) % 1440);
        for (int i = 0; i < 5; ++i) social::social_tick(npcsA[i], real_scheds[i], minute);

        if (t % CHECK_INTERVAL == 0) {
            uint64_t base_h = sim::state_hash(worldA, t);
            hashesA.push_back(social::hash_npcs(base_h, npcsA, 5));
        }
    }

    sim::World worldB;
    sim::init_world(worldB, SEED, COUNT);
    std::vector<uint64_t> hashesB;

    // ==============================================================================
    // [TESTE 1] INVARIANTES
    // ==============================================================================

    bool t1_pass = true;
    for (uint64_t t = 1; t <= MAX_TICKS; ++t) {
        sim::step(worldB, t - 1, js, hw_threads);

        int minute = (int)(((t - 1) / 120) % 1440);
        for (int i = 0; i < 5; ++i) social::social_tick(npcsB[i], real_scheds[i], minute);

        if (t % CHECK_INTERVAL == 0) {
            uint64_t base_h = sim::state_hash(worldB, t);
            hashesB.push_back(social::hash_npcs(base_h, npcsB, 5));

            if (worldB.count != COUNT) {
                std::cout << "[FAIL] INVARIANTES: Count mutado de " << COUNT << " para " << worldB.count << " no tick " << t << "\n";
                t1_pass = false;
            }

            for (size_t i = 0; i < worldB.count; ++i) {
                const auto& p = worldB.position[i];
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
                    std::cout << "[FAIL] INVARIANTES: NaN/Inf detectado no agente " << i << " no tick " << t << "\n";
                    t1_pass = false;
                }
                if (std::abs(p.x) > 96.0f || std::abs(p.z) > 96.0f) {
                    std::cout << "[FAIL] INVARIANTES: Agente " << i << " escapou dos limites (|x|/|z| > 96) no tick " << t << "\n";
                    t1_pass = false;
                }
                if (p.y != 0.0f) {
                    std::cout << "[FAIL] INVARIANTES: Agente " << i << " levantou voo (y != 0) no tick " << t << "\n";
                    t1_pass = false;
                }
            }
        }
    }

    if (t1_pass) std::cout << "[PASS] INVARIANTES: Agentes confinados e validos.\n";
    else all_passed = false;

    // ==============================================================================
    // [TESTE 2] DETERMINISMO DE JOB SYSTEM
    // ==============================================================================

    bool t2_pass = true;
    for (size_t i = 0; i < hashesA.size(); ++i) {
        if (hashesA[i] != hashesB[i]) {
            std::cout << "[FAIL] DETERMINISMO: Divergencia no tick " << (i + 1) * CHECK_INTERVAL
                << ". 1-thread: " << std::hex << hashesA[i] << " != N-threads: " << hashesB[i] << std::dec << "\n";
            t2_pass = false;
            break;
        }
    }

    if (t2_pass) std::cout << "[PASS] DETERMINISMO: Resultados identicos bit-a-bit testados entre 1 e " << hw_threads << " threads.\n";
    else all_passed = false;

    // ==============================================================================
    // [TESTE 3] GOLDEN FILE (Condicionado)
    // ==============================================================================

    bool t3_pass = true;

    if (no_golden) {
        std::cout << "[SKIP] GOLDEN FILE: desabilitado nesta configuracao.\n";
    }
    else {
        std::string golden_path = "tests/golden_hashes.txt";

        if (write_golden) {
            std::ofstream fout(golden_path);
            if (fout) {
                for (auto h : hashesA) fout << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << h << "\n";
                std::cout << "[PASS] GOLDEN FILE: golden gerado, commite-o (" << golden_path << ").\n";
            }
            else {
                std::cout << "[FAIL] GOLDEN FILE: Erro critico na escrita de diretorio.\n";
                t3_pass = false;
            }
        }
        else {
            std::ifstream fin(golden_path);

            if (!fin.is_open()) {
                std::cout << "[FAIL] Golden ausente. Rode sim_tests --write-golden a partir da raiz do projeto para gerar a assinatura, confira o primeiro hash e commite o arquivo.\n";
                t3_pass = false;
            }
            else {
                std::vector<uint64_t> golden_hashes;
                std::string line;
                while (std::getline(fin, line)) {
                    if (line.empty()) continue;
                    golden_hashes.push_back(std::stoull(line, nullptr, 16));
                }

                if (golden_hashes.size() != hashesA.size()) {
                    std::cout << "[FAIL] GOLDEN FILE: Arquivo corrompido ou mismatch de lotes ("
                        << golden_hashes.size() << " armazenados, " << hashesA.size() << " calculados).\n";
                    t3_pass = false;
                }
                else {
                    for (size_t i = 0; i < hashesA.size(); ++i) {
                        if (hashesA[i] != golden_hashes[i]) {
                            std::cout << "[FAIL] GOLDEN FILE: Divergencia detectada na linha " << i + 1
                                << " (tick " << (i + 1) * CHECK_INTERVAL << "). "
                                << "Calculado: " << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << hashesA[i]
                                << " != Golden: " << golden_hashes[i] << std::dec << "\n";
                            t3_pass = false;
                            break;
                        }
                    }
                }

                if (t3_pass) std::cout << "[PASS] GOLDEN FILE: Execucao 100% perfeitamente amarrada ao build anterior.\n";
            }
        }
    }

    if (!t3_pass) all_passed = false;

    // ==============================================================================
    // [TESTE 4 e 4b] MASMORRA PROCEDURAL
    // ==============================================================================
    auto check_connectivity = [](const gen::GridData& gd, int& out_reached, int& out_total_floor) -> bool {
        out_total_floor = 0;
        for (int z = 0; z < 96; ++z) {
            for (int x = 0; x < 96; ++x) {
                if (gd.cells[z][x]) out_total_floor++;
            }
        }
        if (!gd.cells[gd.spawn_z][gd.spawn_x]) return false;
        std::vector<std::pair<int, int>> q;
        bool vis[96][96] = { false };
        q.push_back({ gd.spawn_x, gd.spawn_z });
        vis[gd.spawn_z][gd.spawn_x] = true;
        out_reached = 0;
        while (!q.empty()) {
            auto [cx, cz] = q.back();
            q.pop_back();
            out_reached++;
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
        return out_reached == out_total_floor;
        };

    bool t4_pass = true;
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        gen::GridData gd = gen::gen_grid(seed);
        if (gd.room_count < 8 || gd.room_count > 12) { t4_pass = false; break; }
        if (!gd.cells[gd.spawn_z][gd.spawn_x]) { t4_pass = false; break; }
        int reached = 0, total_floor = 0;
        if (!check_connectivity(gd, reached, total_floor)) { t4_pass = false; break; }
        gen::GenResult res1 = gen::gen_dungeon(seed);
        gen::GenResult res2 = gen::gen_dungeon(seed);
        if (res1.box_count <= 0 || res1.box_count >= 5000) { t4_pass = false; break; }
        if (res1.layout_hash != res2.layout_hash) { t4_pass = false; break; }
    }
    if (t4_pass) std::cout << "[PASS] MASMORRA: 20 seeds validadas (8-12 salas, conectividade garantida, determinismo verificado).\n";
    else all_passed = false;

    bool t4b_pass = true;
    gen::GridData fake_gd = {};
    fake_gd.cells[10][10] = true; fake_gd.cells[10][11] = true;
    fake_gd.spawn_x = 10; fake_gd.spawn_z = 10;
    fake_gd.cells[50][50] = true;
    int fake_reached = 0, fake_total = 0;
    if (check_connectivity(fake_gd, fake_reached, fake_total)) {
        std::cout << "[FAIL] NEGATIVE FIXTURE: O validador de conectividade APROVOU uma grade sabidamente desconexa!\n";
        t4b_pass = false;
    }
    else {
        std::cout << "[PASS] NEGATIVE FIXTURE: Validador reprovou corretamente as ilhas desconexas em grade forjada.\n";
    }
    if (!t4b_pass) all_passed = false;

    // ==============================================================================
    // [TESTE 5] SOCIAL
    // ==============================================================================
    bool t5_pass = true;
    const char* dummy_sched = "0800 10 10 trabalhar\n# lixo\n1200 0 0 almocar\ninvalida 0 0 \n1400 10 10 trabalhar";
    social::Schedule tsched = social::parse_schedule(dummy_sched);

    if (!tsched.ok || tsched.entries.size() != 3) {
        std::cout << "[FAIL] SOCIAL: Erro no parse da agenda. Quantidade incorreta pos purga.\n";
        t5_pass = false;
    }

    social::NpcState tnpc1, tnpc2;
    social::init_npc(tnpc1, tsched);
    social::init_npc(tnpc2, tsched);

    bool arrived_at_noon = false;
    bool deterministic = true;
    bool flee_worked = false;
    bool flee_resumed = false;

    // Teste comprimido: 120 ticks = 1 minuto de jogo -> 2 dias completos em 172800 ticks
    for (uint64_t t = 1; t <= 172800; ++t) {
        int minute = (int)((t / 120) % 1440);
        social::social_tick(tnpc1, tsched, minute);
        social::social_tick(tnpc2, tsched, minute);

        // Determinismo: comparar apenas ANTES da injecao (depois divergem por design)
        if (t < 780 * 120 && (tnpc1.x != tnpc2.x || tnpc1.z != tnpc2.z)) deterministic = false;

        // 12:05 do dia 1: ja chegou ao almoco (0,0)
        if (t == 725 * 120) {
            arrived_at_noon = (std::sqrt(tnpc1.x * tnpc1.x + tnpc1.z * tnpc1.z) <= 0.5f);
        }

        // 13:00: injeta fuga para (50,50)
        if (t == 780 * 120) {
            tnpc1.action = 1;
            tnpc1.flee_x = 50.0f;
            tnpc1.flee_z = 50.0f;
        }

        // Entre 13:00 e 14:00: chegou em (50,50) e voltou a AGENDA
        if (t > 780 * 120 && t < 840 * 120) {
            if (tnpc1.action == 0 && std::sqrt(std::pow(tnpc1.x - 50.0f, 2) + std::pow(tnpc1.z - 50.0f, 2)) < 0.2f) flee_worked = true;
        }

        // 15:00: retomou a agenda e esta no trabalho (10,10)
        if (t == 900 * 120) {
            if (std::sqrt(std::pow(tnpc1.x - 10.0f, 2) + std::pow(tnpc1.z - 10.0f, 2)) < 0.5f) flee_resumed = true;
        }
    }

    if (!deterministic) { std::cout << "[FAIL] SOCIAL: Movimentacao nao convergiu deterministicamente.\n"; t5_pass = false; }
    if (!arrived_at_noon) { std::cout << "[FAIL] SOCIAL: O NPC nao obedeceu o relogio para chegar ao ponto 0, 0 (almocar).\n"; t5_pass = false; }
    if (!flee_worked) { std::cout << "[FAIL] SOCIAL: O primitivo de fuga foi abortado ou nao atingiu ponto final.\n"; t5_pass = false; }
    if (!flee_resumed) { std::cout << "[FAIL] SOCIAL: O resumo de agenda pos-fuga nao acionou sua trajetoria correta.\n"; t5_pass = false; }

    if (t5_pass) std::cout << "[PASS] SOCIAL: Parser resiliente, rotina deterministica e resume de fuga validados.\n";
    else all_passed = false;

    return all_passed ? 0 : 1;
}