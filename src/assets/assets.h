#pragma once
#include <vector>
#include <cstdint>
#include <string>

struct MeshData {
    std::vector<float> pos_rgb; // 6 floats/vértice (x,y,z,r,g,b)
    std::vector<uint16_t> indices;
    bool ok;
    std::string error;
};

MeshData assets_load_glb(const char* path);