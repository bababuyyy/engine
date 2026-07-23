#pragma once
#include <vector>
#include <cstdint>
#include <string>

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
        char categoria[16];
        bool ok;
    };

    struct Relations {
        struct Entry { char catA[16]; char catB[16]; float val; };
        std::vector<Entry> entries;
    };

    struct Barks {
        std::vector<std::string> amistoso;
        std::vector<std::string> neutro;
        std::vector<std::string> frio;
    };

    struct NpcState {
        float x, z;
        float tx, tz;
        int entry_atual;
        uint8_t action; // 0 = AGENDA, 1 = FUGA, 2 = COMER, 3 = CONVERSA
        float flee_x, flee_z;
        float fome;
        int last_minute;
        int conv_ticks;
        uint8_t partner;
        int64_t last_conv_abs_min;
    };

    struct ConvEvent {
        int npc_a;
        int npc_b;
        int tier; // 1 = amistoso, 0 = neutro, -1 = frio
    };

    // Parsers PUROS (sem I/O)
    Schedule parse_schedule(const char* texto);
    Relations parse_relations(const char* texto);
    Barks parse_barks(const char* texto);

    // Consulta de matriz de relações (simétrica)
    float get_relation(const Relations& rel, const char* catA, const char* catB);

    // Inicializa e resolve o estado contínuo do NPC
    void init_npc(NpcState& npc, const Schedule& sched);
    void social_tick(NpcState& npc, const Schedule& sched, int minuto_do_dia);

    // Sistema determinístico para cruzar trajetórias e disparar instâncias de conversação
    std::vector<ConvEvent> social_encounters(NpcState* npcs, const Schedule* scheds, int count, const Relations& rel, int64_t abs_min);

    // Amarra o estado de conversação e movimentação no hash global do mundo
    uint64_t hash_npcs(uint64_t base_hash, const NpcState* npcs, int count);
}