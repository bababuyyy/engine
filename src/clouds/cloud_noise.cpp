// cloud_noise.cpp - M2.5-19A-fix: Perlin-Worley canonico (receita Nubis/Frostbite)
// PCG3D encadeado (Jarzynski-Olano) + Perlin periodico de gradientes diagonais
// + Worley periodico invertido como EROSAO (remap de dilatacao) + cache v2
// + fatias PGM de diagnostico (o parafuso do Passo 0).
#include "cloud_noise.h"
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <chrono>

namespace clouds {

    bool CLOUD_DIAGNOSTIC_VOLUME = false;

    // ---------- PCG3D (mixing encadeado; nunca pcg(x*A+y*B+z*C)) ----------
    struct U3 { uint32_t x, y, z; };
    static inline U3 pcg3d(U3 v) {
        v.x = v.x * 1664525u + 1013904223u;
        v.y = v.y * 1664525u + 1013904223u;
        v.z = v.z * 1664525u + 1013904223u;
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        v.x ^= v.x >> 16; v.y ^= v.y >> 16; v.z ^= v.z >> 16;
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        return v;
    }
    static inline float u2f(uint32_t u) { return (float)u / 4294967296.0f; }

    // ---------- Perlin periodico (gradientes diagonais, fade quintico) ----------
    // 12 gradientes diagonais do Improved Perlin
    static const float GRAD[12][3] = {
        {1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},
        {1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},
        {0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1}
    };
    static inline float fade_q(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
    static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

    static float perlin_periodic(float px, float py, float pz, int period) {
        int X0 = (int)std::floor(px), Y0 = (int)std::floor(py), Z0 = (int)std::floor(pz);
        float fx = px - X0, fy = py - Y0, fz = pz - Z0;
        float u = fade_q(fx), v = fade_q(fy), w = fade_q(fz);
        float corner[2][2][2];
        for (int ci = 0; ci < 2; ++ci)
        for (int cj = 0; cj < 2; ++cj)
        for (int ck = 0; ck < 2; ++ck) {
            // wrap SO na coordenada que vai ao hash (o ponto continuo fica intacto)
            int xi = ((X0 + ci) % period + period) % period;
            int yj = ((Y0 + cj) % period + period) % period;
            int zk = ((Z0 + ck) % period + period) % period;
            U3 h = pcg3d({ (uint32_t)xi, (uint32_t)yj, (uint32_t)zk });
            const float* g = GRAD[h.x % 12u];
            float dx = fx - ci, dy = fy - cj, dz = fz - ck;
            corner[ci][cj][ck] = g[0] * dx + g[1] * dy + g[2] * dz;
        }
        float x00 = lerpf(corner[0][0][0], corner[1][0][0], u);
        float x10 = lerpf(corner[0][1][0], corner[1][1][0], u);
        float x01 = lerpf(corner[0][0][1], corner[1][0][1], u);
        float x11 = lerpf(corner[0][1][1], corner[1][1][1], u);
        float y0 = lerpf(x00, x10, v);
        float y1 = lerpf(x01, x11, v);
        return lerpf(y0, y1, w); // ~[-0.7, 0.7]
    }

    // FBM de Perlin: 4 octavas, freq base 4, periodo dobra por octava
    static float perlin_fbm(float nx, float ny, float nz) {
        float sum = 0.0f, amp = 1.0f, norm = 0.0f;
        int freq = 4;
        for (int o = 0; o < 4; ++o) {
            sum += amp * perlin_periodic(nx * freq, ny * freq, nz * freq, freq);
            norm += amp;
            amp *= 0.5f;
            freq *= 2; // o periodo acompanha a frequencia (senao o loop aparece)
        }
        return sum / norm * 0.5f + 0.5f; // -> [0,1] aprox
    }

    // ---------- Worley periodico invertido (celulas embrulhadas) ----------
    static float worley_inv(float nx, float ny, float nz, int cells) {
        float px = nx * cells, py = ny * cells, pz = nz * cells;
        int X0 = (int)std::floor(px), Y0 = (int)std::floor(py), Z0 = (int)std::floor(pz);
        float min_d2 = 1e9f;
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int cx = X0 + dx, cy = Y0 + dy, cz = Z0 + dz;
            // indice embrulhado vai ao hash; a POSICAO geometrica fica intacta
            int wx = ((cx % cells) + cells) % cells;
            int wy = ((cy % cells) + cells) % cells;
            int wz = ((cz % cells) + cells) % cells;
            U3 h = pcg3d({ (uint32_t)wx, (uint32_t)wy, (uint32_t)wz });
            float fx = cx + u2f(h.x), fy = cy + u2f(h.y), fz = cz + u2f(h.z);
            float ddx = px - fx, ddy = py - fy, ddz = pz - fz;
            float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
            if (d2 < min_d2) min_d2 = d2;
        }
        float d = std::sqrt(min_d2); // [0, ~1] em espaco de celula
        return 1.0f - (d < 1.0f ? d : 1.0f); // invertido: 1 no centro
    }

    // FBM de Worley: pesos 0.625/0.25/0.125 em 4/8/16 celulas
    static float worley_fbm(float nx, float ny, float nz) {
        return worley_inv(nx, ny, nz, 4) * 0.625f
             + worley_inv(nx, ny, nz, 8) * 0.25f
             + worley_inv(nx, ny, nz, 16) * 0.125f;
    }

    static inline float remapf(float v, float lo, float hi, float nlo, float nhi) {
        float t = (v - lo) / (hi - lo);
        return nlo + t * (nhi - nlo);
    }

    static void dump_pgm(const char* path, const std::vector<uint8_t>& img, int w, int h) {
        std::ofstream f(path, std::ios::binary);
        f << "P5\n" << w << " " << h << "\n255\n";
        f.write((const char*)img.data(), img.size());
    }

    std::vector<uint8_t> generate_cloud_noise(int w, int h, int d) {
        std::vector<uint8_t> data((size_t)w * h * d);
        std::string cache_dir = "cache";
        std::string cache_file = cache_dir + "/cloud_noise_v2.bin";

        if (!CLOUD_DIAGNOSTIC_VOLUME) {
            std::ifstream in(cache_file, std::ios::binary);
            if (in) {
                int cw = 0, ch = 0, cd = 0;
                in.read((char*)&cw, 4); in.read((char*)&ch, 4); in.read((char*)&cd, 4);
                if (cw == w && ch == h && cd == d) {
                    in.read((char*)data.data(), data.size());
                    if (in) {
                        std::cout << "[NUVENS] Cache v2 carregado de: " << cache_file << "\n";
                        return data;
                    }
                }
            }
        }

        auto t0 = std::chrono::steady_clock::now();
        for (int z = 0; z < d; ++z) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    float val;
                    if (CLOUD_DIAGNOSTIC_VOLUME) {
                        float cx = x / (float)w - 0.5f, cy = y / (float)h - 0.5f, cz = z / (float)d - 0.5f;
                        val = (std::sqrt(cx * cx + cy * cy + cz * cz) < 0.35f) ? 1.0f : 0.0f;
                    } else {
                        float nx = x / (float)w, ny = y / (float)h, nz = z / (float)d;
                        float P = perlin_fbm(nx, ny, nz);       // a MASSA (conectividade)
                        float W = worley_fbm(nx, ny, nz);       // o TEMPERO (billow nas bordas)
                        val = remapf(P, 0.0f, 1.0f, W, 1.0f);   // dilatacao canonica
                        val = val < 0.0f ? 0.0f : (val > 1.0f ? 1.0f : val);
                    }
                    data[(size_t)z * w * h + (size_t)y * w + x] = (uint8_t)(val * 255.0f);
                }
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "[NUVENS] Volume " << w << "x" << h << "x" << d << " gerado em " << secs << "s\n";

        if (!CLOUD_DIAGNOSTIC_VOLUME) {
            std::filesystem::create_directories(cache_dir);
            std::ofstream out(cache_file, std::ios::binary);
            out.write((const char*)&w, 4); out.write((const char*)&h, 4); out.write((const char*)&d, 4);
            out.write((const char*)data.data(), data.size());
            std::cout << "[NUVENS] Cache salvo: " << cache_file << "\n";

            // Passo 0: fatias de diagnostico (abra no GIMP/paint.net/IrfanView)
            std::vector<uint8_t> sl_xz((size_t)w * d);
            int ym = h / 2;
            for (int z = 0; z < d; ++z) for (int x = 0; x < w; ++x)
                sl_xz[(size_t)z * w + x] = data[(size_t)z * w * h + (size_t)ym * w + x];
            dump_pgm((cache_dir + "/noise_slice_xz.pgm").c_str(), sl_xz, w, d);
            std::vector<uint8_t> sl_xy((size_t)w * h);
            int zm = d / 2;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                sl_xy[(size_t)y * w + x] = data[(size_t)zm * w * h + (size_t)y * w + x];
            dump_pgm((cache_dir + "/noise_slice_xy.pgm").c_str(), sl_xy, w, h);
            std::cout << "[NUVENS] Fatias de diagnostico em cache/noise_slice_*.pgm\n";
        }
        return data;
    }
}
