#pragma once
#include <vector>
#include <cstdint>

namespace social {

    struct SchedEntry {
        int minuto_do_dia;
        float x, z;
        char atividade[16];
    };

    struct Schedule {
        std::vector<SchedEntry> entries;
        float food_x;
        float food_z;
        bool ok;
    };

    struct NpcState {
        float x, z;
        float tx, tz;
        int entry_atual;
        uint8_t action; // 0 = AGENDA, 1 = FUGA, 2 = COMER
        float flee_x, flee_z;
        float fome;
        int last_minute;
    };

    // Recebe buffer de texto (conteúdo do arquivo) para parse PURO (sem I/O)
    Schedule parse_schedule(const char* texto);

    // Inicializa a posição baseando-se no que a agenda dita à meia-noite (00:00)
    void init_npc(NpcState& npc, const Schedule& sched);

    // Passo de simulação puramente determinístico e state-less (tick social + relógio por minuto)
    void social_tick(NpcState& npc, const Schedule& sched, int minuto_do_dia);

    // Adiciona as posições, ações e estados vitais dos NPCs ao hash do mundo
    uint64_t hash_npcs(uint64_t base_hash, const NpcState* npcs, int count);
}