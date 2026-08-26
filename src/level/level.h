#pragma once
#include <vector>
#include <string>

namespace level {
    struct BoxDef {
        float cx, cy, cz;
        float hx, hy, hz;
        float r, g, b;
    };

    struct SpawnDef {
        float x, y, z;
    };

    // T-A4: O novo componente Data-Driven de Níveis
    struct LevelMesh {
        std::string path;
        float x, y, z;
    };

    struct LevelData {
        std::vector<BoxDef> boxes;
        std::vector<LevelMesh> meshes;
        SpawnDef spawn;
        bool ok;
        bool has_terrain;
        int terrain_mesh_index;
    };

    LevelData load_level(const char* filepath);
}