#pragma once
#include <vector>
#include <cstdint>

namespace clouds {
    // Flag do "parafuso de ouro": substitui o ruido por uma esfera puramente matematica
    extern bool CLOUD_DIAGNOSTIC_VOLUME;

    // Gera ou recarrega do cache o volume 3D tileavel de ruido Perlin-Worley (R8)
    std::vector<uint8_t> generate_cloud_noise(int w, int h, int d);

    // M2.5-24C: Gera ou recarrega o ruido de detalhe 3D (Swiss Cheese)
    std::vector<uint8_t> generate_cloud_detail(int w, int h, int d);

    // M2.5-24G-1: Gera ou recarrega o weather map 2D tileavel (RGBA: R=coverage, G=tipo)
    std::vector<uint8_t> generate_weather_map(int w, int h);
}
