#pragma once
#include <vector>
#include <cstdint>
#include <string>

struct MeshData {
    std::vector<float> pos_rgb; // 6 floats/vértice (x,y,z,r,g,b)
    std::vector<float> uv;      // 2 floats/vértice (u,v)
    std::vector<float> normals; // 3 floats/vértice (nx,ny,nz)
    std::vector<uint16_t> indices;

    std::vector<unsigned char> tex_rgba;
    int tex_w = 0;
    int tex_h = 0;

    bool ok;
    std::string error;
};

MeshData assets_load_glb(const char* path);