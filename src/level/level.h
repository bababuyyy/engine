#pragma once
#include <vector>

// Header 100% puro C++ (Sem contaminação de SDL, Jolt, ou Math libs)

namespace level {
    struct LevelBox {
        float cx, cy, cz;
        float hx, hy, hz;
        float r, g, b;
    };

    struct SpawnPoint {
        float x, y, z;
    };

    struct LevelData {
        std::vector<LevelBox> boxes;
        SpawnPoint spawn;
        bool ok;
    };

    LevelData load_level(const char* path);
}