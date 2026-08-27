#include "cloud_noise.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cmath>
#include <algorithm>

namespace clouds {

    bool CLOUD_DIAGNOSTIC_VOLUME = false;

    namespace {
        inline float hash31(float x, float y, float z) {
            float ox = std::fmod(x * 12.9898f + y * 78.233f + z * 37.719f, 314.159f);
            return std::abs(std::fmod(std::sin(ox) * 43758.5453f, 1.0f));
        }

        inline float smooth(float t) {
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        float perlin_tileable(float x, float y, float z, int w, int h, int d) {
            int X = (int)std::floor(x);
            int Y = (int)std::floor(y);
            int Z = (int)std::floor(z);

            float u = x - X;
            float v = y - Y;
            float w_fac = z - Z;

            float u_smooth = smooth(u);
            float v_smooth = smooth(v);
            float w_smooth = smooth(w_fac);

            auto grad = [&](int i, int j, int k, float dx, float dy, float dz) {
                int xi = (i % w + w) % w;
                int yj = (j % h + h) % h;
                int zk = (k % d + d) % d;
                float h_val = hash31(xi, yj, zk);
                float theta = h_val * 2.0f * 3.14159265f;
                float phi = hash31(xi + 1, yj, zk) * 3.14159265f;
                float gx = std::sin(phi) * std::cos(theta);
                float gy = std::sin(phi) * std::sin(theta);
                float gz = std::cos(phi);
                return gx * dx + gy * dy + gz * dz;
                };

            float c000 = grad(X, Y, Z, u, v, w_fac);
            float c100 = grad(X + 1, Y, Z, u - 1, v, w_fac);
            float c010 = grad(X, Y + 1, Z, u, v - 1, w_fac);
            float c110 = grad(X + 1, Y + 1, Z, u - 1, v - 1, w_fac);
            float c001 = grad(X, Y, Z + 1, u, v, w_fac - 1);
            float c101 = grad(X + 1, Y, Z + 1, u - 1, v, w_fac - 1);
            float c011 = grad(X, Y + 1, Z + 1, u, v - 1, w_fac - 1);
            float c111 = grad(X + 1, Y + 1, Z + 1, u - 1, v - 1, w_fac - 1);

            float x00 = std::lerp(c000, c100, u_smooth);
            float x10 = std::lerp(c010, c110, u_smooth);
            float x01 = std::lerp(c001, c101, u_smooth);
            float x11 = std::lerp(c011, c111, u_smooth);

            float y0 = std::lerp(x00, x10, v_smooth);
            float y1 = std::lerp(x01, x11, v_smooth);

            return std::lerp(y0, y1, w_smooth) * 0.5f + 0.5f;
        }

        float worley_tileable(float x, float y, float z, int w, int h, int d) {
            int X = (int)std::floor(x);
            int Y = (int)std::floor(y);
            int Z = (int)std::floor(z);

            float min_dist = 1.0f;

            for (int k = -1; k <= 1; ++k) {
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i) {
                        int xi = (X + i % w + w) % w;
                        int yj = (Y + j % h + h) % h;
                        int zk = (Z + k % d + d) % d;

                        float p_x = hash31(xi, yj, zk);
                        float p_y = hash31(xi + 123, yj, zk);
                        float p_z = hash31(xi, yj + 123, zk);

                        float dx = (i + p_x) - (x - X);
                        float dy = (j + p_y) - (y - Y);
                        float dz = (k + p_z) - (z - Z);

                        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (dist < min_dist) min_dist = dist;
                    }
                }
            }
            return 1.0f - min_dist;
        }
    }

    std::vector<uint8_t> generate_cloud_noise(int w, int h, int d) {
        std::string cache_dir = "cache";
        std::string cache_file = cache_dir + "/cloud_noise_v1.bin";

        struct Header {
            char magic[4] = { 'C','L','N','1' };
            int w, h, d;
        } expected_header{ {'C','L','N','1'}, w, h, d };

        if (!CLOUD_DIAGNOSTIC_VOLUME) {
            std::ifstream in(cache_file, std::ios::binary);
            if (in.is_open()) {
                Header read_h;
                in.read(reinterpret_cast<char*>(&read_h), sizeof(Header));
                if (read_h.w == w && read_h.h == h && read_h.d == d &&
                    read_h.magic[0] == 'C' && read_h.magic[3] == '1') {
                    std::vector<uint8_t> data(w * h * d);
                    in.read(reinterpret_cast<char*>(data.data()), data.size());
                    if (in) {
                        std::cout << "[NUVENS] Cache carregado instantaneamente de: " << cache_file << "\n";
                        return data;
                    }
                }
            }
        }

        std::cout << "[NUVENS] Gerando volume procedural 3D (" << w << "x" << h << "x" << d << ")...\n";
        auto start = std::chrono::high_resolution_clock::now();

        std::vector<uint8_t> data(w * h * d);

        int cells_w = 8, cells_h = 2, cells_d = 8;
        int cells_w2 = 16, cells_h2 = 4, cells_d2 = 16;

        for (int z = 0; z < d; ++z) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    float fx = (float)x / w;
                    float fy = (float)y / h;
                    float fz = (float)z / d;

                    float val = 0.0f;

                    if (CLOUD_DIAGNOSTIC_VOLUME) {
                        float cx = 0.5f, cy = 0.5f, cz = 0.5f;
                        float dist = std::sqrt(std::pow(fx - cx, 2.0f) + std::pow(fy - cy, 2.0f) + std::pow(fz - cz, 2.0f));
                        val = std::clamp(1.0f - (dist / 0.35f), 0.0f, 1.0f);
                    }
                    else {
                        float perlin = perlin_tileable(fx * cells_w, fy * cells_h, fz * cells_d, cells_w, cells_h, cells_d);
                        float w1 = worley_tileable(fx * cells_w, fy * cells_h, fz * cells_d, cells_w, cells_h, cells_d);
                        float w2 = worley_tileable(fx * cells_w2, fy * cells_h2, fz * cells_d2, cells_w2, cells_h2, cells_d2);
                        float worley = w1 * 0.7f + w2 * 0.3f;
                        val = perlin - (1.0f - worley) * 0.3f;
                        val = std::clamp(val, 0.0f, 1.0f);
                    }

                    int idx = x + w * (y + h * z);
                    data[idx] = (uint8_t)(val * 255.0f);
                }
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;
        std::cout << "[NUVENS] Geração concluida em " << diff.count() << " segundos.\n";

        if (!CLOUD_DIAGNOSTIC_VOLUME) {
            std::filesystem::create_directories(cache_dir);
            std::ofstream out(cache_file, std::ios::binary);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(&expected_header), sizeof(Header));
                out.write(reinterpret_cast<const char*>(data.data()), data.size());
            }
        }

        return data;
    }

    // M2.5-24C: Swiss Cheese Inverted Worley FBM
    std::vector<uint8_t> generate_cloud_detail(int w, int h, int d) {
        std::string cache_dir = "cache";
        std::string cache_file = cache_dir + "/cloud_detail_v1.bin";

        struct Header {
            char magic[4] = { 'C','L','D','1' };
            int w, h, d;
        } expected_header{ {'C','L','D','1'}, w, h, d };

        std::ifstream in(cache_file, std::ios::binary);
        if (in.is_open()) {
            Header read_h;
            in.read(reinterpret_cast<char*>(&read_h), sizeof(Header));
            if (read_h.w == w && read_h.h == h && read_h.d == d &&
                read_h.magic[0] == 'C' && read_h.magic[3] == '1') {
                std::vector<uint8_t> data(w * h * d);
                in.read(reinterpret_cast<char*>(data.data()), data.size());
                if (in) {
                    std::cout << "[NUVENS_DET] Cache carregado: " << cache_file << "\n";
                    return data;
                }
            }
        }

        std::cout << "[NUVENS_DET] Gerando detalhe 3D (" << w << "x" << h << "x" << d << ")...\n";
        auto start = std::chrono::high_resolution_clock::now();
        std::vector<uint8_t> data(w * h * d);

        int c1 = 4, c2 = 8, c3 = 16;
        for (int z = 0; z < d; ++z) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    float fx = (float)x / w; float fy = (float)y / h; float fz = (float)z / d;

                    float w1 = worley_tileable(fx * c1, fy * c1, fz * c1, c1, c1, c1);
                    float w2 = worley_tileable(fx * c2, fy * c2, fz * c2, c2, c2, c2);
                    float w3 = worley_tileable(fx * c3, fy * c3, fz * c3, c3, c3, c3);

                    float val = (w1 * 0.625f) + (w2 * 0.25f) + (w3 * 0.125f);
                    val = 1.0f - val; // Inverte o Worley = Swiss Cheese
                    val = std::clamp(val, 0.0f, 1.0f);

                    data[x + w * (y + h * z)] = (uint8_t)(val * 255.0f);
                }
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::cout << "[NUVENS_DET] Concluido em " << std::chrono::duration<double>(end - start).count() << "s\n";

        std::filesystem::create_directories(cache_dir);
        std::ofstream out(cache_file, std::ios::binary);
        if (out.is_open()) {
            out.write(reinterpret_cast<const char*>(&expected_header), sizeof(Header));
            out.write(reinterpret_cast<const char*>(data.data()), data.size());
        }

        return data;
    }
}