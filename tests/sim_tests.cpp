#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <string>

// Prova de pureza: Inclusão única dos módulos nativos.
// Headers de janelas ou renderizadores causariam Linker Error imediatamente.
#include "core/jobs.h"
#include "sim/sim.h"

int main(int argc, char* argv[]) {
    std::cout << "Binario compilado em: " << __DATE__ << " " << __TIME__
        << " | WANDER_SPEED=" << sim::WANDER_SPEED << "\n";
    bool write_golden = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--write-golden") {
            write_golden = true;
        }
    }

    core::JobSystem js;
    int hw_threads = (int)std::thread::hardware_concurrency();
    if (hw_threads < 1) hw_threads = 1;

    // Configurações enxutas controladas para rodar em segundos no CTest
    const uint64_t SEED = 42;
    const size_t COUNT = 10000;
    const uint64_t MAX_TICKS = 10000;
    const uint64_t CHECK_INTERVAL = 1000;

    bool all_passed = true;

    // ==============================================================================
    // [PREPARO] MUNDO A (Serial de 1 Thread) vs MUNDO B (Paralelo de N Threads)
    // ==============================================================================

    sim::World worldA;
    sim::init_world(worldA, SEED, COUNT);
    std::vector<uint64_t> hashesA;

    // Preenche o mundo A iterativamente anotando seus checkpoints físicos
    for (uint64_t t = 1; t <= MAX_TICKS; ++t) {
        sim::step(worldA, t - 1, js, 1);
        if (t % CHECK_INTERVAL == 0) {
            hashesA.push_back(sim::state_hash(worldA, t));
        }
    }

    sim::World worldB;
    sim::init_world(worldB, SEED, COUNT);
    std::vector<uint64_t> hashesB;

    // ==============================================================================
    // [TESTE 1] INVARIANTES: Agentes não destroem seus bounds ou estouram a física.
    // ==============================================================================

    bool t1_pass = true;
    for (uint64_t t = 1; t <= MAX_TICKS; ++t) {
        sim::step(worldB, t - 1, js, hw_threads);

        if (t % CHECK_INTERVAL == 0) {
            hashesB.push_back(sim::state_hash(worldB, t));

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
                // O limite tolerado aqui é 96 para abarcar flutuação de ponto na parede invisível de 95 do wander
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
    // [TESTE 2] DETERMINISMO DE JOB SYSTEM: FNV-1a Bit-to-bit matching entre threads.
    // ==============================================================================

    bool t2_pass = true;
    for (size_t i = 0; i < hashesA.size(); ++i) {
        if (hashesA[i] != hashesB[i]) {
            std::cout << "[FAIL] DETERMINISMO: Divergencia no tick " << (i + 1) * CHECK_INTERVAL
                << ". 1-thread: " << std::hex << hashesA[i] << " != N-threads: " << hashesB[i] << std::dec << "\n";
            t2_pass = false;
            break; // A primeira falha aciona o log early-out estilo Factorio
        }
    }

    if (t2_pass) std::cout << "[PASS] DETERMINISMO: Resultados identicos bit-a-bit testados entre 1 e " << hw_threads << " threads.\n";
    else all_passed = false;


    // ==============================================================================
    // [TESTE 3] GOLDEN FILE: Assinatura cravada contra quebra do código-fonte.
    // ==============================================================================

    bool t3_pass = true;
    std::string golden_path = "tests/golden_hashes.txt";
    std::ifstream fin(golden_path);

    // Salva automaticamente no primeiro run ou se forçado pelo arquiteto
    if (!fin.is_open() || write_golden) {
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

    if (!t3_pass) all_passed = false;

    // Retorna zero somente se os 3 guardiões reportarem positivo
    return all_passed ? 0 : 1;
}