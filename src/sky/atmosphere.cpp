// ============================================================================
// M2.5-25-2 (S1+S2+S3): assadores das LUTs da atmosfera fisica.
// Fonte: Hillaire, EGSR 2020 (ver atmosphere.h). Unidades internas: km.
// Tudo aqui e FISICA COM FONTE = CODIGO BLINDADO.
// ============================================================================

#include "atmosphere.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace atmo {
namespace {

// ---------------------------------------------------------------- vetor local
// (self-contained de proposito: o modulo nao depende de glm)
struct V3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    V3() = default;
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
    explicit V3(float a) : x(a), y(a), z(a) {}
};
inline V3 operator+(const V3& a, const V3& b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(const V3& a, const V3& b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(const V3& a, const V3& b) { return V3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline V3 operator*(const V3& a, float s) { return V3(a.x * s, a.y * s, a.z * s); }
inline V3 operator/(const V3& a, const V3& b) { return V3(a.x / b.x, a.y / b.y, a.z / b.z); }
inline V3& operator+=(V3& a, const V3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline float dot3(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len3(const V3& a) { return std::sqrt(dot3(a, a)); }
inline V3 norm3(const V3& a) { float l = len3(a); return (l > 1e-9f) ? a * (1.0f / l) : V3(0, 1, 0); }
inline V3 expn(const V3& a) { return V3(std::exp(-a.x), std::exp(-a.y), std::exp(-a.z)); }

// ------------------------------------------- Tabela 1 + geometria (Hillaire §4)
constexpr float kPi = 3.14159265358979f;
constexpr float kRg = 6360.0f; // raio do chao (km)
constexpr float kRt = 6460.0f; // topo da atmosfera (km) - camada de 100 km
const V3  kRayS(5.802e-3f, 13.558e-3f, 33.1e-3f); // Rayleigh scattering (1/km)
constexpr float kMieS = 3.996e-3f;                // Mie scattering (1/km)
constexpr float kMieA = 4.40e-3f;                 // Mie absorption (1/km)
const V3  kOzoA(0.650e-3f, 1.881e-3f, 0.085e-3f); // Ozonio: SO absorve (1/km)
constexpr float kGroundAlbedo = 0.3f;             // §4, NASA Earth Fact Sheet

inline void densities(float h, float& dr, float& dm, float& dz)
{
    dr = std::exp(-h / 8.0f);                                  // Rayleigh: e^(-h/8km)
    dm = std::exp(-h / 1.2f);                                  // Mie: e^(-h/1.2km)
    dz = std::max(0.0f, 1.0f - std::fabs(h - 25.0f) / 15.0f);  // Ozonio: tenda 30km @ 25km
}

inline V3 sigma_t(float h)
{
    float dr, dm, dz; densities(h, dr, dm, dz);
    return kRayS * dr + V3(kMieS + kMieA) * dm + kOzoA * dz;
}

inline V3 sigma_s(float h) // so espalha (ozonio nao entra)
{
    float dr, dm, dz; densities(h, dr, dm, dz);
    return kRayS * dr + V3(kMieS) * dm;
}

// Menor t>0 da intersecao com a esfera de raio r. -1 = nao intersecta a frente.
inline float ray_sphere(const V3& p, const V3& d, float r)
{
    float b = dot3(p, d);
    float c = dot3(p, p) - r * r;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    disc = std::sqrt(disc);
    float t0 = -b - disc, t1 = -b + disc;
    if (t1 < 0.0f) return -1.0f;
    return (t0 < 0.0f) ? t1 : t0;
}

// ------------------------------------------------------ parametrizacao das LUTs
// Transmittance (nossa, consistente entre assador e shader; compressao junto ao
// horizonte, onde a transmitancia varia rapido):
//   u = 0.5 + 0.5*sign(mu)*sqrt(|mu|)   ->   mu = sign(2u-1) * (2u-1)^2
//   v = h / (Rt - Rg)
// Multi-scattering (Hillaire §5.5.2, verbatim):
//   u = 0.5 + 0.5*cos(theta_s)          ;   v = h / (Rt - Rg)

std::vector<V3> g_trans; // kTransW*kTransH, precisao float (fonte do MS LUT)

V3 transmittance_march(float h, float mu)
{
    V3 p(0.0f, kRg + h, 0.0f);
    V3 d(std::sqrt(std::max(0.0f, 1.0f - mu * mu)), mu, 0.0f);

    // Chao bloqueia?
    float b = dot3(p, d);
    float c = dot3(p, p) - kRg * kRg;
    float disc = b * b - c;
    if (disc >= 0.0f && (-b - std::sqrt(disc)) > 0.0f) return V3(0.0f);

    float t_top = ray_sphere(p, d, kRt);
    if (t_top <= 0.0f) return V3(1.0f);

    const int kSteps = 40; // §5.2 / Tabela 2
    float dt = t_top / (float)kSteps;
    V3 od(0.0f);
    for (int i = 0; i < kSteps; ++i)
    {
        V3 x = p + d * ((i + 0.5f) * dt);
        od += sigma_t(len3(x) - kRg) * dt;
    }
    return expn(od);
}

void build_trans_table()
{
    if (!g_trans.empty()) return;
    g_trans.resize((size_t)kTransW * kTransH);
    for (int j = 0; j < kTransH; ++j)
    {
        float v = (j + 0.5f) / (float)kTransH;
        float h = v * (kRt - kRg);
        for (int i = 0; i < kTransW; ++i)
        {
            float u = (i + 0.5f) / (float)kTransW;
            float s = 2.0f * u - 1.0f;
            float mu = (s < 0.0f ? -1.0f : 1.0f) * s * s;
            g_trans[(size_t)j * kTransW + i] = transmittance_march(h, mu);
        }
    }
}

// Amostra bilinear da tabela (clamp), com a MESMA parametrizacao do shader.
V3 sample_trans(float h, float mu)
{
    build_trans_table();
    float s = (mu < 0.0f ? -1.0f : 1.0f) * std::sqrt(std::fabs(mu));
    float u = std::clamp(0.5f + 0.5f * s, 0.0f, 1.0f);
    float v = std::clamp(h / (kRt - kRg), 0.0f, 1.0f);

    float fx = u * kTransW - 0.5f, fy = v * kTransH - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float tx = fx - x0, ty = fy - y0;
    int x1 = std::clamp(x0 + 1, 0, kTransW - 1), y1 = std::clamp(y0 + 1, 0, kTransH - 1);
    x0 = std::clamp(x0, 0, kTransW - 1); y0 = std::clamp(y0, 0, kTransH - 1);

    const V3& a = g_trans[(size_t)y0 * kTransW + x0];
    const V3& b = g_trans[(size_t)y0 * kTransW + x1];
    const V3& c = g_trans[(size_t)y1 * kTransW + x0];
    const V3& d = g_trans[(size_t)y1 * kTransW + x1];
    V3 top = a * (1.0f - tx) + b * tx;
    V3 bot = c * (1.0f - tx) + d * tx;
    return top * (1.0f - ty) + bot * ty;
}

// ----------------------------------------------------- Psi_ms (Hillaire §5.5.3)
V3 multiscatter_at(float h, float sun_cos)
{
    V3 pos(0.0f, kRg + h, 0.0f);
    V3 sun(std::sqrt(std::max(0.0f, 1.0f - sun_cos * sun_cos)), sun_cos, 0.0f);

    const int kDirs = 64;   // §5.5.3: 64 direcoes uniformes
    const int kSteps = 20;  // Tabela 2
    const float pu = 1.0f / (4.0f * kPi); // fase isotropica (§4)

    V3 L2(0.0f), fms(0.0f);
    for (int k = 0; k < kDirs; ++k)
    {
        // esfera de Fibonacci
        float kk = k + 0.5f;
        float cphi = 1.0f - 2.0f * kk / (float)kDirs;
        float sphi = std::sqrt(std::max(0.0f, 1.0f - cphi * cphi));
        float theta = kPi * (1.0f + std::sqrt(5.0f)) * kk;
        V3 dir(std::cos(theta) * sphi, cphi, std::sin(theta) * sphi);

        float t_max = ray_sphere(pos, dir, kRt);
        if (t_max <= 0.0f) continue;
        bool hit_ground = false;
        {
            float b = dot3(pos, dir);
            float c = dot3(pos, pos) - kRg * kRg;
            float disc = b * b - c;
            if (disc >= 0.0f)
            {
                float tg = -b - std::sqrt(disc);
                if (tg > 0.0f) { t_max = tg; hit_ground = true; }
            }
        }

        float dt = t_max / (float)kSteps;
        V3 Tv(1.0f);
        for (int n = 0; n < kSteps; ++n)
        {
            V3 x = pos + dir * ((n + 0.5f) * dt);
            float r = len3(x);
            float hh = r - kRg;
            V3 ss = sigma_s(hh);
            V3 st = sigma_t(hh);
            V3 Tsun = sample_trans(hh, dot3(x * (1.0f / r), sun));

            // Eq. 6: contribuicao de 2a ordem, fase isotropica, E_I = 1
            V3 S = ss * Tsun * pu;
            V3 e = expn(st * dt);
            V3 Sint = (S - S * e) / V3(std::max(st.x, 1e-9f), std::max(st.y, 1e-9f), std::max(st.z, 1e-9f));
            L2 += Tv * Sint;

            // Eq. 8: fator de transferencia (sem sombra, sem fase)
            V3 Fint = (ss - ss * e) / V3(std::max(st.x, 1e-9f), std::max(st.y, 1e-9f), std::max(st.z, 1e-9f));
            fms += Tv * Fint;

            Tv = Tv * e;
        }

        if (hit_ground) // Lo: resposta difusa do chao (§4, albedo 0.3)
        {
            V3 pg = pos + dir * t_max;
            V3 ng = norm3(pg);
            float ns = std::max(0.0f, dot3(ng, sun));
            V3 Tg = sample_trans(0.0f, dot3(ng, sun));
            L2 += Tv * Tg * (kGroundAlbedo / kPi) * ns;
        }
    }

    L2 = L2 * (1.0f / (float)kDirs);
    fms = fms * (1.0f / (float)kDirs);

    // Eq. 9-10: soma geometrica infinita das ordens seguintes
    V3 Fms(1.0f / (1.0f - std::min(fms.x, 0.999f)),
           1.0f / (1.0f - std::min(fms.y, 0.999f)),
           1.0f / (1.0f - std::min(fms.z, 0.999f)));
    return L2 * Fms;
}

// ------------------------------------------------------------- half + arquivo
inline uint16_t f2h(float f)
{
    uint32_t x; std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t man = x & 0x7FFFFFu;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (man >> 13));
}

void push_rgba16f(std::vector<uint8_t>& out, const V3& c)
{
    uint16_t h[4] = { f2h(c.x), f2h(c.y), f2h(c.z), f2h(1.0f) };
    const uint8_t* p = reinterpret_cast<const uint8_t*>(h);
    out.insert(out.end(), p, p + 8);
}

const char* kCachePath = "cache/atmo_luts_v1.bin";
const char kMagic[4] = { 'A', 'T', 'M', '1' };

std::vector<V3> g_ms; // kMsW*kMsH

bool cache_load()
{
    std::ifstream f(kCachePath, std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0) return false;
    int32_t dims[4] = {};
    f.read(reinterpret_cast<char*>(dims), sizeof(dims));
    if (dims[0] != kTransW || dims[1] != kTransH || dims[2] != kMsW || dims[3] != kMsH) return false;
    g_trans.resize((size_t)kTransW * kTransH);
    g_ms.resize((size_t)kMsW * kMsH);
    f.read(reinterpret_cast<char*>(g_trans.data()), (std::streamsize)(g_trans.size() * sizeof(V3)));
    f.read(reinterpret_cast<char*>(g_ms.data()), (std::streamsize)(g_ms.size() * sizeof(V3)));
    if (!f) { g_trans.clear(); g_ms.clear(); return false; }
    return true;
}

void cache_save()
{
    std::error_code ec;
    std::filesystem::create_directories("cache", ec);
    std::ofstream f(kCachePath, std::ios::binary);
    if (!f) return;
    int32_t dims[4] = { kTransW, kTransH, kMsW, kMsH };
    f.write(kMagic, 4);
    f.write(reinterpret_cast<const char*>(dims), sizeof(dims));
    f.write(reinterpret_cast<const char*>(g_trans.data()), (std::streamsize)(g_trans.size() * sizeof(V3)));
    f.write(reinterpret_cast<const char*>(g_ms.data()), (std::streamsize)(g_ms.size() * sizeof(V3)));
}

void ensure_baked()
{
    if (!g_trans.empty() && !g_ms.empty()) return;
    if (cache_load()) return;

    build_trans_table();
    g_ms.resize((size_t)kMsW * kMsH);
    for (int j = 0; j < kMsH; ++j)
    {
        float v = (j + 0.5f) / (float)kMsH;
        float h = v * (kRt - kRg);
        for (int i = 0; i < kMsW; ++i)
        {
            float u = (i + 0.5f) / (float)kMsW;
            g_ms[(size_t)j * kMsW + i] = multiscatter_at(h, 2.0f * u - 1.0f); // §5.5.2
        }
    }
    cache_save();
}

// -------------------------------------------------- M2.5-25-3 (S10): ambiente
// Versao CPU da marcha do ceu (a MESMA fisica do sky.hlsl), usada para calcular
// a luminancia media do hemisferio que ilumina as nuvens.

V3 sample_ms(float h, float mu_s)
{
    float u = std::clamp(0.5f + 0.5f * mu_s, 0.0f, 1.0f);
    float v = std::clamp(h / (kRt - kRg), 0.0f, 1.0f);

    float fx = u * kMsW - 0.5f, fy = v * kMsH - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float tx = fx - x0, ty = fy - y0;
    int x1 = std::clamp(x0 + 1, 0, kMsW - 1), y1 = std::clamp(y0 + 1, 0, kMsH - 1);
    x0 = std::clamp(x0, 0, kMsW - 1); y0 = std::clamp(y0, 0, kMsH - 1);

    V3 top = g_ms[(size_t)y0 * kMsW + x0] * (1.0f - tx) + g_ms[(size_t)y0 * kMsW + x1] * tx;
    V3 bot = g_ms[(size_t)y1 * kMsW + x0] * (1.0f - tx) + g_ms[(size_t)y1 * kMsW + x1] * tx;
    return top * (1.0f - ty) + bot * ty;
}

V3 sky_radiance(const V3& dir, const V3& sun)
{
    V3 p0(0.0f, kRg + 0.2f, 0.0f);
    float t_top = ray_sphere(p0, dir, kRt);
    if (t_top <= 0.0f) return V3(0.0f);

    float t_max = t_top;
    {
        float b = dot3(p0, dir);
        float c = dot3(p0, p0) - kRg * kRg;
        float disc = b * b - c;
        if (disc >= 0.0f)
        {
            float tg = -b - std::sqrt(disc);
            if (tg > 0.0f) t_max = tg;
        }
    }

    float ct = dot3(dir, sun);
    float pr = 3.0f * (1.0f + ct * ct) / (16.0f * kPi);
    const float g = 0.8f;
    float pm = (3.0f / (8.0f * kPi)) * ((1.0f - g * g) * (1.0f + ct * ct)) /
               ((2.0f + g * g) * std::pow(std::max(1.0f + g * g - 2.0f * g * ct, 1e-4f), 1.5f));

    const int N = 20;
    float dt = t_max / (float)N;
    V3 L(0.0f), T(1.0f);
    for (int i = 0; i < N; ++i)
    {
        V3 x = p0 + dir * ((i + 0.5f) * dt);
        float r = len3(x);
        float h = r - kRg;
        float dr_, dm_, dz_; densities(h, dr_, dm_, dz_);
        V3 ss_r = kRayS * dr_;
        V3 ss_m = V3(kMieS) * dm_;
        V3 st = sigma_t(h);

        float mu_s = dot3(x * (1.0f / r), sun);
        V3 Tsun = sample_trans(h, mu_s);
        V3 psi = sample_ms(h, mu_s);

        V3 S = (ss_r * pr + ss_m * pm) * Tsun + (ss_r + ss_m) * psi;
        V3 e = expn(st * dt);
        V3 Sint = (S - S * e) / V3(std::max(st.x, 1e-7f), std::max(st.y, 1e-7f), std::max(st.z, 1e-7f));
        L += T * Sint;
        T = T * e;
    }
    return L;
}

} // namespace

std::vector<uint8_t> generate_transmittance_lut()
{
    ensure_baked();
    std::vector<uint8_t> out;
    out.reserve((size_t)kTransW * kTransH * 8);
    for (const V3& c : g_trans) push_rgba16f(out, c);
    return out;
}

std::vector<uint8_t> generate_multiscatter_lut()
{
    ensure_baked();
    std::vector<uint8_t> out;
    out.reserve((size_t)kMsW * kMsH * 8);
    for (const V3& c : g_ms) push_rgba16f(out, c);
    return out;
}

void sky_ambient(float sun_cos_zenith, float out_rgb[3])
{
    ensure_baked();

    float sc = std::clamp(sun_cos_zenith, -1.0f, 1.0f);
    V3 sun(std::sqrt(std::max(0.0f, 1.0f - sc * sc)), sc, 0.0f);

    const int kDirs = 32; // hemisferio superior, distribuicao de Fibonacci
    V3 acc(0.0f);
    for (int k = 0; k < kDirs; ++k)
    {
        float kk = k + 0.5f;
        float cy = kk / (float)kDirs; // 0..1 = horizonte..zenite
        float sy = std::sqrt(std::max(0.0f, 1.0f - cy * cy));
        float th = kPi * (1.0f + std::sqrt(5.0f)) * kk;
        acc += sky_radiance(V3(std::cos(th) * sy, cy, std::sin(th) * sy), sun);
    }
    acc = acc * (1.0f / (float)kDirs);

    out_rgb[0] = acc.x;
    out_rgb[1] = acc.y;
    out_rgb[2] = acc.z;
}

void sky_horizon(float sun_cos_zenith, float out_rgb[3])
{
    ensure_baked();

    float sc = std::clamp(sun_cos_zenith, -1.0f, 1.0f);
    V3 sun(std::sqrt(std::max(0.0f, 1.0f - sc * sc)), sc, 0.0f);

    const int kAz = 8;
    const float el = 0.07f;                 // ~4 graus de elevacao
    const float hr = std::sqrt(std::max(0.0f, 1.0f - el * el));
    V3 acc(0.0f);
    for (int k = 0; k < kAz; ++k)
    {
        float a = (k + 0.5f) * 2.0f * kPi / (float)kAz;
        acc += sky_radiance(V3(std::cos(a) * hr, el, std::sin(a) * hr), sun);
    }
    acc = acc * (1.0f / (float)kAz);

    out_rgb[0] = acc.x;
    out_rgb[1] = acc.y;
    out_rgb[2] = acc.z;
}

} // namespace atmo
