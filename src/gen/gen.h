#pragma once
#include "level/level.h"
#include <cstdint>

// Header puro: Sem dependências de simulação física, SDL, ImGui ou tipos GLM

namespace gen {
    struct GridData {
        bool cells[96][96];
        int spawn_x;
        int spawn_z;
        int room_count;
    };

    struct GenResult {
        level::LevelData level;
        uint64_t layout_hash;
        int room_count;
        int box_count;
    };

    // Exportado separadamente para propósitos de teste (validação de conectividade Headless)
    GridData gen_grid(uint64_t seed);

    // Geração master do domínio de masmorras retornando LevelData canônico
    GenResult gen_dungeon(uint64_t seed);

    // Hash do layout para teste de determinismo e rastreio estrutural (FNV-1a)
    uint64_t gen_layout_hash(const level::LevelData& data);
}