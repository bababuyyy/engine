#pragma once
#include <vector>
#include <cstdint>

namespace clouds {
    // Flag do "parafuso de ouro": substitui o ruido por uma esfera puramente matematica
    extern bool CLOUD_DIAGNOSTIC_VOLUME;

    // Gera ou recarrega do cache o volume 3D tileavel de ruido Perlin-Worley (R8)
    std::vector<uint8_t> generate_cloud_noise(int w, int h, int d);
}