#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdint>
#include <cstring> // M2.5-26-M: memcpy do half->float
#include <cstdio>
#include <vector>
#include <string>
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>

#include "core/jobs.h"
#include "sim/sim.h"
#include "phys/physics.h"
#include "level/level.h"
#include "gen/gen.h"
#include "assets/assets.h"
#include "social/social.h"

#ifndef SOCIAL_DIR
#define SOCIAL_DIR "data/social/"
#endif

typedef unsigned char BYTE;
#include "basic_vs.h"
#include "basic_ps.h"
#include "fullscreen_vs.h"
#include "fullscreen_ps.h"
#include "outline_vs.h"
#include "outline_ps.h"
#include "sky_vs.h"
#include "sky_ps.h"
#include "grass_vs.h"
#include "grass_ps.h"
#include "bloom_vs.h"
#include "bloom_thresh_ps.h"
#include "bloom_down_ps.h"
#include "bloom_up_ps.h"
#include "expo_vs.h"        // M2.5-26-4
#include "expo_luma_ps.h"   // M2.5-26-4
#include "expo_adapt_ps.h"  // M2.5-26-4
#include "shadow_vs.h"
#include "shadow_ps.h"
#include "lt_down_ps.h"    // M2.5-26-14b
#include "lt_blur_ps.h"    // M2.5-26-14b
#include "lt_bilat_ps.h"   // M2.5-26-16
#include "lt_grid_ps.h"    // M2.5-26-23a: a grade bilateral
#include "shaft_vs.h"       // M2.5-26-25: a marcha volumetrica, agora passe proprio
#include "shaft_ps.h"

#include "clouds/cloud_noise.h"
#include "sky/atmosphere.h" // M2.5-25-2: LUTs da atmosfera (Hillaire 2020)

// ============================================================================

// ============================================================================
// SISTEMA DE FEEL / MAQUINA DE ESTADO DO JOGADOR
// ============================================================================
namespace feel {
    float jump_height = 2.2f;
    int ticks_to_apex = 22;
    float fall_gravity_mult = 1.8f;
    float apex_gravity_mult = 0.5f;
    int coyote_ticks = 6;
    int buffer_ticks = 5;
    float ground_accel = 60.0f;
    float ground_decel = 80.0f;
    float air_accel_mult = 0.4f;
    float max_speed = 6.0f;
    float dash_speed = 14.0f;
    int dash_ticks = 10;
    int dash_cooldown = 30;

    float t_apex_s = 0.0f;
    float g = 0.0f;
    float v0 = 0.0f;

    void update_derived() {
        t_apex_s = ticks_to_apex / 60.0f;
        g = 2.0f * jump_height / (t_apex_s * t_apex_s);
        v0 = 2.0f * jump_height / t_apex_s;
    }
}

glm::vec2 move_towards(glm::vec2 current, glm::vec2 target, float max_delta) {
    glm::vec2 diff = target - current;
    float len = glm::length(diff);
    if (len <= max_delta) return target;
    return current + (diff / len) * max_delta;
}

struct AppLog {
    std::vector<std::string> lines;
    bool auto_scroll = true;

    void add(const char* fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        lines.push_back(buf);
        if (lines.size() > 256) lines.erase(lines.begin());
        auto_scroll = true;
    }
};

struct Vertex { float x, y, z; float r, g, b; float u, v; float nx, ny, nz; };
struct InstanceData { float x, y, z; float r, g, b; };

struct MeshRange {
    uint32_t first_index;
    uint32_t index_count;
};

struct SunParams {
    float dir[3];            float ambient;
    float color[3];          float toon_cuts;
    float cam_pos[3];        float toon_soft;
    float wind_dir[2];       float fog_density; float cloud_time;
    float fog_color[3];      float cloud_cover;
    float shadow_str;        float grass_backlight; float grass_specular; float obj_shadow_str;
    float obj_shadow_bias;   float debug_shadows;   float pad_0[2];
    // M2.5-26-7: O CEU VIRA FONTE DE LUZ DA CENA.
    // Frostbite (Lagarde & de Rousiers, SIGGRAPH 2014), Tabela 8: o Sol usa
    // ILLUMINANCE (lx) e o Ceu usa LUMINANCE (cd/m2) - sao fontes SEPARADAS, com
    // unidades diferentes. E, verbatim: "the lighting from the sky is handled with
    // a distant light probe". Nos tinhamos so o sol; o 'ambient' era uma FRACAO
    // dele fazendo as vezes do ceu.
    // Medido (sonda 26-7, sol a -0,55 graus = 5h56): o ceu entrega 1.091 lx a cena,
    // o que poe o chao em ~347 cd/m2. O 'ambient' entregava ~20. Erro de 17x, e por
    // isso o amanhecer tinha 10,6 stops de amplitude: nao era o ceu brilhante
    // demais, era o CHAO sem a luz que o ilumina.
    // sky_light = a MESMA sky_ambient_color que ja alimenta as nuvens desde a 25-3
    // (media do hemisferio pela marcha do Hillaire). Ja estava calculada.
    float sky_light[3];      float sky_light_str; // 26-7 (str: 1.0 = fisico)
    float light_vp[16];
    // M2.5-26-8: perspectiva aerea da cena - as MESMAS grandezas que o ceu usa
    // na marcha, para que os dois nao possam divergir.
    float ap_to_sun[3];     float ap_steps;
    float ap_sun_illum[3];  float ap_str;
    float ap_to_moon[3];    float ap_on;
    float ap_moon_illum[3]; float ap_pad;
};

struct SkyParams {
    float to_sun[3];        float pad0;
    float sun_color[3];     float pad3;
    float cam_pos[3];       float pad4;
    float wind_dir[2];      float time;       float cloud_speed;
    float cloud_color[3];   float cobertura;
    float densidade;        float escala;     float h_min;      float h_max;
    float cap;              float passos;     float absorcao;   float hg_g_frontal;
    float powder_str;       float sun_intensity; float sun_ambient; float cloud_type;
    float erosao_str;       float cloud_silhouette; float cloud_bands; float cloud_ambient_str; // 24E-3: ocupa o antigo pad_a
    float sky_ambient_color[3];               float detail_str;
    float cloud_shadow_color[3];              float curl_str;
    float silver_k;         float silver_spread;    float clima_cov_teto;   float clima_type_teto; // 24G-3: tetos do clima (ex-pads)
    float curvatura;        float fade_ini;         float passo_far; float cloud_ms_str; // 24G-6 + 24J-3
    float cloud_ms_g2;      float cloud_iso_str; float sky_gain;  float atmo_debug; // 24J-3 + 24J-7 + 25-2
    float sun_illum[3];     float night_floor; // 25-2 + 25-5a
    float sun_disk_deg;     float sun_disk_bright; float stars_str; float stars_mag_lim; // 25-7 + 25-18 (os dois pads viram as estrelas)
    float night_tint[3];    float pad_n; // 25-8: cor do piso noturno = ARTE
    float to_moon[3];       float moon_disk_deg;
    float moon_disk_bright; float stars_size; float stars_dens; float stars_slope; // 25-9a + 25-18 (os tres pads viram as estrelas)
    float to_astro[3];      float pad_p;
    float astro_sky_col[3]; float aerial_str; // 25-12 + 25-13
    float moon_illum[3];    float pad_r; // 25-15: a lua na marcha atmosferica (352B)
};

struct ObjIdParams {
    float id;
    float pad[3];
};

struct FsParams { // M2.5-24H: shafts volumetricos
    float inv_viewproj[16];
    float light_vp[16];
    float cam_pos[3];    float shaft_intensity;
    float to_sun[3];     float shaft_g;
    float sun_color[3];  float shaft_samples;
    float tex_size[2];   float wind_off[2];
    float shaft_sigma;   float cloud_cover;   float shadow_str_fs; float bloom_intensity;
    float tonemap_shoulder; float tonemap_modo; float shaft_cloud_occ; float cloud_mid; // 26-11: exposure estava MORTO aqui desde a 26-2a (a pre-exposicao passou a vir da textura 1x1); o campo vira o seletor de curva, sem mudar um byte de layout
    float shaft_debug;      float shaft_height_falloff; float dither_amt; float expo_auto; // 25-U6d + 26-4 ocupam os dois pad_j
    // M2.5-26-14: operador local (Patry 2021, slide 137)
    float lt_on;            float lt_c; float lt_d; float lt_m;
    // M2.5-26-15: contraste do lado BAIXO, separado (slide 145)
    // M2.5-26-23c: os tres pads viram o eixo Z da grade, e o bloco cresce 4.
    float lt_c_lo;          float bg_sr; float bg_lo; float bg_hi;
    // M2.5-26-23c: +4 floats (68 -> 72 = 288 B, multiplo de 16) para o peso da
    // mistura da grade. O static_assert abaixo continua guardando o alinhamento.
    float bg_mix;           float bg_pad0; float bg_pad1; float bg_pad2;
    // M2.5-26-24: +12 floats (72 -> 84 = 336 B, multiplo de 16) para a matriz de
    // adaptacao cromatica. Ela comeca em indice 72, que e multiplo de 4, entao a
    // linha 0 pousa no componente 0 de um registrador - sem straddle.
    // NAO usar array no cbuffer do HLSL: la cada elemento de array ocupa um
    // registrador INTEIRO de 16 bytes. Do lado do HLSL sao nove escalares.
    float wb_m[9];          float wb_pad0; float wb_pad1; float wb_pad2;
};
static_assert(sizeof(FsParams) % 16 == 0, "FsParams: alinhamento de 16 bytes quebrado");

// ============================================================================
// M2.5-26-25: os 52 floats (208 B) que a marcha usa, e nada mais.
// Os NOMES sao identicos aos do FsParams de proposito: o corpo da marcha foi
// movido verbatim e nao sofreu uma renomeacao sequer. Renomear um campo seria
// criar um lugar onde a copia pode divergir do original em silencio.
// POR QUE UM BLOCO PROPRIO e nao o FsParams: aquele so e preenchido dezenas de
// linhas DEPOIS deste passe, e reordenar o preenchimento seria mexer em codigo
// que funciona por uma razao que nao e a desta estacao.
// ============================================================================
struct ShaftBlock {
    float inv_viewproj[16];
    float light_vp[16];
    float cam_pos[3];    float shaft_intensity;
    float to_sun[3];     float shaft_g;
    float sun_color[3];  float shaft_samples;
    float wind_off[2];   float shaft_sigma; float shaft_height_falloff;
    float shaft_cloud_occ; float cloud_mid; float cloud_cover; float shaft_pad0;
};
static_assert(sizeof(ShaftBlock) % 16 == 0, "ShaftBlock: alinhamento de 16 bytes quebrado");

struct BloomParams {
    float texel[2];
    float threshold;
    float knee;
    float cap;        // M2.5-26-11: teto do que ENTRA na piramide
    float pad_bl[3];
};

struct OutlineParams {
    float texel[2];
    float z_cutoff;
    float grazing_scale;
    float crease_low;
    float crease_high;
    float line_darken;
    float crease_brighten;
    float cam_fwd[3];
    float pad;
};

struct GrassVSParams {
    float mvp[16];
    float cam_pos[3];
    float scale;
    float wind_dir[2];
    float wind_strength;
    float time;
};

struct LoadedAsset {
    MeshRange range;
    SDL_GPUTexture* tex;
    bool is_terrain;
};

struct NpcVisual {
    MeshRange range;
    SDL_GPUTexture* tex;
    bool has_mesh;
};

namespace wind {
    float dir_deg = 45.0f;
    float strength = 0.5f;
}

float grass_clump_pull = 0.35f;
float grass_clump_cell = 2.5f;
float grass_density = 8.0f;
float grass_verdice_min = 0.05f;
float grass_slope_min = 0.7f;
float grass_border_sharp = 12.0f;
uint32_t grass_inst_count = 0;
SDL_GPUBuffer* grass_instbuf = nullptr;

std::vector<float> terrain_phys_verts;
std::vector<uint16_t> terrain_indices;

bool draw_sky = true;
float fog_density = 0.006f;

bool draw_clouds = true;
float cloud_cobertura = 0.45f;
float cloud_densidade = 3.0f;
float cloud_escala = 0.005f;
float cloud_speed = 1.0f;
int cloud_passos = 16;
float cloud_h_min = 120.0f;
float cloud_h_max = 220.0f;
float cloud_cap = 600.0f;
float cloud_absorcao = 1.0f;
// M2.5-26-13: g = 0.2, o valor da fonte. GPU Pro 7 cap. 4, sec. 4.4.3, p. 112,
// VERBATIM: "We use A SINGLE Henyey-Greenstein phase function with an eccentricity
// (directional component) g of 0.2, to make sure that more light in our clouds
// scatters forward". Era 0.8, sem fonte.
// E o "silver lining" NAO e um termo separado na fonte. HZD 2015, slide 53,
// VERBATIM: "It is the probability of light scattering forward or backward in the
// cloud. THIS IS RESPONSIBLE FOR THE SILVER LINING IN CLOUDS, one of our look
// goals." Ou seja: o silver lining E o espalhamento frontal da propria HG.
// O nosso segundo lobo e o max() do silver continuam no painel como ARTE declarada
// (cloud_silver_k ja nasce em 0 = fisica pura). Nubis 2022 e 2023 compoem os dois
// termos por SOMA, nunca por max - registrado como divergencia nossa, assumida.
float cloud_hg_g_frontal = 0.2f;
float cloud_powder_str = 0.5f;
float shadow_str = 0.55f;

float cloud_type_blend = 0.5f;
float cloud_erosao_str = 0.4f;
float cloud_silhouette = 0.18f;
float cloud_bands = 3.0f;
float cloud_detail_str = 0.35f; // M2.5-24C
float cloud_curl_str = 0.4f;
float cloud_ambient_str = 1.0f; // M2.5-24E-3: energia propria da nuvem
static_assert(sizeof(SkyParams) % 16 == 0, "SkyParams: alinhamento de 16 bytes quebrado");
float cloud_silver_k = 0.0f;      // M2.5-24E-4: silver lining (0 = fisica pura)
float cloud_ms_str = 1.0f;        // M2.5-24J-3: forca do brilho interno MS (retina; a fonte da a forma, nao o numero)
float cloud_ms_g2 = 0.2f;         // M2.5-24J-3: fase secundaria "mais isotropica" (retina; 0 = isotropica pura)
float cloud_iso_str = 0.12f;      // M2.5-24J-7: 3o termo de fase, isotropico (era o 0.12 fixo; 0.12 = comportamento anterior)
// M2.5-26-2b: LEGADO. Este ganho existia para costurar duas reguas: a atmosfera do
// Hillaire e fotometrica e o nosso sol era arbitrario. Com o sol em lux as duas
// reguas viraram uma so e ele nao tem mais funcao. Fica em 1.0 e sai do painel na
// proxima faxina; nao mexer.
float sky_gain = 1.0f;
int atmo_debug_mode = 0;          // M2.5-25-2: 0=off, 1=LUT de transmitancia, 2=LUT de multi-scattering
float cloud_silver_spread = 0.3f;
float clima_cov_teto = 0.85f;  // M2.5-24G-3: embriao da maquina de clima
float clima_type_teto = 0.80f;
float cloud_curvatura = 0.000025f; // M2.5-24G-6: horizonte honesto
float cloud_fade_ini = 0.45f;
float cloud_passo_far = 0.0f; // M2.5-24J-2b (A18): crescimento do passo no longe (0 = passo fixo; LIGAR e decisao de retina)

bool sol_automatico = true;
// ============================================================================
// M2.5-25-1 (S1+S2): transmitancia do sol atraves da atmosfera.
// Fonte: Hillaire, "A Scalable and Production-Ready Sky and Atmosphere
// Rendering Technique" (EGSR 2020) - Tabela 1 (coeficientes) + secao 5.2
// (marcha de 40 passos). Unidades internas: km. FISICA COM FONTE = CODIGO
// BLINDADO - nenhum destes numeros vira slider (doutrina da casa).
// ============================================================================
static glm::vec3 sun_transmittance(float cos_zenith)
{
    const glm::vec3 kRayleighS(5.802e-3f, 13.558e-3f, 33.1e-3f); // por km (Tabela 1)
    const float     kMieS = 3.996e-3f, kMieA = 4.40e-3f;         // por km
    const glm::vec3 kOzoneA(0.650e-3f, 1.881e-3f, 0.085e-3f);    // por km (so absorve)
    const float kRg = 6360.0f, kRt = 6460.0f;                    // raios (km)

    float s = std::clamp(cos_zenith, -1.0f, 1.0f);
    float c = std::sqrt(std::max(0.0f, 1.0f - s * s));
    glm::vec3 pos(0.0f, kRg + 0.2f, 0.0f);                       // observador ~200m
    glm::vec3 dir(c, s, 0.0f);

    float b = glm::dot(pos, dir);
    float disc_top = b * b - (glm::dot(pos, pos) - kRt * kRt);
    if (disc_top < 0.0f) return glm::vec3(0.0f);
    float t_top = -b + std::sqrt(disc_top);

    float disc_g = b * b - (glm::dot(pos, pos) - kRg * kRg);
    if (disc_g >= 0.0f && (-b - std::sqrt(disc_g)) > 0.0f) return glm::vec3(0.0f); // chao bloqueia

    glm::vec3 od(0.0f);
    const int kSteps = 40; // Hillaire 5.2
    float dt = t_top / (float)kSteps;
    for (int i = 0; i < kSteps; ++i)
    {
        glm::vec3 p = pos + dir * ((i + 0.5f) * dt);
        float h = glm::length(p) - kRg;
        float dr = std::exp(-h / 8.0f);                                        // Rayleigh
        float dm = std::exp(-h / 1.2f);                                        // Mie
        float dz = std::max(0.0f, 1.0f - std::fabs(h - 25.0f) / 15.0f);        // Ozonio (tenda)
        od += (kRayleighS * dr + glm::vec3(kMieS + kMieA) * dm + kOzoneA * dz) * dt;
    }
    return glm::vec3(std::exp(-od.x), std::exp(-od.y), std::exp(-od.z));
}

// ============================================================================
// M2.5-26-24: BALANCO DE BRANCO POR ADAPTACAO CROMATICA.
//
// A FONTE, e ela e do mesmo deck que ja nos deu a grade bilateral e o operador
// local. Ghost of Tsushima, SIGGRAPH 2021 "Advances in Real-Time Rendering",
// slide 155, VERBATIM:
//   "Before tossing our existing pipeline and LUTs, we tried adding white
//    balance controls. Free, since we can roll it into the Rec.709 => tonemap
//    matrix. WORKED SO WELL THAT WE DIDN'T NEED TO CHANGE OUR GRADING APPROACH."
// slide 156, VERBATIM:
//   "Rather than providing color temperature and hue sliders, WE LET THE ARTIST
//    CHOOSE THE COLOR THEY WANTED WHITE TO CHANGE *TO*. [...] if you want the
//    scene to be bluer, you pick a blue color, which the artists found more
//    intuitive."
//   "The transformation we used is called a chromatic adaptation matrix [...]
//    Specifically, we used the VON KRIES transform in BRADFORD LMS color space."
//
// POR QUE ESTA ESTACAO EXISTE, e nao um slider de saturacao. A queixa medida foi
// "quando a luz da lua bate nas nuvens, a diferenca das partes iluminadas para as
// nao iluminadas e gritante". Esse gritante tem DUAS metades: brilho e MATIZ. A
// metade de matiz foi medida aqui: a linha da cena e
//     astro_col = moon_tint * t_moon * I_lua
// e o t_moon (transmitancia na direcao da lua) avermelha. Rodando os coeficientes
// do proprio motor, a razao R/B da transmitancia por elevacao da lua:
//     30 graus 1.50x | 20 graus 1.81x | 10 graus 3.11x | 5 graus 8.06x
// Multiplicando pelo moon_tint = {0.80, 0.88, 1.00} - que o comentario dele chama
// de "luar levemente frio (arte)" - sobra 1.20x / 1.45x / 2.49x / 6.45x. Ou seja:
// o artista pediu luar FRIO e a fisica ENTREGA LUAR QUENTE, invertendo a intencao.
// Esta correto (o luar avermelha no horizonte igual ao sol) e e perceptualmente
// errado - ninguem ve nuvem enluarada cor de areia. A correcao perceptual completa
// e o Purkinje (Cao et al. 2008, slides 160-173), que espera a matriz M do slide
// 164 - LACUNA declarada. Este e o outro instrumento que a mesma fonte prescreve,
// e ele nao tem lacuna nenhuma.
//
// O QUE ESTA FUNCAO FAZ. Recebe a cor que o BRANCO deve virar e devolve a matriz
// 3x3 que faz isso, em luz linear:
//     M = XYZ2RGB . Bradford^-1 . diag(LMS_alvo/LMS_origem) . Bradford . RGB2XYZ
//
// LUMINANCIA PRESERVADA de proposito: o alvo e normalizado para o mesmo Y do
// branco antes de virar razao de LMS. Sem isso o seletor de cor viraria um SEGUNDO
// botao de exposicao e brigaria com o fotometro - exatamente o tipo de botao com
// dois significados que esta temporada catalogou. So a CROMATICIDADE se move.
// MEDIDO no bloco isolado antes de embarcar: com alvo (1,1,1) o desvio maximo da
// identidade e 3.3e-07 (o default e no-op de verdade), o branco pousa na
// cromaticidade do alvo com desvio 0.0000, e a luminancia varia 0.0008%.
//
// ONDE ELA ENTRA. Em luz LINEAR, depois do operador local e ANTES da curva - e a
// fonte e explicita ("roll it into the Rec.709 => tonemap matrix"). O Frostbite
// poe "style" DEPOIS do tonemap na ordem do deck dele; seguimos o Ghost aqui, e a
// razao e fisica: a adaptacao cromatica de Von Kries modela o sistema visual se
// adaptando ao ILUMINANTE, e so vale em luz linear. Aplicada depois de uma curva
// nao-linear ela deixa de ser o modelo que diz ser. Divergencia declarada.
//
// AS MATRIZES SAO PUBLICADAS, nenhuma e nossa: Rec.709/sRGB com branco D65 <-> XYZ
// da IEC 61966-2-1, e a Bradford da literatura padrao de adaptacao cromatica.
// ============================================================================
static void wb_matriz(const float alvo[3], float out9[9])
{
    const float R2X[9] = { 0.4124564f, 0.3575761f, 0.1804375f,
                           0.2126729f, 0.7151522f, 0.0721750f,
                           0.0193339f, 0.1191920f, 0.9503041f };
    const float X2R[9] = { 3.2404542f,-1.5371385f,-0.4985314f,
                          -0.9692660f, 1.8760108f, 0.0415560f,
                           0.0556434f,-0.2040259f, 1.0572252f };
    const float MA[9]  = { 0.8951f, 0.2664f,-0.1614f,
                          -0.7502f, 1.7135f, 0.0367f,
                           0.0389f,-0.0685f, 1.0296f };
    const float MAi[9] = { 0.9869929f,-0.1470543f, 0.1599627f,
                           0.4323053f, 0.5183603f, 0.0492912f,
                          -0.0085287f, 0.0400428f, 0.9684867f };
    auto mm = [](const float A[9], const float B[9], float O[9]) {
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
            float s = 0.0f; for (int k = 0; k < 3; ++k) s += A[r * 3 + k] * B[k * 3 + c];
            O[r * 3 + c] = s;
        }
    };
    auto mv = [](const float A[9], const float v[3], float o[3]) {
        for (int r = 0; r < 3; ++r) o[r] = A[r*3+0]*v[0] + A[r*3+1]*v[1] + A[r*3+2]*v[2];
    };

    const float branco[3] = { 1.0f, 1.0f, 1.0f };
    float a[3] = { std::max(alvo[0], 1e-4f), std::max(alvo[1], 1e-4f), std::max(alvo[2], 1e-4f) };

    float xyz_o[3], xyz_d[3];
    mv(R2X, branco, xyz_o);
    mv(R2X, a,      xyz_d);
    float k = (xyz_d[1] > 1e-6f) ? (xyz_o[1] / xyz_d[1]) : 1.0f;  // Y igual: so a cromaticidade anda
    xyz_d[0] *= k; xyz_d[1] *= k; xyz_d[2] *= k;

    float lms_o[3], lms_d[3];
    mv(MA, xyz_o, lms_o);
    mv(MA, xyz_d, lms_d);
    float D[9] = { 0,0,0, 0,0,0, 0,0,0 };
    for (int i = 0; i < 3; ++i)
        D[i * 3 + i] = (std::fabs(lms_o[i]) > 1e-6f) ? (lms_d[i] / lms_o[i]) : 1.0f;

    float t1[9], t2[9], t3[9];
    mm(D,   MA,  t1);
    mm(MAi, t1,  t2);
    mm(t2,  R2X, t1);
    mm(X2R, t1,  t3);
    for (int i = 0; i < 9; ++i) out9[i] = t3[i];
}

// ============================================================================
// M2.5-26-26: CONFORMIDADE. O sigma_t do AR LIMPO, derivado dos MESMOS
// coeficientes da Tabela 1 do Hillaire que a sun_transmittance ja usa acima.
//
// POR QUE ELE E FUNCAO E NAO CONSTANTE: se alguem mexer nos coeficientes da
// Tabela 1, este numero se corrige sozinho. Uma constante escrita a mao seria
// mais um lugar de onde a verdade pode divergir em silencio - a classe de
// defeito que esta temporada catalogou cinco vezes.
//
// PONDERADO POR LUMINANCIA (Rec.709) porque os sliders que o consomem sao
// ESCALARES: os shafts e a nevoa da cena marcham com um sigma so, enquanto a
// atmosfera marcha com os tres canais. Aproximacao declarada; a unificacao
// completa e o WeatherContext.
//
// O QUE ELE VALE: 2.1716e-05 por metro. E o que os dois sliders que o
// consomem NAO CONSEGUIAM EXPRESSAR ate esta estacao - o de Sigma tinha minimo
// 0.0002 (9,2x acima do fisico) e o de Fog tinha passo 1e-4. Nao era calibracao
// ruim: o EIXO estava errado, exatamente como o Defeito 3 da
// referencia-calibracao previu.
// ============================================================================
static float sigma_ar_limpo()
{
    const float kR[3] = { 5.802e-3f, 13.558e-3f, 33.1e-3f }; // Rayleigh, por km
    const float kMie  = 3.996e-3f + 4.40e-3f;                // Mie: espalha + absorve
    const float r = 0.2126f * kR[0] + 0.7152f * kR[1] + 0.0722f * kR[2];
    return (r + kMie) * 1e-3f; // por km -> por metro
}

bool sol_hora_manual = false;  // M2.5-24G-5: override de apresentacao (nao persiste de proposito)
float sol_hora = 12.0f;
float arco_inclinacao = 25.0f;
float sun_yaw = 45.0f;
float sun_pitch = -45.0f;
float sun_ambient = 0.55f;       // 26-7: LEGADO - nao ilumina mais nada
// M2.5-26-7: forca do ceu como fonte. 1.0 = fisico (a luminancia que a marcha do
// Hillaire devolve). E o unico numero de ARTE desta estacao, e o default e o fisico.
float sky_light_str = 1.0f;
// M2.5-26-8: perspectiva aerea da cena (Hillaire 5.4 por marcha curta).
bool  aerial_cena = true;         // desligado = a nevoa antiga, para comparacao A/B
int   aerial_steps = 6;           // 6 passos: o meio e quase homogeneo nos km da cena
float aerial_cena_str = 1.0f;     // multiplicador do sigma_t: 1.0 = ar fisico
// M2.5-26-2b: O SOL EM LUX (iluminancia no TOPO da atmosfera - o E_i da Eq. 3/11
// do Hillaire). Derivacao a partir da Tabela 11 do Frostbite (medicao com fotometro,
// Estocolmo, julho, ceu limpo), linha "Sol" as 12h = 90.700 lx no plano horizontal:
//   a nossa marcha entrega E_chao = E_topo * T(mu) * mu
//   com o arco de 25 graus o meio-dia poe o sol a 65 graus: mu = 0,9063
//   T(65 graus) luma = 0,8611  ->  T*mu = 0,7804
//   E_topo = 90.700 / 0,7804 = 116.216 lx
// CONTRAPROVA (declarada, nao escondida): com esse E_topo o nosso CEU entrega
// 8.338 lx no plano horizontal, contra os 19.300 lx da mesma tabela. O total fica
// 99.000 contra 110.000 - 10% baixo, 0,14 stop. A diferenca mora no ceu, e a
// explicacao provavel e que a medicao foi feita "com poucas nuvens" enquanto o
// modelo do Hillaire e de ceu LIMPO. Fica registrado para a contraprova Sunny 16.
// MARCHA ABSOLVIDA: 20 passos entregam 8.032 lx contra 8.338 com 320 passos - 3,7%
// de diferenca. A resolucao da marcha nao e a causa.
// M2.5-26-9a: 133.334 lx, NAO 116.216. O valor antigo saiu de 90.700/(T*mu) com a
// Tabela 11 do Frostbite (12h) e o cosseno ESQUECIDO: Estocolmo em julho ao meio-dia
// tem o sol a ~51,7 graus de elevacao, logo mu ~ 0,785, nao 1. O acervo da o numero
// direto, sem intermediario. Kuzminykh, Bachelorarbeit, eq. 2.17, VERBATIM:
//   "E_earth,sun ~ I_sun/d^2_au * cos(theta_sun) = 133 334 cos(theta_sun) lx when we
//    ignore Earth's atmosphere. E.g., given theta_sun = 0 and an atmosphere-less
//    Earth, the illuminance is around 133 334 lx."
// E este e o lugar certo do numero: o nosso sun_illum entra no TOPO da atmosfera e a
// Transmittance LUT aplica a extincao depois. Usar um valor ja atenuado (Tabela 11 e
// medida ao NIVEL DO SOLO) atenuava duas vezes.
// Referencia cruzada, Frostbite sec. 4.6 nota 29, VERBATIM: "lux due to the Sun at
// Earth surface in direction perpendicular to the ground without considering
// participating media should be in the range 105 000 and 114 000 lux" - esse e o
// valor NA SUPERFICIE (L=1.6e9 * Omega), nao no topo. Os dois numeros sao coerentes
// entre si; nos e que estavamos usando o de baixo no lugar do de cima.
float sun_intensity = 133334.0f;

float ray_strength = 0.6f;
float ray_decay = 0.97f;
int ray_samples = 48;
// M2.5-26-26: 0.55 -> 0.80. A auditoria de conformidade achou uma divergencia
// INTERNA: o atmo_common.hlsli marcha o ar com "const float g = 0.8f;
// // Cornette-Shanks, padrao do paper" (Hillaire, Tabela 1) e os shafts
// marchavam O MESMO AR com 0.55, herdado da linhagem Killzone, anterior ao
// Hillaire. Medido: olhando para o sol, g=0.80 espalha 5,88x mais que g=0.55
// (+2,56 stops). Duas fisicas para o mesmo ar, no mesmo quadro.
// A referencia-calibracao ja tinha decidido isto, verbatim: "discordo do nosso
// proprio botao: o 0,55 e anterior ao Hillaire".
float shaft_g = 0.80f;      // Cornette-Shanks (Hillaire T1) - o MESMO da atmosfera
float shaft_height_falloff = 0.015f; // M2.5-24J (B6): queda exp. da densidade do ar com a altura (1/u); 0 = ar uniforme
// M2.5-25-U6d: forca do dither de saida, em unidades de 1/255 (meio degrau = 1.0).
// ARTE por eliminacao: nenhuma fonte primaria do acervo prescreve dither de saida.
// A justificativa original ("padrao HDRP", 27/08) e apelo a outra engine, e nasceu
// quando a cena era 8-bit LDR - antes do HDR (24I) e do sRGB (25-U6a). 0 = desligado.
float dither_amt = 1.0f;
float shaft_sigma = 0.0006f; // M2.5-24I-5: OD 0.1-0.4 em 500u (Koschmieder) - antes estava 5x leitoso

float out_z_cutoff = 0.005f;
float out_grazing_scale = 4.0f;
float out_crease_low = 0.05f;
float out_crease_high = 0.3f;
float out_line_darken = 0.35f;
float out_crease_brighten = 1.25f;

bool draw_bloom = true;
float bloom_intensity = 0.25f;
float bloom_threshold = 0.75f;
// M2.5-26-11: teto do que entra na piramide da bloom. Forma com fonte (Frostbite
// 5.1.3: sensor saturado tem capacidade finita); NUMERO com precedente de producao
// (Godot glow_hdr_luminance_cap, default 12.0) e sem fonte no acervo - arte declarada.
float bloom_cap = 12.0f;
// M2.5-26-11: 0 = ombro exponencial da 24I (satura em tc 1,757), 1 = Khronos PBR
// Neutral (satura em tc 13,45; preserva bandas entre 0.08 e 0.8 por translacao rigida).
int tonemap_modo = 1;
// M2.5-26-14: operador LOCAL de tonemap (Patry, SIGGRAPH 2021, slide 137).
// c = escala de contraste. A fonte usa "75% High Contrast, 100% Low Contrast" como
// ajuste tipico em jogo (slide 145) - adotamos 0.75.
// d = forca do detalhe. Default 1.0; a fonte mostra 1.5 como "50% detail strength
// boost" (slide 146), usado "sparingly".
// M (ponto medio em log) NAO e slider: e derivado de exposure_key. A exposicao e
// definida para que a luminancia media caia em 0,1041667 na tela, entao log dessa
// chave E o ponto medio por construcao. Um numero a menos para calibrar.
bool  lt_on = true;
float lt_c  = 0.75f;
float lt_d  = 1.0f;
// M2.5-26-15: O CONTRASTE TEM DOIS LADOS.
// Patry, SIGGRAPH 2021, slide 145, TITULO VERBATIM:
//   "Typical In-Game Settings (75% High Contrast, 100% Low Contrast)"
// Ou seja: a fonte roda o jogo com DUAS escalas de contraste, e a de BAIXO em
// 100% - isto e, o que esta ABAIXO do ponto medio nao e tocado. So o que esta
// ACIMA e comprimido. As notas do palestrante nao explicam o titulo, entao a
// leitura "duas escalas, separadas em M" e INFERENCIA nossa - mas e a unica em
// que "100% Low Contrast" quer dizer alguma coisa, ja que 1.0 significa "nao
// mexe". Registrado como leitura, nao como citacao.
// POR QUE IMPORTA, com medida nossa (5h50): com um c unico de 0.75 e o M 0.74
// stop acima da media real do quadro, o ganho no pixel medio era exp((c-1)(B-M))
// = 1.138. O quadro inteiro clareava 13.8% - foi o "deixou tudo mais claro".
// Com lt_c_lo = 1.0 esse ganho vira exp(0) = 1.000 POR CONSTRUCAO.
float lt_c_lo = 1.0f;
// M2.5-26-16: SIGMA DA FAIXA do bilateral, em log-luminancia. A fonte NAO publica
// este numero - ela usa uma grade de 64 caixas e nunca diz a largura delas. 1.0 em
// log significa: diferencas acima de ~1,5 stop sao tratadas como BORDA (preservadas)
// e abaixo disso como ruido (suavizado). ARTE DECLARADA, no painel.
// M2.5-26-26: 1.00 -> 0.921. Durand & Dorsey, SIGGRAPH 2002, VERBATIM: "The
// value sigma_r = 0.4 performed consistently well for all our experiments" - em
// log10. O nosso PS_Down escreve log() natural, entao 0.4 * ln(10) = 0.921.
// O 1.00 estava +8,6% do valor publicado, sem motivo declarado.
float lt_sr = 0.921f;
// M2.5-26-16: peso do bilateral na mistura. Slide 140, VERBATIM: "we used a
// weighting of 40% for the bilateral blur and 60% for the Gaussian blur". Nasce no
// valor da fonte. MEDIDO NO NOSSO CASO (quadro das 5h54): com 0.40 nenhum 'c'
// positivo alcanca a faixa, porque a nossa gaussiana e cega a ela; a mistura
// precisa passar de ~0.80. A divergencia fica no slider, medida, nao num default.
float lt_mix = 0.40f;
// ============================================================================
// M2.5-26-23a: A GRADE BILATERAL. Todo numero aqui tem fonte nomeada.
//
// grid_sr = 0.921 ln. Durand & Dorsey, SIGGRAPH 2002, VERBATIM:
//   "The value sigma_r = 0.4 performed consistently well for all our
//    experiments" - em log10. 0.4 * ln(10) = 0.9210.
// E Chen, Paris & Durand 2007 sec. 3.2, VERBATIM: "we set the sampling rates
// s_s and s_r to correspond to the bandwidths of the Gaussian sigma_s and
// sigma_r" - ou seja, A LARGURA DA CAIXA Z E O RANGE SIGMA. Um parametro, nao
// dois. A profundidade de 12 fatias sai disso: faixa exposta medida nesta
// engine as 5h54 = ln(156.55/0.002986) = 10.87 ln, dividida por 0.921 = 11.8.
//
// grid_lo/grid_hi em unidades de sigma_r, ABAIXO e ACIMA do ponto medio M.
// A SOMA e 12 por construcao - e a profundidade derivada acima, e manter a soma
// em 12 mantem a largura da caixa igual a sigma_r, que e a regra do Chen.
// A REPARTICAO 2/10 vem de MEDIDA, no quadro real das 5h54 (ev_min 4.00): com
// M = -3.768 ln, o piso do quadro esta em ln(0.00299) = -5.81 = 2.22 sigma_r
// abaixo, e a faixa do horizonte em ln(156.5) = +5.05 = 9.58 sigma_r acima.
// Arredondado para 2 e 10, mantendo a soma. O vies para o lado CLARO e
// deliberado: e o lado que o operador local precisa separar. A assimetria bate
// com a da propria camera - Frostbite 5.1.2 poe a media em 12,7% da saturacao,
// entao sobra pouco abaixo dela e a cena vai muito alem acima.
// Amostras fora da faixa grampeiam na fatia extrema mas entram com o seu valor
// VERDADEIRO no (wiVi) - politica identica a do histograma da 26-12. Nada e
// perdido; o que se perde no extremo grampeado e SEPARACAO, nao energia.
// LACUNA DECLARADA, e ela e da fonte: nem Patry nem Chen publicam o intervalo
// absoluto do eixo Z. Chen normaliza a guia para [0,1] e a faixa e ENTRADA da
// aplicacao, nao constante do algoritmo. Estes dois sao sliders por isso.
//
// grid_taps = 13. NAO e escolhido, e derivado: a celula tem 2560/64 x 1417/32 =
// 40 x 44.3 = 1771 pixels da fonte, e 13x13 = 169 amostras = 9.54% deles.
// Chen sec. 3.5, VERBATIM: "using only 10% of the input pixels produces an
// output with no visual artifacts", valido "for typical values of sigma_s in
// [10,50] and sigma_r in [0.05,0.4]" - e nos satisfazemos AS DUAS metades
// (s_s = 40 e 44.3; sigma_r = 1/12 = 0.083). Modo de falha declarado pela
// fonte: "except at hard scene transitions".
// ============================================================================
bool  grid_on   = true;
// M2.5-26-23c: de onde o operador local tira o B. false = o caminho antigo
// (PS_Down + blur, que faz media de bloco ANTES de qualquer bilateral e por isso
// e cego a faixas finas); true = a grade bilateral da 26-23a. E um botao para
// comparar os dois a um clique de distancia, que e o unico jeito honesto de
// julgar - o [MEDIDA] le o composite, ANTES do operador local, e nunca vai ver
// a diferenca. Aqui o instrumento e a retina, e nao ha instrumento melhor.
bool  lt_b_grade = false;
float grid_mix   = 0.40f;  // Patry slide 140: 40% bilateral, 60% gaussiana
float grid_sr   = 0.921f;  // Durand & Dorsey 2002: 0.4 log10
float grid_lo   = 2.0f;    // sigma_r abaixo de M
float grid_hi   = 10.0f;   // sigma_r acima de M
int   grid_taps = 13;      // 13x13 = 169 = 9.54% da celula (Chen sec. 3.5)
float tonemap_shoulder = 0.75f; // M2.5-24I: inicio do ombro (abaixo = cores intactas)
// M2.5-26-24: a cor que o BRANCO deve virar (Ghost of Tsushima, slide 156).
// (1,1,1) = identidade EXATA - medido, desvio 3.3e-07. O default nao faz nada.
float wb_alvo[3] = { 1.0f, 1.0f, 1.0f };
float exposure = 1.0f;          // M2.5-24I-2: a chave do HDR (Lagarde 2014 via Hillaire)
                                // M2.5-26-6b: NAO E MAIS UM CONTROLE. Virou valor DERIVADO
                                // de exposure_ev a cada frame. Continua sendo o numero que
                                // os shaders recebem; so nao e mais o que o dedo toca.
// M2.5-26-6b: A EXPOSICAO MANUAL PASSA A SER EV100.
// Divida da 26-2a, cobrada agora. Depois que a pre-exposicao entrou, o slider
// "Exposicao" multiplica TODA a iluminacao antes de gravar no buffer - mas ficou
// com o intervalo 0,1-4,0 da regua antiga, enquanto a exposicao fisica de meio-dia
// vale ~2,6x10^-5. Desligar o automatico e mexer nele estourava tudo em branco.
// E o mesmo erro de forma do Defeito 3 desta temporada: controle LINEAR para uma
// grandeza que a fonte parametriza em STOPS. Frostbite (Lagarde & de Rousiers,
// SIGGRAPH 2014) eq. 69 e Listing 28: exposure = 1/(1.2*2^EV100). Um EV = um stop =
// um fator 2; a faixa util (-6 a 22) cobre luar e neve ao sol no MESMO slider.
// Default 15,0: sec. 5.1.4, regra Sunny 16 -> EV100 ~ 14,97 para o meio-dia claro.
float exposure_ev = 15.0f;
// M2.5-25-4: exposicao automatica analitica (ver comentario no calculo, no loop de frame)
bool  auto_exposure = true;      // desligado = o slider "Exposicao" manda, como antes
float exposure_key = 0.1041667f; // M2.5-25-17: NAO e arte. E K/(1.2*100) com K=12.5, a constante
                                 // do fotometro do Frostbite (eq. 69 + Listing 28, 2014). Substituindo
                                 // uma na outra: exposure = 0.1041667/Lavg - que e exatamente a forma
                                 // do nosso alvo. O gosto mora no EC (Compensacao), como a fonte define.
// M2.5-26-18b: RESOLUCAO DE RENDER. Globais para persistirem no tuning.ini.
// modo 2 = NATIVO e o default; a escada continua inteira (o 180 nunca saiu)
// porque a direcao de arte pode voltar a ela a qualquer momento.
int pixel_mode = 2;
const int alturas[] = { 180, 240, 270, 360, 450, 540, 720, 900, 1080 };
int altura_idx = 3;
int pixel_divisor = 4;
// ============================================================================
// M2.5-26-22: A COMPENSACAO GANHA VALOR, E ELE E DO ARTHUR.
//
// A FONTE reserva este parametro para exatamente isto. Frostbite, eq. 68:
//     EV100' = EV100 - EC
// e o texto de 5.1.1, VERBATIM: "artists can apply an exposure compensation
// (EC, in f-stops) in order to over-expose (i.e. brighten) or under-expose
// (i.e. darken) the image". A fonte NAO publica um valor - ela assume um
// artista decidindo por cena.
//
// POR QUE PRECISA DE UM. Medido no quadro de meio-dia com EC = 0:
//     BLOCO min   1873 cd/m2 -> exposto 0.0199 ->  39/255
//     Lavg        9818       -> exposto 0.1042 ->  91/255
//     PIXEL max  11250       -> exposto 0.1194 ->  97/255
// O quadro INTEIRO vivia entre 39 e 97 de 255 - nada alcancava o branco.
// Nao e defeito: a camera poe a media em 12,7% da saturacao (5.1.2) e a nossa
// cena de meio-dia tem 2,6 stops de faixa propria. Uma cena de 2,6 stops
// centrada em 12,7% nao chega no branco POR CONSTRUCAO.
//
// DE ONDE VEM O 2,30. Do olho do Arthur, ajustando ate o quadro ter branco.
// Extraido dos numeros dele: alvo = K/Lavg * 2^EC -> 6.19e-5 = 1.257e-5 * 2^EC
// -> EC = +2.30. Com ele o meio-dia passa a ir de 75/255 ate estourar.
// Isto e proveniencia legitima pela politica da casa: quando a documentacao nao
// fixa o numero, vale quem tem os olhos e a direcao artistica. Neste projeto
// essa pessoa e o Arthur, e o valor fica escrito aqui em vez de viver num
// tuning.ini que ja se perdeu uma vez.
//
// O CUSTO, dito com numero antes de alguem se assustar: EC e GLOBAL, e o
// crepusculo paga. Medido as 5h54 (BLOCO max 7095, exposicao 0.00544):
//     EC = 0.00  ->  faixa exposta  38.6  (+5.27 stops acima da saturacao)
//     EC = 2.30  ->  faixa exposta 190.0  (+7.57 stops)
// Sao os mesmos 2,30 stops, na hora em que menos se pode pagar. A NOITE nao
// paga: com o grampo do ev_min mordendo, o EC nao alcanca (o alvo ja esta acima
// do teto), entao dia e noite ficam desacoplados por construcao.
// A saida estrutural e o EC (ou o EV) ser FUNCAO DA HORA, tabelado - e esta e a
// primeira linha dessa tabela. Enquanto ela nao existe, se o amanhecer incomodar
// mais que o meio-dia apagado, baixe isto para 0 e espere a grade bilateral.
// ============================================================================
float exposure_bias_ev = 2.30f;  // eq. 68 - ARTE, calibrada no olho do Arthur
// ============================================================================
// M2.5-26-19: A ADAPTACAO PASSA A ANDAR EM STOPS POR SEGUNDO, E NAO EM
// "MULTIPLICADOR POR SEGUNDO".
//
// O DEFEITO, medido no nosso proprio codigo (aritmetica, nao opiniao). Ate aqui:
//     cur = prev + (alvo - prev) * (1 - exp(-dt * speed))
// e uma interpolacao no MULTIPLICADOR. A mesma constante de tempo produz taxas
// completamente diferentes em STOPS conforme a direcao, porque o multiplicador e
// exponencial no EV. Simulado com speed = 2,0 e um salto de 11 stops (EV 4 -> 15):
//     escuro -> claro (a exposicao FECHA):  ~2,9 stops/s, converge em ~5 s
//     claro -> escuro (a exposicao ABRE):  10,3 stops NO PRIMEIRO MEIO SEGUNDO
// Ou seja: ~7x mais rapido numa direcao que na outra, e na direcao ERRADA.
//
// A DIRECAO CERTA, VERBATIM - Unity HDRP, documentacao de Exposure:
//     "The human eye adapts slower to darkness than to lightness"
// O olho fecha rapido na luz e abre devagar no escuro. O nosso fazia o oposto.
//
// AS UNIDADES E OS NUMEROS, de quem ja resolveu isto:
//   Unreal Engine, Post Process Volume, Exposure:
//     "Speed Up"   - adaptacao indo para um ambiente MAIS CLARO, em f-stops/s
//     "Speed Down" - adaptacao indo para um ambiente MAIS ESCURO, em f-stops/s
//   Defaults publicados: Speed Up 3.0, Speed Down 1.0 (razao 3:1).
//
// A TRANSICAO LINEAR -> EXPONENCIAL:
//   Unreal, r.EyeAdaptation.ExponentialTransitionDistance, default 1.5
//   Acima dessa distancia do alvo a adaptacao e uma RAMPA de taxa constante
//   (o salto nao pode ser instantaneo por construcao); dentro dela vira
//   exponencial, para assentar sem batida.
//
// CHECAGEM NOSSA do 1,5 (derivacao, marcada como tal): a taxa instantanea do
// trecho exponencial d*(1-2^(-dt*v)) vale d*v*ln2 stops/s. Ela iguala a taxa da
// rampa (v stops/s) exatamente em d = 1/ln2 = 1,4427 stops. O 1,5 publicado pela
// Epic esta a 4% do ponto de continuidade C1 - a forma fecha sozinha.
//
// O QUE E MEU AQUI: a montagem em duas pecas (rampa + exponencial) e a conversao
// exposicao <-> EV. Nenhum NUMERO desta estacao e meu.
// ============================================================================
float expo_vel_up = 3.0f;   // stops/s indo para o CLARO  (Unreal Speed Up)
float expo_vel_dn = 1.0f;   // stops/s indo para o ESCURO (Unreal Speed Down)
float expo_ev_trans = 1.5f; // stops: onde a rampa vira exponencial (Unreal 1.5)
// ============================================================================
// M2.5-26-19: OS PERCENTIS DEIXAM DE SER MEUS.
//
// Frostbite sec. 5.1.1 nomeia o METODO (histograma) e nao publica os percentis.
// A 26-12 preencheu a lacuna com 5% / 95%, que eram MEUS - e o resultado foi uma
// janela que cobre 90% do quadro, ou seja, praticamente a media de tudo.
//
// Quem publica os numeros, VERBATIM - Unreal Engine, Post Process Volume,
// Exposure, Metering Mode Auto Exposure Histogram:
//   "Low Percent - a good starting range is 70 to 80"
//   "High Percent - a good starting range is 80 to 95"
//
// A LEITURA CERTA DO PARAMETRO, que a 26-12 errou: nao e "apara as caudas". E uma
// JANELA ESTREITA E ALTA. Ela pergunta "qual e a luminancia do decil superior do
// quadro", nao "qual e o brilho medio". Por isso 75/85 e nao 5/95: sao 10% do
// quadro, escolhidos no alto da distribuicao.
//
// MEDIDO NO HISTOGRAMA REAL DAS 5h50 (hist <0:458 0:468 1:54 2:43 3:1):
//   5/95   -> Lavg ~1,3 cd/m2   EV100 3,4   exposicao 0,077
//   75/85  -> Lavg ~5,9 cd/m2   EV100 5,6   exposicao 0,018
//   = 2,1 stops A MENOS de exposicao, que e exatamente o sinal do estouro.
// APROXIMACAO DECLARADA nesta conta: o histograma so nos da a contagem por
// DECADA; a distribuicao DENTRO de cada decada foi assumida uniforme em log10.
// Reconstrucoes diferentes dao entre 2,1 e 2,4 stops.
// ============================================================================
float expo_perc_lo = 0.75f;  // Unreal: "Low Percent - a good starting range is 70 to 80"
float expo_perc_hi = 0.85f;  // Unreal: "High Percent - a good starting range is 80 to 95"
// M2.5-25-5a: limites da exposicao = ARTE (quanto a noite tem direito de ser escura)
// M2.5-26-2b: com Lavg de meio-dia perto de 4.000 cd/m2 (Tabela 7: EV100 15), a
// exposicao passa a viver perto de 0,104167/4000 = 2,6x10^-5. Os limites mudam de
// ordem de grandeza junto. Ctrl+clique no slider digita o valor exato.
// M2.5-26-17: exposure_min/exposure_max FORAM REMOVIDOS. Desde a 26-2c quem manda
// e ev_min/ev_max; estes dois viraram espelho, continuaram sendo salvos no
// tuning.ini e alimentavam o estimador de CPU que a 26-17 matou. Peso morto que
// so servia para confundir quem lesse o arquivo de config.
// M2.5-26-2c: OS LIMITES DA EXPOSICAO PASSAM A SER EV100.
// Com o sol em lux, a exposicao de meio-dia vale 2,6x10^-5 e a de noite de lua
// cheia vale 1,71 - SsEIS ORDENS DE GRANDEZA de amplitude. Nenhum slider linear
// expressa isso, e foi exatamente por isso que a 26-2b quebrou: o meu teto de
// 1x10^-3 mordia a noite por 10,7 stops.
// A fonte ja parametriza isso, Listing 28 (Frostbite 5.1):
//     exposure = convertEV100toExposure(EV100) = 1 / (1.2 * 2^EV100)
// EV alto = pouca exposicao (dia). EV baixo = muita exposicao (noite).
// Logo:  ev_min = o teto de exposicao (noite)   e   ev_max = o piso (dia).
// M2.5-26-17: este comentario citava exposure_min/exposure_max, que deixaram de
// existir. Comentario que descreve variavel morta e a mesma classe de mentira que
// o painel: alguem le, acredita, e decide errado.
// Referencias uteis (Tabela 7): EV100 15 = 4.096 cd/m2 = o meio-dia do Sunny 16.
// Lua cheia neste motor da Lavg ~ 0,061 cd/m2, que e EV100 ~ -1.
// ============================================================================
// M2.5-26-19: OS LIMITES PASSAM A SER OS DO INSTRUMENTO, NAO OS DA ARTE.
//
// Frostbite sec. 5.1 NAO TEM GRAMPO. Verificado, NOT FOUND: nao existe piso de
// EV, teto, nem limite de auto-exposicao em toda a secao. O nosso grampo era
// arte vestida com nome de fisica - e a vistoria 26-x mostrou que ele estava
// grampeando as 5h50 em EV 4,00 com cinco digitos de coincidencia.
//
// OS LIMITES DO INSTRUMENTO, pela propria formula da fonte (Listing 28,
// EV100 = log2(Lux/pi) na leitura por iluminancia):
//   EV100 = -8  - o limite de captura de luz de ESTRELAS
//   EV100 = 18  - luz solar direta com margem
// Fora dessa faixa nao existe cena; dentro dela o medidor nunca deveria ser
// impedido de medir.
//
// O QUE MUDA NA PRATICA, medido: com a janela 75/85 o alvo das 5h50 vale
// EV 5,56 - fica 1,56 stop DENTRO do antigo grampo de EV 4. Ou seja, as duas
// mudancas desta estacao sao independentes: a janela conserta o crepusculo, e
// tirar o grampo conserta a noite. Com 5/95 elas brigavam (a vistoria 26-x
// mostrou que abrir o grampo PIORAVA o estouro em 0,57 stop); com 75/85 nao.
//
// ONDE A ARTE VOLTA A MORAR: em "Compensacao (EV)", que e o parametro que a
// fonte reserva para isso (eq. 68, EV100' = EV100 - EC). Se a noite ficar clara
// demais, o ajuste e la - ou subindo este slider de volta, agora sabendo que
// isso e uma decisao de arte e nao um limite do medidor.
// ============================================================================
// ============================================================================
// M2.5-26-22: O PISO DA NOITE VOLTA, AGORA COM O NOME CERTO.
//
// A 26-19 tirou este grampo alegando que Frostbite 5.1 nao tem nenhum
// (verificado, NOT FOUND) e o pos no limite do instrumento (-8). O resultado
// foi imediato e o Arthur reportou em uma frase: "a noite esta tao clara que
// parece quase dia".
//
// A ALGEBRA DE POR QUE ISSO ERA INEVITAVEL. Eq. 69 + Listing 28 dao
// alvo = K / Lavg, logo Lavg * alvo = K = 0,104 para QUALQUER cena. Medido:
//     noite, chao   Lavg 0.09424 x exposicao 1.004      = 0.0946
//     noite, ceu    Lavg 0.12    x exposicao 0.8604     = 0.1032
//     dia 11h89     Lavg 9818    x exposicao 1.061e-05  = 0.1042
// A media exposta e SEMPRE 0,104. Noite e dia recebem o mesmo brilho medio na
// tela - e isso e o que exposicao automatica FAZ, nao um defeito dela.
//
// E POR ISSO CONSERTAR A LUA NAO RESOLVE: divida moon_scene_bright por 10 e o
// alvo multiplica por 10; a tela nao muda um valor. (A lua CONTINUA errada por
// 3,4 stops, por dois caminhos independentes - so nao e este o sintoma.)
//
// A UNICA coisa que escurecia a noite era este grampo:
//     ev_min 4.06 -> exposicao maxima 0.0507 ; alvo da noite 0.4518
//     = 3,18 stops de escurecimento
//
// DE ONDE VEM O 4,06. Do olho do Arthur, subindo o slider ate a noite parecer
// noite. O valor que estava aqui antes da 26-19 era 4,00. O olho dele
// reproduziu o numero antigo com 0,06 de diferenca, o que e a melhor prova que
// a gente vai ter de que isto sempre foi ARTE e nunca foi fisica.
// Nao existe resposta publicada para "quao escura a noite deve ser". A
// Frostbite nao tem grampo porque assume um artista decidindo. Aqui o artista
// e o Arthur, e o numero e dele.
//
// ev_max continua em 18 (limite do instrumento, EV100 = log2(lux/pi), sol
// direto com margem). Ele nunca mordeu em cena nenhuma - o meio-dia mede 16.
// ============================================================================
float ev_min = 4.06f;   // ARTE, olho do Arthur: o piso da noite (era 4.00 antes da 26-19)
float ev_max = 18.0f;   // limite do CLARO: EV100 18 = sol direto com margem
// M2.5-26-2b: agora em cd/m2 DE VERDADE. Item N3.1 da checklist da noite, com tres
// fontes convergentes: o ceu sem lua vale 2x10^-4 cd/m2 (22,0 mag/arcsec2). Ate a
// 25-U6 esse numero era inexprimivel - o passo minimo do slider valia 66.000x o
// valor certo. Agora ele e simplesmente o default.
float night_sky_floor = 0.0002f;
// M2.5-25-7: disco solar. O TAMANHO e fisico (0.2725 graus de raio, real); o BRILHO e arte
// (o fisico seria E/Omega, ~15.000x - impossivel nas nossas unidades arbitrarias por ora).
float sun_disk_deg = 0.2725f;
// M2.5-26-2b: MULTIPLICADOR sobre a radiancia que conserva energia, L = E / Omega.
// 1.0 = fisico. Mudar o raio do disco por direcao de arte deixa de mudar a energia
// total que ele emite - so espalha a mesma energia numa area maior.
// M2.5-26-2c: o multiplicador cai para 0.005 e ISSO E UMA LACUNA DECLARADA, nao
// uma calibracao. A radiancia que conserva energia num disco de 3 graus vale
// 1,35x10^7 cd/m2, que pos-exposicao da 351 - oito stops acima do limiar do bloom,
// e o bloom inunda o quadro inteiro. A propria fonte avisa que nao resolveu isto:
// sec. 4.6, Remark, VERBATIM: "We have no energy conserving factor for the disk
// area light, as we have not found a correct one currently."
// O caminho documentado para objetos que devem florescer de forma CONSISTENTE em
// qualquer exposicao e o Listing 29 (sec. 5.1.3): exprimir o brilho como
// COMPENSACAO DE EXPOSICAO sobre o EV corrente, e nao em cd/m2 absolutos -
// "VFX artists want consistent bloom strength for their effect either in the day
// or in the night". Isso e a 26-2d. Ate la, 0.005 e um freio de mao honesto.
float sun_disk_bright = 0.005f;
// M2.5-25-8: cor do piso noturno. NAO vem da fonte (ver comentario no sky.hlsl):
// default azul-acinzentado, que e como o olho adaptado ao escuro ve o ceu sem lua.
float night_tint[3] = { 0.55f, 0.66f, 1.00f };
// M2.5-25-9a: a lua. A ELONGACAO (angulo Sol-Lua visto daqui) E a fase: 0 = nova,
// 180 = cheia, 90/270 = quartos. O ciclo real leva ~29,5 dias; por ora e slider.
float moon_elong_deg = 180.0f;   // cheia
float moon_disk_deg = 0.26f;     // raio angular real (diametro ~0.52 graus)
// M2.5-26-2b: MULTIPLICADOR sobre L = (rho/pi) * E_sol * N.L, com rho = 0.11 (bond
// albedo lunar, N2.3). Com E_sol = 116.216 lx e lua cheia o pico da ~4.070 cd/m2, e
// a luminancia real da superficie lunar fica entre 2.500 e 6.000 cd/m2. 1.0 = fisico.
float moon_disk_bright = 1.0f;
// M2.5-25-9c: a luz que a lua joga na CENA. ARTE: a razao fisica real entre lua
// cheia e sol e ~1/400.000 - nenhum jogo usa isso literalmente.
// M2.5-26-2b: A LUA EM LUX. Calibrada pelo item N2.4 (ceu de lua cheia ~ 0,03
// cd/m2): a sonda sobre o nosso proprio modelo mostra que 1 lx de lua produz
// 0,017108 cd/m2 no ceu a 45 graus, logo E_lua = 0,03/0,017108 = 1,754 lx.
// DISCREPANCIA DECLARADA: isso da 16,0 stops entre sol e lua, e a checklist deriva
// 17,6 stops comparando ceu de meio-dia com ceu de lua cheia. 1,6 stop em aberto -
// resolver na contraprova Sunny 16, NAO por ajuste de gosto. O que importa agora e
// que o numero finalmente CABE no controle: antes o slider era 0..1 linear.
float moon_scene_bright = 1.754f;
// M2.5-25-13: multiplicador da profundidade optica da perspectiva aerea das nuvens.
// 1.0 = extincao fisica pura (Tabela 1 do Hillaire). Acima disso e bruma de ARTE.
float aerial_str = 1.0f;
float moon_tint[3] = { 0.80f, 0.88f, 1.00f }; // luar levemente frio (arte)
// ============================================================================
// M2.5-25-18: AS ESTRELAS. Item 5 do VEREDITO da checklist da noite, e o
// primeiro depois que as unidades fisicas destravaram - o mapa-do-projeto diz
// por que ele esperava, verbatim: "A conversao canonica e magnitude aparente ->
// luminancia (Pogson, N3.4). Magnitude e uma escala fotometrica: com o ceu ja
// em cd/m2 ela entra direto, SEM GANHO ARBITRARIO."
// A escala absoluta NAO e slider: sai de N3.1 + N3.4 e vive no shader. O que e
// slider aqui e o que a fonte nao fixa.
// ============================================================================
float stars_str     = 1.0f;   // 1.0 = a luminancia fisica. Acima disso e ARTE.
float stars_mag_lim = 6.50f;  // magnitude mais fraca desenhada (limite do olho nu)
float stars_size    = 0.70f;  // sigma do borrao, em PIXELS. NAO muda o brilho:
                              // a energia e conservada, so o espalhamento muda.
float stars_dens    = 40.0f;  // celulas por lado de face do cubo: 6*40^2 = 9.600
float stars_slope   = 0.50f;  // LACUNA: a inclinacao da contagem por magnitude
                              // nao tem fonte no acervo. Entra como arte.
float shaft_cloud_occ = 0.95f;  // M2.5-24I-5: quase-binario de verdade (T ~ 0.05 sob nuvem)

bool draw_grass = true;
float grass_scale = 1.0f;
float grass_backlight = 0.35f;
float grass_specular = 0.25f;

bool draw_shadows = true;
bool debug_shadows = false;
float obj_shadow_str = 0.8f;
float obj_shadow_bias = 0.005f;

// M2.5-26-M: half-float -> float, para ler de volta o mapa de log-luminancia.
static float h2f(uint16_t h)
{
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    uint32_t f;
    if (e == 0u) {
        if (m == 0u) { f = s << 31; }
        else { uint32_t ee = 127u - 15u + 1u;
               while ((m & 0x400u) == 0u) { m <<= 1; --ee; }
               m &= 0x3FFu; f = (s << 31) | (ee << 23) | (m << 13); }
    } else if (e == 31u) { f = (s << 31) | 0x7F800000u | (m << 13); }
    else { f = (s << 31) | ((e - 15u + 127u) << 23) | (m << 13); }
    float r; std::memcpy(&r, &f, 4); return r;
}
static bool g_medir_pedido = false; // M2.5-26-M: botao "Medir agora"
// M2.5-26-17: o painel passa a mostrar a ULTIMA MEDIDA, com o tick em que ela foi
// tirada. E um retrato datado em vez de uma estimativa continua que nao renderiza.
static double   g_med_expo = 0.0;
static double   g_med_ev   = 0.0;
static uint64_t g_med_tick = 0;
static double   g_med_lavg = 0.0; // M2.5-26-20: a replica de CPU do l_avg, para
                                  // confrontar com o que a GPU de fato calculou

auto save_tuning = []() {
    std::filesystem::create_directories("config");
    std::ofstream f("config/tuning.ini");
    if (!f) {
        SDL_Log("FALHA: Nao foi possivel salvar config/tuning.ini");
        return;
    }

    f << "wind_dir_deg=" << wind::dir_deg << "\n";
    f << "wind_strength=" << wind::strength << "\n";
    f << "grass_clump_pull=" << grass_clump_pull << "\n";
    f << "grass_clump_cell=" << grass_clump_cell << "\n";
    f << "grass_density=" << grass_density << "\n";
    f << "grass_verdice_min=" << grass_verdice_min << "\n";
    f << "grass_slope_min=" << grass_slope_min << "\n";
    f << "grass_border_sharp=" << grass_border_sharp << "\n";
    f << "grass_scale=" << grass_scale << "\n";
    f << "grass_backlight=" << grass_backlight << "\n";
    f << "grass_specular=" << grass_specular << "\n";
    f << "draw_grass=" << (draw_grass ? "1" : "0") << "\n";
    f << "draw_sky=" << (draw_sky ? "1" : "0") << "\n";
    f << "fog_density=" << fog_density << "\n";
    f << "draw_clouds=" << (draw_clouds ? "1" : "0") << "\n";
    f << "cloud_cobertura=" << cloud_cobertura << "\n";
    f << "cloud_densidade=" << cloud_densidade << "\n";
    f << "cloud_escala=" << cloud_escala << "\n";
    f << "cloud_speed=" << cloud_speed << "\n";
    f << "cloud_passos=" << cloud_passos << "\n";
    f << "cloud_h_min=" << cloud_h_min << "\n";
    f << "cloud_h_max=" << cloud_h_max << "\n";
    f << "cloud_cap=" << cloud_cap << "\n";
    f << "cloud_absorcao=" << cloud_absorcao << "\n";
    f << "cloud_hg_g_frontal=" << cloud_hg_g_frontal << "\n";
    f << "cloud_powder_str=" << cloud_powder_str << "\n";
    f << "shadow_str=" << shadow_str << "\n";
    f << "cloud_type_blend=" << cloud_type_blend << "\n";
    f << "cloud_erosao_str=" << cloud_erosao_str << "\n";
    f << "cloud_silhouette=" << cloud_silhouette << "\n";
    f << "cloud_bands=" << cloud_bands << "\n";
    f << "cloud_detail_str=" << cloud_detail_str << "\n";
    f << "cloud_curl_str=" << cloud_curl_str << "\n";
    f << "cloud_ambient_str=" << cloud_ambient_str << "\n";
    f << "cloud_silver_k=" << cloud_silver_k << "\n";
    f << "cloud_ms_str=" << cloud_ms_str << "\n";
    f << "cloud_ms_g2=" << cloud_ms_g2 << "\n";
    f << "cloud_iso_str=" << cloud_iso_str << "\n";
    f << "sky_gain=" << sky_gain << "\n";
    f << "cloud_silver_spread=" << cloud_silver_spread << "\n";
    f << "clima_cov_teto=" << clima_cov_teto << "\n";
    f << "clima_type_teto=" << clima_type_teto << "\n";
    f << "cloud_curvatura=" << cloud_curvatura << "\n";
    f << "cloud_fade_ini=" << cloud_fade_ini << "\n";
    f << "cloud_passo_far=" << cloud_passo_far << "\n";
    f << "sol_automatico=" << (sol_automatico ? "1" : "0") << "\n";
    f << "arco_inclinacao=" << arco_inclinacao << "\n";
    f << "sun_yaw=" << sun_yaw << "\n";
    f << "sun_pitch=" << sun_pitch << "\n";
    f << "sun_ambient=" << sun_ambient << "\n";
    f << "sky_light_str=" << sky_light_str << "\n"; // 26-7
    f << "aerial_cena=" << (aerial_cena ? 1 : 0) << "\n";   // 26-8
    f << "aerial_steps=" << aerial_steps << "\n";
    f << "aerial_cena_str=" << aerial_cena_str << "\n";
    f << "sun_intensity=" << sun_intensity << "\n";
    f << "ray_strength=" << ray_strength << "\n";
    f << "ray_decay=" << ray_decay << "\n";
    f << "ray_samples=" << ray_samples << "\n";
    f << "shaft_g=" << shaft_g << "\n";
    f << "shaft_sigma=" << shaft_sigma << "\n";
    f << "shaft_height_falloff=" << shaft_height_falloff << "\n";
    f << "dither_amt=" << dither_amt << "\n";
    f << "draw_bloom=" << (draw_bloom ? "1" : "0") << "\n";
    f << "bloom_intensity=" << bloom_intensity << "\n";
    f << "bloom_threshold=" << bloom_threshold << "\n";
    f << "bloom_cap=" << bloom_cap << "\n";           // 26-11
    f << "tonemap_modo=" << tonemap_modo << "\n";     // 26-11
    f << "lt_on=" << (lt_on ? 1 : 0) << "\n";          // 26-14
    f << "lt_c=" << lt_c << "\n";
    f << "lt_d=" << lt_d << "\n";
    f << "lt_c_lo=" << lt_c_lo << "\n";   // 26-15
    f << "lt_sr=" << lt_sr << "\n";       // 26-16
    f << "lt_mix=" << lt_mix << "\n";
    f << "lt_b_grade=" << (lt_b_grade ? 1 : 0) << "\n"; // 26-23c
    f << "grid_mix=" << grid_mix << "\n";
    f << "grid_on=" << (grid_on ? 1 : 0) << "\n";   // 26-23a
    f << "grid_sr=" << grid_sr << "\n";
    f << "grid_lo=" << grid_lo << "\n";
    f << "grid_hi=" << grid_hi << "\n";
    f << "grid_taps=" << grid_taps << "\n";
    f << "tonemap_shoulder=" << tonemap_shoulder << "\n";
    f << "wb_alvo_r=" << wb_alvo[0] << "\n";   // 26-24
    f << "wb_alvo_g=" << wb_alvo[1] << "\n";
    f << "wb_alvo_b=" << wb_alvo[2] << "\n";
    f << "exposure=" << exposure << "\n";
    f << "exposure_ev=" << exposure_ev << "\n"; // 26-6b
    f << "auto_exposure=" << (auto_exposure ? 1 : 0) << "\n";
    f << "exposure_key=" << exposure_key << "\n";
    f << "exposure_bias_ev=" << exposure_bias_ev << "\n";
    f << "expo_vel_up=" << expo_vel_up << "\n";     // 26-19
    f << "expo_vel_dn=" << expo_vel_dn << "\n";
    f << "expo_ev_trans=" << expo_ev_trans << "\n";
    f << "expo_perc_lo=" << expo_perc_lo << "\n"; // 26-12
    f << "expo_perc_hi=" << expo_perc_hi << "\n";
    f << "pixel_mode=" << pixel_mode << "\n";       // 26-18b
    f << "altura_idx=" << altura_idx << "\n";
    f << "pixel_divisor=" << pixel_divisor << "\n";
    f << "ev_min=" << ev_min << "\n";
    f << "ev_max=" << ev_max << "\n";
    f << "night_sky_floor=" << night_sky_floor << "\n";
    f << "sun_disk_deg=" << sun_disk_deg << "\n";
    f << "sun_disk_bright=" << sun_disk_bright << "\n";
    f << "night_tint_r=" << night_tint[0] << "\n";
    f << "night_tint_g=" << night_tint[1] << "\n";
    f << "night_tint_b=" << night_tint[2] << "\n";
    f << "moon_elong_deg=" << moon_elong_deg << "\n";
    f << "moon_disk_deg=" << moon_disk_deg << "\n";
    f << "moon_disk_bright=" << moon_disk_bright << "\n";
    f << "moon_scene_bright=" << moon_scene_bright << "\n";
    f << "aerial_str=" << aerial_str << "\n";
    f << "stars_str=" << stars_str << "\n";           // 25-18
    f << "stars_mag_lim=" << stars_mag_lim << "\n";
    f << "stars_size=" << stars_size << "\n";
    f << "stars_dens=" << stars_dens << "\n";
    f << "stars_slope=" << stars_slope << "\n";
    f << "moon_tint_r=" << moon_tint[0] << "\n";
    f << "moon_tint_g=" << moon_tint[1] << "\n";
    f << "moon_tint_b=" << moon_tint[2] << "\n";
    f << "shaft_cloud_occ=" << shaft_cloud_occ << "\n";
    f << "out_z_cutoff=" << out_z_cutoff << "\n";
    f << "out_grazing_scale=" << out_grazing_scale << "\n";
    f << "out_crease_low=" << out_crease_low << "\n";
    f << "out_crease_high=" << out_crease_high << "\n";
    f << "out_line_darken=" << out_line_darken << "\n";
    f << "out_crease_brighten=" << out_crease_brighten << "\n";
    f << "draw_shadows=" << (draw_shadows ? "1" : "0") << "\n";
    f << "obj_shadow_str=" << obj_shadow_str << "\n";
    f << "obj_shadow_bias=" << obj_shadow_bias << "\n";
    SDL_Log("Tuning salvo com sucesso em config/tuning.ini");
    };

auto load_tuning = []() {
    std::ifstream f("config/tuning.ini");
    if (!f) {
        SDL_Log("tuning.ini nao encontrado. Usando defaults.");
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto delim = line.find('=');
        if (delim == std::string::npos) continue;

        std::string key = line.substr(0, delim);
        std::string val = line.substr(delim + 1);

        try {
            if (key == "wind_dir_deg") wind::dir_deg = std::stof(val);
            else if (key == "wind_strength") wind::strength = std::stof(val);
            else if (key == "grass_clump_pull") grass_clump_pull = std::stof(val);
            else if (key == "grass_clump_cell") grass_clump_cell = std::stof(val);
            else if (key == "grass_density") grass_density = std::stof(val);
            else if (key == "grass_verdice_min") grass_verdice_min = std::stof(val);
            else if (key == "grass_slope_min") grass_slope_min = std::stof(val);
            else if (key == "grass_border_sharp") grass_border_sharp = std::stof(val);
            else if (key == "grass_scale") grass_scale = std::stof(val);
            else if (key == "grass_backlight") grass_backlight = std::stof(val);
            else if (key == "grass_specular") grass_specular = std::stof(val);
            else if (key == "draw_grass") draw_grass = (val == "1");
            else if (key == "draw_sky") draw_sky = (val == "1");
            else if (key == "fog_density") fog_density = std::stof(val);
            else if (key == "draw_clouds") draw_clouds = (val == "1");
            else if (key == "cloud_cobertura") cloud_cobertura = std::stof(val);
            else if (key == "cloud_densidade") cloud_densidade = std::stof(val);
            else if (key == "cloud_escala") cloud_escala = std::stof(val);
            else if (key == "cloud_speed") cloud_speed = std::stof(val);
            else if (key == "cloud_passos") cloud_passos = std::stoi(val);
            else if (key == "cloud_h_min") cloud_h_min = std::stof(val);
            else if (key == "cloud_h_max") cloud_h_max = std::stof(val);
            else if (key == "cloud_cap") cloud_cap = std::stof(val);
            else if (key == "cloud_absorcao") cloud_absorcao = std::stof(val);
            else if (key == "cloud_hg_g_frontal") cloud_hg_g_frontal = std::stof(val);
            else if (key == "cloud_powder_str") cloud_powder_str = std::stof(val);
            else if (key == "shadow_str") shadow_str = std::stof(val);
            else if (key == "cloud_type_blend") cloud_type_blend = std::stof(val);
            else if (key == "cloud_erosao_str") cloud_erosao_str = std::stof(val);
            else if (key == "cloud_silhouette") cloud_silhouette = std::stof(val);
            else if (key == "cloud_bands") cloud_bands = std::stof(val);
            else if (key == "cloud_detail_str") cloud_detail_str = std::stof(val);
            else if (key == "cloud_curl_str") cloud_curl_str = std::stof(val);
            else if (key == "cloud_ambient_str") cloud_ambient_str = std::stof(val);
            else if (key == "cloud_silver_k") cloud_silver_k = std::stof(val);
            else if (key == "cloud_ms_str") cloud_ms_str = std::stof(val);
            else if (key == "cloud_ms_g2") cloud_ms_g2 = std::stof(val);
            else if (key == "cloud_iso_str") cloud_iso_str = std::stof(val);
            else if (key == "sky_gain") sky_gain = std::stof(val);
            else if (key == "cloud_silver_spread") cloud_silver_spread = std::stof(val);
            else if (key == "clima_cov_teto") clima_cov_teto = std::stof(val);
            // ================================================================
            // M2.5-26-24a: A CADEIA DE 'else if' E QUEBRADA AQUI, DE PROPOSITO.
            //
            // O DEFEITO, medido: o MSVC conta cada 'else if' como um BLOCO
            // ANINHADO. O limite dele e 128 (C1061 - "blocos com aninhamento
            // muito profundo"), e esta cadeia tinha 123 ramos mais o try, o
            // while e a funcao em volta. A 26-24 acrescentou tres chaves e
            // estourou. O g++ compila sem reclamar; so o MSVC morre.
            //
            // POR QUE ISTO E SEGURO: as chaves sao UNICAS. Se um ramo do trecho
            // anterior casou, este trecho roda as comparacoes dele e nenhuma
            // casa - mesmo resultado, algumas comparacoes de string a mais, uma
            // vez so no arranque. O comportamento nao muda em nada.
            //
            // NAO EXISTE 'else' SOLTO nesta regiao (conferido antes do corte);
            // se existisse, cortar mudaria a quem ele se liga.
            // ================================================================
            if (key == "clima_type_teto") clima_type_teto = std::stof(val);
            else if (key == "cloud_curvatura") cloud_curvatura = std::stof(val);
            else if (key == "cloud_fade_ini") cloud_fade_ini = std::stof(val);
            else if (key == "cloud_passo_far") cloud_passo_far = std::stof(val);
            else if (key == "sol_automatico") sol_automatico = (val == "1");
            else if (key == "arco_inclinacao") arco_inclinacao = std::stof(val);
            else if (key == "sun_yaw") sun_yaw = std::stof(val);
            else if (key == "sun_pitch") sun_pitch = std::stof(val);
            else if (key == "sun_ambient") sun_ambient = std::stof(val);
            else if (key == "sky_light_str") sky_light_str = std::stof(val); // 26-7 (chave nova: default 1.0 se ausente)
            else if (key == "aerial_cena") aerial_cena = (std::stoi(val) != 0);      // 26-8
            else if (key == "aerial_steps") aerial_steps = std::stoi(val);
            else if (key == "aerial_cena_str") aerial_cena_str = std::stof(val);
            else if (key == "sun_intensity") sun_intensity = std::stof(val);
            else if (key == "ray_strength") ray_strength = std::stof(val);
            else if (key == "ray_decay") ray_decay = std::stof(val);
            else if (key == "ray_samples") ray_samples = std::stoi(val);
            else if (key == "shaft_g") shaft_g = std::stof(val);
            else if (key == "shaft_sigma") shaft_sigma = std::stof(val);
            else if (key == "shaft_height_falloff") shaft_height_falloff = std::stof(val);
            else if (key == "dither_amt") dither_amt = std::stof(val);
            else if (key == "draw_bloom") draw_bloom = (val == "1");
            else if (key == "bloom_intensity") bloom_intensity = std::stof(val);
            else if (key == "bloom_threshold") bloom_threshold = std::stof(val);
            else if (key == "bloom_cap") bloom_cap = std::stof(val);         // 26-11
            else if (key == "tonemap_modo") tonemap_modo = std::stoi(val);   // 26-11
            else if (key == "lt_on") lt_on = (std::stoi(val) != 0);          // 26-14
            else if (key == "lt_c") lt_c = std::stof(val);
            else if (key == "lt_d") lt_d = std::stof(val);
            else if (key == "lt_c_lo") lt_c_lo = std::stof(val);            // 26-15
            else if (key == "lt_sr") lt_sr = std::stof(val);                // 26-16
            else if (key == "lt_mix") lt_mix = std::stof(val);
            else if (key == "lt_b_grade") lt_b_grade = (std::stoi(val) != 0); // 26-23c
            else if (key == "grid_mix") grid_mix = std::stof(val);
            else if (key == "grid_on") grid_on = (std::stoi(val) != 0);      // 26-23a
            else if (key == "grid_sr") grid_sr = std::stof(val);
            else if (key == "grid_lo") grid_lo = std::stof(val);
            else if (key == "grid_hi") grid_hi = std::stof(val);
            else if (key == "grid_taps") grid_taps = std::stoi(val);
            else if (key == "tonemap_shoulder") tonemap_shoulder = std::stof(val);
            else if (key == "wb_alvo_r") wb_alvo[0] = std::stof(val);   // 26-24
            else if (key == "wb_alvo_g") wb_alvo[1] = std::stof(val);
            else if (key == "wb_alvo_b") wb_alvo[2] = std::stof(val);
            // ================================================================
            // M2.5-26-24a: A CADEIA DE 'else if' E QUEBRADA AQUI, DE PROPOSITO.
            //
            // O DEFEITO, medido: o MSVC conta cada 'else if' como um BLOCO
            // ANINHADO. O limite dele e 128 (C1061 - "blocos com aninhamento
            // muito profundo"), e esta cadeia tinha 123 ramos mais o try, o
            // while e a funcao em volta. A 26-24 acrescentou tres chaves e
            // estourou. O g++ compila sem reclamar; so o MSVC morre.
            //
            // POR QUE ISTO E SEGURO: as chaves sao UNICAS. Se um ramo do trecho
            // anterior casou, este trecho roda as comparacoes dele e nenhuma
            // casa - mesmo resultado, algumas comparacoes de string a mais, uma
            // vez so no arranque. O comportamento nao muda em nada.
            //
            // NAO EXISTE 'else' SOLTO nesta regiao (conferido antes do corte);
            // se existisse, cortar mudaria a quem ele se liga.
            // ================================================================
            if (key == "exposure") exposure = std::stof(val);
            else if (key == "exposure_ev") exposure_ev = std::stof(val); // 26-6b: chave NOVA -
            // ausente no tuning.ini antigo, entao o default 15,0 (Sunny 16) prevalece sozinho
            // e nenhuma migracao e necessaria. O "exposure=" velho continua sendo lido e e
            // sobrescrito no primeiro frame pelo valor derivado - fica so como historico.
            else if (key == "auto_exposure") auto_exposure = (std::stoi(val) != 0);
            else if (key == "exposure_key") exposure_key = std::stof(val);
            else if (key == "exposure_bias_ev") exposure_bias_ev = std::stof(val);
            else if (key == "expo_vel_up") expo_vel_up = std::stof(val);       // 26-19
            else if (key == "expo_vel_dn") expo_vel_dn = std::stof(val);
            else if (key == "expo_ev_trans") expo_ev_trans = std::stof(val);
            else if (key == "expo_perc_lo") expo_perc_lo = std::stof(val); // 26-12
            else if (key == "expo_perc_hi") expo_perc_hi = std::stof(val);
            else if (key == "pixel_mode") pixel_mode = std::stoi(val);          // 26-18b
            else if (key == "altura_idx") altura_idx = std::stoi(val);
            else if (key == "pixel_divisor") pixel_divisor = std::stoi(val);
            else if (key == "ev_min") ev_min = std::stof(val);
            else if (key == "ev_max") ev_max = std::stof(val);
            else if (key == "night_sky_floor") night_sky_floor = std::stof(val);
            else if (key == "sun_disk_deg") sun_disk_deg = std::stof(val);
            else if (key == "sun_disk_bright") sun_disk_bright = std::stof(val);
            else if (key == "night_tint_r") night_tint[0] = std::stof(val);
            else if (key == "night_tint_g") night_tint[1] = std::stof(val);
            else if (key == "night_tint_b") night_tint[2] = std::stof(val);
            else if (key == "moon_elong_deg") moon_elong_deg = std::stof(val);
            else if (key == "moon_disk_deg") moon_disk_deg = std::stof(val);
            else if (key == "moon_disk_bright") moon_disk_bright = std::stof(val);
            else if (key == "moon_scene_bright") moon_scene_bright = std::stof(val);
            else if (key == "aerial_str") aerial_str = std::stof(val);
            else if (key == "stars_str") stars_str = std::stof(val);              // 25-18
            else if (key == "stars_mag_lim") stars_mag_lim = std::stof(val);
            else if (key == "stars_size") stars_size = std::stof(val);
            else if (key == "stars_dens") stars_dens = std::stof(val);
            else if (key == "stars_slope") stars_slope = std::stof(val);
            else if (key == "moon_tint_r") moon_tint[0] = std::stof(val);
            else if (key == "moon_tint_g") moon_tint[1] = std::stof(val);
            else if (key == "moon_tint_b") moon_tint[2] = std::stof(val);
            else if (key == "shaft_cloud_occ") shaft_cloud_occ = std::stof(val);
            else if (key == "out_z_cutoff") out_z_cutoff = std::stof(val);
            else if (key == "out_grazing_scale") out_grazing_scale = std::stof(val);
            else if (key == "out_crease_low") out_crease_low = std::stof(val);
            else if (key == "out_crease_high") out_crease_high = std::stof(val);
            else if (key == "out_line_darken") out_line_darken = std::stof(val);
            else if (key == "out_crease_brighten") out_crease_brighten = std::stof(val);
            else if (key == "draw_shadows") draw_shadows = (val == "1");
            else if (key == "obj_shadow_str") obj_shadow_str = std::stof(val);
            else if (key == "obj_shadow_bias") obj_shadow_bias = std::stof(val);
        }
        catch (...) {
            SDL_Log("Aviso: Falha ao fazer o parse da chave %s no tuning.ini", key.c_str());
        }
    }
    // M2.5-26-2b: MIGRACAO DE REGUA. Todo tuning.ini salvo antes desta estacao guarda
    // um sol em unidade arbitraria (~1.5). Carregar aquilo aqui deixaria as RAZOES
    // entre sol, lua, piso noturno e discos erradas por ordens de grandeza - e a
    // exposicao automatica esconderia o estrago, porque ela normaliza o brilho medio.
    // Detectamos pelo proprio valor: nenhuma calibracao em lux fica abaixo de 100.
    if (sun_intensity < 100.0f) {
        sun_intensity     = 133334.0f;  // 26-9a: Kuzminykh eq. 2.17 (topo da atmosfera)
        moon_scene_bright = 1.754f;     // N2.4 (ceu de lua cheia 0,03 cd/m2)
        night_sky_floor   = 0.0002f;    // N3.1 (2x10^-4 cd/m2)
        sun_disk_bright   = 1.0f;       // E / Omega
        moon_disk_bright  = 1.0f;       // rho/pi * E_sol
        sky_gain          = 1.0f;       // legado, sem funcao
        ev_min            = 4.06f;      // 26-22: ARTE, o piso da noite (olho do Arthur)
        ev_max            = 18.0f;      // 26-19: limite do instrumento (sol direto)
        sun_disk_bright   = 0.005f;     // 26-2c: ver o comentario do disco solar
        SDL_Log("[26-2b] tuning.ini era da regua antiga (sol=%.3f) - valores de LUZ resetados para as unidades fisicas. A ARTE nao foi tocada. Salvar Config para persistir.", 1.0f);
    }

    // M2.5-26-10a: O RAIO ANGULAR DOS DISCOS E GEOMETRIA, NAO ARTE.
    // Guarda propria, independente da migracao de lux acima, porque o estrago e de
    // outra natureza e pode sobreviver a um "Salvar Config".
    // Sol: raio 0,2725 graus (diametro 0,545) - Frostbite sec. 4.6 nota 28, verbatim:
    //   "The sun has an angular diameter between 0.526 and 0.545 [...] this mean a
    //    solid angle between 0.000066 and 0.000071 as seen from Earth."
    // Lua: raio ~0,26 graus (diametro ~0,52) - N1.1 da checklist da noite.
    // POR QUE ISTO E URGENTE, com numero: a luminancia do disco da LUA e
    //   L = (rho/pi) * E_sol * N.L
    // - uma radiancia de SUPERFICIE, que NAO depende do raio. O raio decide apenas
    // quantos pixels recebem esse valor, e area cresce com o quadrado. Um disco de
    // 3 graus contra os 0,26 reais emite (3/0,26)^2 = 133x a energia certa. Com a
    // bloom em cima, a lua vira um sol - foi exatamente o relato da retina.
    // (O disco do SOL e outro caso: ele divide por Omega, entao mudar o raio
    // CONSERVA energia - espalha a mesma luz por 121x a area. Nao estoura mais,
    // mas vira um bolo branco em vez de um ponto. Tambem errado, sem ser explosivo.)
    if (sun_disk_deg > 1.0f || moon_disk_deg > 1.0f) {
        SDL_Log("[26-10a] discos com raio %.3f / %.3f graus (sol/lua) - sao GEOMETRIA e estavam com valores da era da regua arbitraria. Restaurados para 0.2725 / 0.26. Salvar Config para persistir.", sun_disk_deg, moon_disk_deg);
        sun_disk_deg  = 0.2725f;
        moon_disk_deg = 0.26f;
    }
    SDL_Log("Tuning carregado de config/tuning.ini");
    };

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    int win_w = 1600;
    int win_h = 900;
    SDL_Window* window = SDL_CreateWindow("Minha Engine", win_w, win_h, SDL_WINDOW_RESIZABLE);
    if (!window) return 1;

#ifdef NDEBUG
    const bool gpu_debug = false;
#else
    const bool gpu_debug = true;
#endif

    SDL_GPUShaderFormat format_flags = SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_DXIL;
    SDL_GPUDevice* device = SDL_CreateGPUDevice(format_flags, gpu_debug, nullptr);
    if (!device) return 1;

    if (!SDL_ClaimWindowForGPUDevice(device, window)) return 1;

    if (!SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC)) {
        SDL_Log("Erro no SDL_SetGPUSwapchainParameters: %s", SDL_GetError());
    }

    SDL_GPUShaderCreateInfo vs_info = {};
    vs_info.code = g_basic_vs;
    vs_info.code_size = sizeof(g_basic_vs);
    vs_info.entrypoint = "VSMain";
    vs_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs_info.num_uniform_buffers = 1;
    SDL_GPUShader* vs = SDL_CreateGPUShader(device, &vs_info);
    if (!vs) return 1;

    SDL_GPUShaderCreateInfo ps_info = {};
    ps_info.code = g_basic_ps;
    ps_info.code_size = sizeof(g_basic_ps);
    ps_info.entrypoint = "PSMain";
    ps_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    ps_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    ps_info.num_samplers = 7; // M2.5-26-8: + as duas LUTs da atmosfera (t5/t6)
    ps_info.num_uniform_buffers = 2;
    SDL_GPUShader* ps = SDL_CreateGPUShader(device, &ps_info);
    if (!ps) return 1;

    SDL_GPUShaderCreateInfo fs_vs_info = {};
    fs_vs_info.code = g_fullscreen_vs;
    fs_vs_info.code_size = sizeof(g_fullscreen_vs);
    fs_vs_info.entrypoint = "VSMain";
    fs_vs_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    fs_vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    fs_vs_info.num_uniform_buffers = 0;
    SDL_GPUShader* fs_vs = SDL_CreateGPUShader(device, &fs_vs_info);
    if (!fs_vs) return 1;

    SDL_GPUShaderCreateInfo fs_ps_info = {};
    fs_ps_info.code = g_fullscreen_ps;
    fs_ps_info.code_size = sizeof(g_fullscreen_ps);
    fs_ps_info.entrypoint = "PSMain";
    fs_ps_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    fs_ps_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fs_ps_info.num_samplers = 9; // M2.5-26-23a: + o atlas da grade bilateral (t8)
    fs_ps_info.num_uniform_buffers = 1;
    SDL_GPUShader* fs_ps = SDL_CreateGPUShader(device, &fs_ps_info);
    if (!fs_ps) return 1;

    SDL_GPUShaderCreateInfo out_vs_info = {};
    out_vs_info.code = g_outline_vs;
    out_vs_info.code_size = sizeof(g_outline_vs);
    out_vs_info.entrypoint = "VSMain";
    out_vs_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    out_vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    out_vs_info.num_uniform_buffers = 0;
    SDL_GPUShader* out_vs = SDL_CreateGPUShader(device, &out_vs_info);
    if (!out_vs) return 1;

    SDL_GPUShaderCreateInfo out_ps_info = {};
    out_ps_info.code = g_outline_ps;
    out_ps_info.code_size = sizeof(g_outline_ps);
    out_ps_info.entrypoint = "PSMain";
    out_ps_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    out_ps_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    out_ps_info.num_samplers = 3;
    out_ps_info.num_uniform_buffers = 1;
    SDL_GPUShader* out_ps = SDL_CreateGPUShader(device, &out_ps_info);
    if (!out_ps) return 1;

    SDL_GPUShaderCreateInfo sky_vs_info = {};
    sky_vs_info.code = g_sky_vs;
    sky_vs_info.code_size = sizeof(g_sky_vs);
    sky_vs_info.entrypoint = "VSMain";
    sky_vs_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    sky_vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    sky_vs_info.num_uniform_buffers = 1;
    SDL_GPUShader* sky_vs = SDL_CreateGPUShader(device, &sky_vs_info);
    if (!sky_vs) { SDL_Log("FALHA sky_vs: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo sky_ps_info = {};
    sky_ps_info.code = g_sky_ps;
    sky_ps_info.code_size = sizeof(g_sky_ps);
    sky_ps_info.entrypoint = "PSMain";
    sky_ps_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    sky_ps_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    sky_ps_info.num_samplers = 6; // M2.5-26-2a: + a exposicao (t5) para a pre-exposicao
    sky_ps_info.num_uniform_buffers = 1;
    SDL_GPUShader* sky_ps = SDL_CreateGPUShader(device, &sky_ps_info);
    if (!sky_ps) { SDL_Log("FALHA sky_ps: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo grass_vs_info = {};
    grass_vs_info.code = g_grass_vs;
    grass_vs_info.code_size = sizeof(g_grass_vs);
    grass_vs_info.entrypoint = "VSMain";
    grass_vs_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    grass_vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    grass_vs_info.num_uniform_buffers = 1;
    SDL_GPUShader* grass_vs = SDL_CreateGPUShader(device, &grass_vs_info);
    if (!grass_vs) return 1;

    SDL_GPUShaderCreateInfo grass_ps_info = {};
    grass_ps_info.code = g_grass_ps;
    grass_ps_info.code_size = sizeof(g_grass_ps);
    grass_ps_info.entrypoint = "PSMain";
    grass_ps_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    grass_ps_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    grass_ps_info.num_samplers = 7; // M2.5-26-8: + as duas LUTs da atmosfera (t5/t6)
    grass_ps_info.num_uniform_buffers = 1;
    SDL_GPUShader* grass_ps = SDL_CreateGPUShader(device, &grass_ps_info);
    if (!grass_ps) return 1;

    SDL_GPUShaderCreateInfo bloom_vs_info = {};
    bloom_vs_info.code = g_bloom_vs;
    bloom_vs_info.code_size = sizeof(g_bloom_vs);
    bloom_vs_info.entrypoint = "VSMain";
    bloom_vs_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    bloom_vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    bloom_vs_info.num_uniform_buffers = 0;
    SDL_GPUShader* bloom_vs = SDL_CreateGPUShader(device, &bloom_vs_info);
    if (!bloom_vs) { SDL_Log("FALHA bloom_vs: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo b_thresh_info = {};
    b_thresh_info.code = g_bloom_thresh_ps;
    b_thresh_info.code_size = sizeof(g_bloom_thresh_ps);
    b_thresh_info.entrypoint = "PS_Threshold";
    b_thresh_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    b_thresh_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    b_thresh_info.num_samplers = 1;
    b_thresh_info.num_uniform_buffers = 1;
    SDL_GPUShader* bloom_thresh_ps = SDL_CreateGPUShader(device, &b_thresh_info);
    if (!bloom_thresh_ps) { SDL_Log("FALHA bloom_thresh_ps: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo b_down_info = {};
    b_down_info.code = g_bloom_down_ps;
    b_down_info.code_size = sizeof(g_bloom_down_ps);
    b_down_info.entrypoint = "PS_Down";
    b_down_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    b_down_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    b_down_info.num_samplers = 1;
    b_down_info.num_uniform_buffers = 1;
    SDL_GPUShader* bloom_down_ps = SDL_CreateGPUShader(device, &b_down_info);
    if (!bloom_down_ps) { SDL_Log("FALHA bloom_down_ps: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo b_up_info = {};
    b_up_info.code = g_bloom_up_ps;
    b_up_info.code_size = sizeof(g_bloom_up_ps);
    b_up_info.entrypoint = "PS_Up";
    b_up_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    b_up_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    b_up_info.num_samplers = 1;
    b_up_info.num_uniform_buffers = 1;
    SDL_GPUShader* bloom_up_ps = SDL_CreateGPUShader(device, &b_up_info);
    if (!bloom_up_ps) { SDL_Log("FALHA bloom_up_ps: %s", SDL_GetError()); return 1; }

    // M2.5-26-4: shaders da exposicao medida (Frostbite 2014, sec. 5.1)
    SDL_GPUShaderCreateInfo expo_vs_info = {};
    expo_vs_info.code = g_expo_vs;
    expo_vs_info.code_size = sizeof(g_expo_vs);
    expo_vs_info.entrypoint = "VSMain";
    expo_vs_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    expo_vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    expo_vs_info.num_uniform_buffers = 0;
    SDL_GPUShader* expo_vs = SDL_CreateGPUShader(device, &expo_vs_info);
    if (!expo_vs) { SDL_Log("FALHA expo_vs: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo expo_luma_info = {};
    expo_luma_info.code = g_expo_luma_ps;
    expo_luma_info.code_size = sizeof(g_expo_luma_ps);
    expo_luma_info.entrypoint = "PS_Luma";
    expo_luma_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    expo_luma_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    expo_luma_info.num_samplers = 2; // M2.5-26-2a: + a exposicao anterior, para desfazer a pre-exposicao
    expo_luma_info.num_uniform_buffers = 1;
    SDL_GPUShader* expo_luma_ps = SDL_CreateGPUShader(device, &expo_luma_info);
    if (!expo_luma_ps) { SDL_Log("FALHA expo_luma_ps: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo expo_adapt_info = {};
    expo_adapt_info.code = g_expo_adapt_ps;
    expo_adapt_info.code_size = sizeof(g_expo_adapt_ps);
    expo_adapt_info.entrypoint = "PS_Adapt";
    expo_adapt_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    expo_adapt_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    expo_adapt_info.num_samplers = 2;
    expo_adapt_info.num_uniform_buffers = 1;
    SDL_GPUShader* expo_adapt_ps = SDL_CreateGPUShader(device, &expo_adapt_info);
    if (!expo_adapt_ps) { SDL_Log("FALHA expo_adapt_ps: %s", SDL_GetError()); return 1; }

    // M2.5-26-14: shaders do operador local (Patry, SIGGRAPH 2021). Reaproveitam o
    // vertex shader da exposicao - e o mesmo triangulo de tela cheia.
    SDL_GPUShaderCreateInfo lt_down_info = {};
    lt_down_info.code = g_lt_down_ps;
    lt_down_info.code_size = sizeof(g_lt_down_ps);
    lt_down_info.entrypoint = "PS_Down";
    lt_down_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    lt_down_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    lt_down_info.num_samplers = 2; // 26-15: cena + bloom
    lt_down_info.num_uniform_buffers = 1;
    SDL_GPUShader* lt_down_ps = SDL_CreateGPUShader(device, &lt_down_info);
    if (!lt_down_ps) { SDL_Log("FALHA lt_down_ps: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo lt_blur_info = {};
    lt_blur_info.code = g_lt_blur_ps;
    lt_blur_info.code_size = sizeof(g_lt_blur_ps);
    lt_blur_info.entrypoint = "PS_Blur";
    lt_blur_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    lt_blur_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    lt_blur_info.num_samplers = 1;
    lt_blur_info.num_uniform_buffers = 1;
    SDL_GPUShader* lt_blur_ps = SDL_CreateGPUShader(device, &lt_blur_info);
    if (!lt_blur_ps) { SDL_Log("FALHA lt_blur_ps: %s", SDL_GetError()); return 1; }

    // M2.5-26-16: o bilateral. Le a log-luminancia crua (t0) e o B gaussiano (t1).
    SDL_GPUShaderCreateInfo lt_bilat_info = {};
    lt_bilat_info.code = g_lt_bilat_ps;
    lt_bilat_info.code_size = sizeof(g_lt_bilat_ps);
    lt_bilat_info.entrypoint = "PS_Bilat";
    lt_bilat_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    lt_bilat_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    lt_bilat_info.num_samplers = 2;
    lt_bilat_info.num_uniform_buffers = 1;
    SDL_GPUShader* lt_bilat_ps = SDL_CreateGPUShader(device, &lt_bilat_info);
    if (!lt_bilat_ps) { SDL_Log("FALHA lt_bilat_ps: %s", SDL_GetError()); return 1; }

    // M2.5-26-23a: a construcao da grade. TRES samplers: a cena pre-exposta (t0),
    // o bloom (t1) e a exposicao 1x1 (t2, para o ponto medio M).
    SDL_GPUShaderCreateInfo lt_grid_info = {};
    lt_grid_info.code = g_lt_grid_ps;
    lt_grid_info.code_size = sizeof(g_lt_grid_ps);
    lt_grid_info.entrypoint = "PS_Grid";
    lt_grid_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    lt_grid_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    lt_grid_info.num_samplers = 3;
    lt_grid_info.num_uniform_buffers = 1;
    SDL_GPUShader* lt_grid_ps = SDL_CreateGPUShader(device, &lt_grid_info);
    if (!lt_grid_ps) { SDL_Log("FALHA lt_grid_ps: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo shadow_vs_info = {};
    shadow_vs_info.code = g_shadow_vs;
    shadow_vs_info.code_size = sizeof(g_shadow_vs);
    shadow_vs_info.entrypoint = "VSMain";
    shadow_vs_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    shadow_vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    shadow_vs_info.num_uniform_buffers = 1;
    SDL_GPUShader* shadow_vs = SDL_CreateGPUShader(device, &shadow_vs_info);
    if (!shadow_vs) { SDL_Log("FALHA shadow_vs: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo shadow_ps_info = {};
    shadow_ps_info.code = g_shadow_ps;
    shadow_ps_info.code_size = sizeof(g_shadow_ps);
    shadow_ps_info.entrypoint = "PSMain";
    shadow_ps_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    shadow_ps_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    shadow_ps_info.num_samplers = 0;
    shadow_ps_info.num_uniform_buffers = 0;
    SDL_GPUShader* shadow_ps = SDL_CreateGPUShader(device, &shadow_ps_info);
    if (!shadow_ps) { SDL_Log("FALHA shadow_ps_dummy: %s", SDL_GetError()); return 1; }

    core::JobSystem js;

    sim::World world;
    sim::init_world(world, 42, 100000);

    phys::phys_init();

    phys::phys_create_static_box(300.0f, -0.5f, 0.0f, 15.0f, 0.5f, 15.0f);
    phys::phys_create_static_box(295.0f, 1.0f, 5.0f, 5.0f, 1.0f, 0.5f, 0.785398f);
    phys::phys_create_static_box(305.0f, 1.0f, 5.0f, 5.0f, 1.0f, 0.5f, -0.785398f);
    phys::phys_create_static_box(300.0f, 2.5f, 12.0f, 5.0f, 2.5f, 0.5f);
    phys::phys_create_static_box(300.0f, 0.0f, -10.0f, 2.0f, 0.1f, 3.0f, 0.0f, 0.785398f);
    phys::phys_create_static_box(305.0f, 1.0f, -13.75f, 2.0f, 0.125f, 2.236f, 0.0f, 0.4636f);

    uint32_t plat_id = phys::phys_create_kinematic_box(0.0f, 0.0f, 0.0f, 2.0f, 0.25f, 2.0f);
    phys::phys_create_character(0.0f, 2.0f, 0.0f);

    SDL_GPUTextureCreateInfo fallback_tex_info = {};
    fallback_tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    fallback_tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB; // 25-U6b: COR -> o hardware decodifica sRGB
    fallback_tex_info.width = 1;
    fallback_tex_info.height = 1;
    fallback_tex_info.layer_count_or_depth = 1;
    fallback_tex_info.num_levels = 1;
    fallback_tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* fallback_tex = SDL_CreateGPUTexture(device, &fallback_tex_info);

    uint8_t white_pixel[4] = { 255, 255, 255, 255 };
    SDL_GPUTransferBufferCreateInfo fb_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 4 };
    SDL_GPUTransferBuffer* fallback_tb = SDL_CreateGPUTransferBuffer(device, &fb_tb_info);
    void* fb_map = SDL_MapGPUTransferBuffer(device, fallback_tb, false);
    memcpy(fb_map, white_pixel, 4);
    SDL_UnmapGPUTransferBuffer(device, fallback_tb);

    SDL_GPUSamplerCreateInfo samp_info = {};
    samp_info.min_filter = SDL_GPU_FILTER_LINEAR;
    samp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device, &samp_info);

    SDL_GPUSamplerCreateInfo shadow_diag_info = {};
    shadow_diag_info.min_filter = SDL_GPU_FILTER_NEAREST;
    shadow_diag_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    shadow_diag_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    shadow_diag_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadow_diag_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadow_diag_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* shadow_diag = SDL_CreateGPUSampler(device, &shadow_diag_info);
    if (!shadow_diag) { SDL_Log("FALHA shadow_diag: %s", SDL_GetError()); return 1; }

    SDL_GPUSamplerCreateInfo shadow_cmp_info = shadow_diag_info;
    shadow_cmp_info.min_filter = SDL_GPU_FILTER_LINEAR;
    shadow_cmp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    shadow_cmp_info.enable_compare = true;
    shadow_cmp_info.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    SDL_GPUSampler* shadow_cmp = SDL_CreateGPUSampler(device, &shadow_cmp_info);
    if (!shadow_cmp) { SDL_Log("FALHA shadow_cmp: %s", SDL_GetError()); return 1; }

    SDL_GPUTextureCreateInfo shadow_tex_info = {};
    shadow_tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    shadow_tex_info.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    shadow_tex_info.width = 2048;
    shadow_tex_info.height = 2048;
    shadow_tex_info.layer_count_or_depth = 1;
    shadow_tex_info.num_levels = 1;
    shadow_tex_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
#ifdef SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_DEPTH_FLOAT
    SDL_PropertiesID sm_props = SDL_CreateProperties();
    SDL_SetFloatProperty(sm_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_DEPTH_FLOAT, 1.0f);
    shadow_tex_info.props = sm_props;
#endif
    SDL_GPUTexture* shadow_map = SDL_CreateGPUTexture(device, &shadow_tex_info);
#ifdef SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_DEPTH_FLOAT
    SDL_DestroyProperties(sm_props);
#endif
    if (!shadow_map) { SDL_Log("FALHA shadow_map_tex: %s", SDL_GetError()); return 1; }

    SDL_GPUCommandBuffer* init_cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* init_pass = SDL_BeginGPUCopyPass(init_cmd);
    SDL_GPUTextureTransferInfo fb_tex_loc = { fallback_tb, 0, 0, 0 };
    SDL_GPUTextureRegion fb_tex_reg = { fallback_tex, 0, 0, 0, 0, 0, 1, 1, 1 };
    SDL_UploadToGPUTexture(init_pass, &fb_tex_loc, &fb_tex_reg, false);

    SDL_GPUTextureCreateInfo cloud_tex_info = {};
    cloud_tex_info.type = SDL_GPU_TEXTURETYPE_3D;
    cloud_tex_info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    cloud_tex_info.width = 128;
    cloud_tex_info.height = 32;
    cloud_tex_info.layer_count_or_depth = 128;
    cloud_tex_info.num_levels = 1;
    cloud_tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* cloud_tex = SDL_CreateGPUTexture(device, &cloud_tex_info);

    // M2.5-24C: Textura de Detalhe Swiss Cheese
    SDL_GPUTextureCreateInfo detail_tex_info = {};
    detail_tex_info.type = SDL_GPU_TEXTURETYPE_3D;
    detail_tex_info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    detail_tex_info.width = 32;
    detail_tex_info.height = 32;
    detail_tex_info.layer_count_or_depth = 32;
    detail_tex_info.num_levels = 1;
    detail_tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* cloud_detail_tex = SDL_CreateGPUTexture(device, &detail_tex_info);

    // M2.5-24G-1: Weather Map 512x512 RGBA (R=coverage, G=tipo regional)
    SDL_GPUTextureCreateInfo weather_tex_info = {};
    weather_tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    weather_tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    weather_tex_info.width = 512;
    weather_tex_info.height = 512;
    weather_tex_info.layer_count_or_depth = 1;
    weather_tex_info.num_levels = 1;
    weather_tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* weather_tex = SDL_CreateGPUTexture(device, &weather_tex_info);
    if (!weather_tex) { SDL_Log("FALHA weather_tex: %s", SDL_GetError()); return 1; }
    // M2.5-25-2: LUTs da atmosfera (Hillaire 2020, Tabela 2). RGBA16F: a
    // transmitancia e o Psi_ms tem faixa dinamica que UNORM8 destruiria.
    SDL_GPUTextureCreateInfo trans_tex_info = {};
    trans_tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    trans_tex_info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    trans_tex_info.width = atmo::kTransW;
    trans_tex_info.height = atmo::kTransH;
    trans_tex_info.layer_count_or_depth = 1;
    trans_tex_info.num_levels = 1;
    trans_tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* trans_tex = SDL_CreateGPUTexture(device, &trans_tex_info);
    if (!trans_tex) { SDL_Log("FALHA trans_tex: %s", SDL_GetError()); return 1; }

    SDL_GPUTextureCreateInfo ms_tex_info = trans_tex_info;
    ms_tex_info.width = atmo::kMsW;
    ms_tex_info.height = atmo::kMsH;
    SDL_GPUTexture* ms_tex = SDL_CreateGPUTexture(device, &ms_tex_info);
    if (!ms_tex) { SDL_Log("FALHA ms_tex: %s", SDL_GetError()); return 1; }

    SDL_GPUSamplerCreateInfo cloud_samp_info = {};
    cloud_samp_info.min_filter = SDL_GPU_FILTER_LINEAR;
    cloud_samp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    cloud_samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    cloud_samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    cloud_samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    cloud_samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    SDL_GPUSampler* cloud_sampler = SDL_CreateGPUSampler(device, &cloud_samp_info);
    if (!cloud_sampler) { SDL_Log("FALHA cloud_sampler: %s", SDL_GetError()); return 1; }
    // M2.5-25-2: sampler das LUTs da atmosfera - CLAMP obrigatorio (LUT nao repete)
    SDL_GPUSamplerCreateInfo lut_samp_info = {};
    lut_samp_info.min_filter = SDL_GPU_FILTER_LINEAR;
    lut_samp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    lut_samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    lut_samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    lut_samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    lut_samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* lut_sampler = SDL_CreateGPUSampler(device, &lut_samp_info);
    if (!lut_sampler) { SDL_Log("FALHA lut_sampler: %s", SDL_GetError()); return 1; }

    std::vector<uint8_t> cloud_data = clouds::generate_cloud_noise(128, 32, 128);
    SDL_GPUTransferBufferCreateInfo c_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (uint32_t)cloud_data.size() };
    SDL_GPUTransferBuffer* cloud_tb = SDL_CreateGPUTransferBuffer(device, &c_tb_info);
    void* cloud_map = SDL_MapGPUTransferBuffer(device, cloud_tb, false);
    memcpy(cloud_map, cloud_data.data(), cloud_data.size());
    SDL_UnmapGPUTransferBuffer(device, cloud_tb);

    std::vector<uint8_t> det_data = clouds::generate_cloud_detail(32, 32, 32);
    SDL_GPUTransferBufferCreateInfo det_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (uint32_t)det_data.size() };
    SDL_GPUTransferBuffer* det_tb = SDL_CreateGPUTransferBuffer(device, &det_tb_info);
    void* det_map = SDL_MapGPUTransferBuffer(device, det_tb, false);
    memcpy(det_map, det_data.data(), det_data.size());
    SDL_UnmapGPUTransferBuffer(device, det_tb);

    SDL_GPUTextureTransferInfo cloud_tex_loc = { cloud_tb, 0, 128, 32 };
    SDL_GPUTextureRegion cloud_tex_reg = { cloud_tex, 0, 0, 0, 0, 0, 128, 32, 128 };
    SDL_UploadToGPUTexture(init_pass, &cloud_tex_loc, &cloud_tex_reg, false);

    SDL_GPUTextureTransferInfo det_tex_loc = { det_tb, 0, 32, 32 };
    SDL_GPUTextureRegion det_tex_reg = { cloud_detail_tex, 0, 0, 0, 0, 0, 32, 32, 32 };
    SDL_UploadToGPUTexture(init_pass, &det_tex_loc, &det_tex_reg, false);

    std::vector<uint8_t> weather_data = clouds::generate_weather_map(512, 512);
    SDL_GPUTransferBufferCreateInfo w_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (uint32_t)weather_data.size() };
    SDL_GPUTransferBuffer* weather_tb = SDL_CreateGPUTransferBuffer(device, &w_tb_info);
    void* weather_map_ptr = SDL_MapGPUTransferBuffer(device, weather_tb, false);
    memcpy(weather_map_ptr, weather_data.data(), weather_data.size());
    SDL_UnmapGPUTransferBuffer(device, weather_tb);
    SDL_GPUTextureTransferInfo w_tex_loc = { weather_tb, 0, 512, 512 };
    SDL_GPUTextureRegion w_tex_reg = { weather_tex, 0, 0, 0, 0, 0, 512, 512, 1 };
    SDL_UploadToGPUTexture(init_pass, &w_tex_loc, &w_tex_reg, false);
    // M2.5-25-2: assa (ou le do cache) as LUTs da atmosfera e sobe para a GPU
    std::vector<uint8_t> trans_data = atmo::generate_transmittance_lut();
    SDL_GPUTransferBufferCreateInfo tr_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (uint32_t)trans_data.size() };
    SDL_GPUTransferBuffer* trans_tb = SDL_CreateGPUTransferBuffer(device, &tr_tb_info);
    void* trans_ptr = SDL_MapGPUTransferBuffer(device, trans_tb, false);
    memcpy(trans_ptr, trans_data.data(), trans_data.size());
    SDL_UnmapGPUTransferBuffer(device, trans_tb);
    SDL_GPUTextureTransferInfo tr_tex_loc = { trans_tb, 0, atmo::kTransW, atmo::kTransH };
    SDL_GPUTextureRegion tr_tex_reg = { trans_tex, 0, 0, 0, 0, 0, atmo::kTransW, atmo::kTransH, 1 };
    SDL_UploadToGPUTexture(init_pass, &tr_tex_loc, &tr_tex_reg, false);

    std::vector<uint8_t> ms_data = atmo::generate_multiscatter_lut();
    SDL_GPUTransferBufferCreateInfo ms_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (uint32_t)ms_data.size() };
    SDL_GPUTransferBuffer* ms_tb = SDL_CreateGPUTransferBuffer(device, &ms_tb_info);
    void* ms_ptr = SDL_MapGPUTransferBuffer(device, ms_tb, false);
    memcpy(ms_ptr, ms_data.data(), ms_data.size());
    SDL_UnmapGPUTransferBuffer(device, ms_tb);
    SDL_GPUTextureTransferInfo ms_tex_loc = { ms_tb, 0, atmo::kMsW, atmo::kMsH };
    SDL_GPUTextureRegion ms_tex_reg = { ms_tex, 0, 0, 0, 0, 0, atmo::kMsW, atmo::kMsH, 1 };
    SDL_UploadToGPUTexture(init_pass, &ms_tex_loc, &ms_tex_reg, false);

    SDL_EndGPUCopyPass(init_pass);
    SDL_SubmitGPUCommandBuffer(init_cmd);
    SDL_ReleaseGPUTransferBuffer(device, fallback_tb);
    SDL_ReleaseGPUTransferBuffer(device, cloud_tb);
    SDL_ReleaseGPUTransferBuffer(device, det_tb);
    SDL_ReleaseGPUTransferBuffer(device, weather_tb);
    SDL_ReleaseGPUTransferBuffer(device, trans_tb);
    SDL_ReleaseGPUTransferBuffer(device, ms_tb);
    SDL_Log("BOOT: nuvens ok, seguindo p/ ceu");

    std::vector<float> dome_vertices;
    std::vector<uint16_t> dome_indices;
    int d_seg = 24;
    int d_rings = 12;
    for (int r = 0; r <= d_rings; ++r) {
        float v = (float)r / (float)d_rings;
        float phi = v * 3.14159265f;
        for (int s = 0; s <= d_seg; ++s) {
            float u = (float)s / (float)d_seg;
            float theta = u * 2.0f * 3.14159265f;
            float x = std::cos(theta) * std::sin(phi);
            float y = std::cos(phi);
            float z = std::sin(theta) * std::sin(phi);
            dome_vertices.push_back(x);
            dome_vertices.push_back(y);
            dome_vertices.push_back(z);
        }
    }
    for (int r = 0; r < d_rings; ++r) {
        for (int s = 0; s < d_seg; ++s) {
            uint16_t i0 = r * (d_seg + 1) + s;
            uint16_t i1 = i0 + 1;
            uint16_t i2 = i0 + (d_seg + 1);
            uint16_t i3 = i2 + 1;
            dome_indices.push_back(i0);
            dome_indices.push_back(i2);
            dome_indices.push_back(i1);
            dome_indices.push_back(i1);
            dome_indices.push_back(i2);
            dome_indices.push_back(i3);
        }
    }

    uint32_t dome_vb_size = (uint32_t)(dome_vertices.size() * sizeof(float));
    uint32_t dome_ib_size = (uint32_t)(dome_indices.size() * sizeof(uint16_t));

    SDL_GPUTransferBufferCreateInfo d_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, dome_vb_size + dome_ib_size };
    SDL_GPUTransferBuffer* dome_tb = SDL_CreateGPUTransferBuffer(device, &d_tb_info);

    void* dome_map = SDL_MapGPUTransferBuffer(device, dome_tb, false);
    memcpy(dome_map, dome_vertices.data(), dome_vb_size);
    memcpy((uint8_t*)dome_map + dome_vb_size, dome_indices.data(), dome_ib_size);
    SDL_UnmapGPUTransferBuffer(device, dome_tb);

    SDL_GPUBufferCreateInfo dv_info = { SDL_GPU_BUFFERUSAGE_VERTEX, dome_vb_size };
    SDL_GPUBuffer* dome_vertex_buffer = SDL_CreateGPUBuffer(device, &dv_info);

    SDL_GPUBufferCreateInfo di_info = { SDL_GPU_BUFFERUSAGE_INDEX, dome_ib_size };
    SDL_GPUBuffer* dome_index_buffer = SDL_CreateGPUBuffer(device, &di_info);

    SDL_GPUCommandBuffer* d_cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* d_pass = SDL_BeginGPUCopyPass(d_cmd);

    SDL_GPUTransferBufferLocation dv_loc = { dome_tb, 0 };
    SDL_GPUBufferRegion dv_reg = { dome_vertex_buffer, 0, dome_vb_size };
    SDL_UploadToGPUBuffer(d_pass, &dv_loc, &dv_reg, false);

    SDL_GPUTransferBufferLocation di_loc = { dome_tb, dome_vb_size };
    SDL_GPUBufferRegion di_reg = { dome_index_buffer, 0, dome_ib_size };
    SDL_UploadToGPUBuffer(d_pass, &di_loc, &di_reg, false);

    SDL_EndGPUCopyPass(d_pass);
    SDL_SubmitGPUCommandBuffer(d_cmd);
    SDL_ReleaseGPUTransferBuffer(device, dome_tb);


    // M2.5-26-18b: as tres viraram GLOBAIS (topo do arquivo) para poderem ser
    // salvas no tuning.ini - save/load sao funcoes livres e nao enxergam locais
    // de main(). Sem isso a escolha de resolucao nao sobrevive a um restart, que
    // e metade do valor de ter o dial.
    bool resize_needed = false;

    int internal_w, internal_h;
    if (pixel_mode == 2) {                 // M2.5-26-18: NATIVO
        internal_w = std::max(2, win_w) & ~1;
        internal_h = std::max(1, win_h);
    }
    else if (pixel_mode == 0) {
        internal_w = std::max(1, win_w / pixel_divisor);
        internal_h = std::max(1, win_h / pixel_divisor);
    }
    else {
        internal_h = alturas[altura_idx];
        internal_w = std::max(2, (int)std::llround(internal_h * (double)win_w / (double)win_h)) & ~1;
    }

    SDL_GPUTextureCreateInfo scene_color_info = {};
    scene_color_info.type = SDL_GPU_TEXTURETYPE_2D;
    scene_color_info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // M2.5-24I: HDR
    scene_color_info.width = (uint32_t)internal_w;
    scene_color_info.height = (uint32_t)internal_h;
    scene_color_info.layer_count_or_depth = 1;
    scene_color_info.num_levels = 1;
    scene_color_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    scene_color_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* scene_color = SDL_CreateGPUTexture(device, &scene_color_info);

    SDL_GPUTextureCreateInfo normal_id_info = {};
    normal_id_info.type = SDL_GPU_TEXTURETYPE_2D;
    normal_id_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    normal_id_info.width = (uint32_t)internal_w;
    normal_id_info.height = (uint32_t)internal_h;
    normal_id_info.layer_count_or_depth = 1;
    normal_id_info.num_levels = 1;
    normal_id_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    normal_id_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* normal_id = SDL_CreateGPUTexture(device, &normal_id_info);

    SDL_GPUTextureCreateInfo comp_color_info = {};
    comp_color_info.type = SDL_GPU_TEXTURETYPE_2D;
    comp_color_info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // M2.5-24I: HDR
    comp_color_info.width = (uint32_t)internal_w;
    comp_color_info.height = (uint32_t)internal_h;
    comp_color_info.layer_count_or_depth = 1;
    comp_color_info.num_levels = 1;
    comp_color_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    comp_color_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* composite_color = SDL_CreateGPUTexture(device, &comp_color_info);

    SDL_GPUTextureCreateInfo depth_info = {};
    depth_info.type = SDL_GPU_TEXTURETYPE_2D;
    depth_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    depth_info.width = (uint32_t)internal_w;
    depth_info.height = (uint32_t)internal_h;
    depth_info.layer_count_or_depth = 1;
    depth_info.num_levels = 1;
    depth_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    depth_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* depth_texture = SDL_CreateGPUTexture(device, &depth_info);

    SDL_GPUSamplerCreateInfo upscale_samp_info = {};
    upscale_samp_info.min_filter = SDL_GPU_FILTER_LINEAR;
    upscale_samp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    upscale_samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    upscale_samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    upscale_samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    upscale_samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* upscale_sampler = SDL_CreateGPUSampler(device, &upscale_samp_info);

    SDL_GPUSamplerCreateInfo point_samp_info = {};
    point_samp_info.min_filter = SDL_GPU_FILTER_NEAREST;
    point_samp_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    point_samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    point_samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    point_samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    point_samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* point_sampler = SDL_CreateGPUSampler(device, &point_samp_info);

    std::vector<Vertex> g_verts;
    std::vector<uint16_t> g_inds;

    auto add_g_quad = [&](float x0, float y0, float z0, float u0, float v0,
        float x1, float y1, float z1, float u1, float v1,
        float x2, float y2, float z2, float u2, float v2,
        float x3, float y3, float z3, float u3, float v3) {
            uint16_t b = (uint16_t)g_verts.size();
            g_verts.push_back({ x0,y0,z0, 1,1,1, u0,v0, 0,1,0 });
            g_verts.push_back({ x1,y1,z1, 1,1,1, u1,v1, 0,1,0 });
            g_verts.push_back({ x2,y2,z2, 1,1,1, u2,v2, 0,1,0 });
            g_verts.push_back({ x3,y3,z3, 1,1,1, u3,v3, 0,1,0 });
            g_inds.push_back(b); g_inds.push_back(b + 1); g_inds.push_back(b + 2);
            g_inds.push_back(b); g_inds.push_back(b + 2); g_inds.push_back(b + 3);
        };

    uint32_t g_start = (uint32_t)g_inds.size();
    for (int i = 0; i < 3; ++i) {
        float ang = i * 2.09439f;
        float s = sin(ang), c = cos(ang);
        auto rot = [&](float x, float z, float& ox, float& oz) { ox = x * c - z * s; oz = x * s + z * c; };
        float w1 = 0.075f, w2 = 0.05f;
        float xbl, zbl, xbr, zbr, xml, zml, xmr, zmr, xt, zt;
        rot(-w1, 0, xbl, zbl); rot(w1, 0, xbr, zbr);
        rot(-w2, 0.15f, xml, zml); rot(w2, 0.15f, xmr, zmr);
        rot(0, 0.4f, xt, zt);

        add_g_quad(xml, 0.5f, zml, 0, 0.5f, xmr, 0.5f, zmr, 1, 0.5f, xbr, 0.0f, zbr, 1, 0.0f, xbl, 0.0f, zbl, 0, 0.0f);
        add_g_quad(xt, 1.0f, zt, 0.5f, 1.0f, xt, 1.0f, zt, 0.5f, 1.0f, xmr, 0.5f, zmr, 1, 0.5f, xml, 0.5f, zml, 0, 0.5f);
    }
    uint32_t g_count = (uint32_t)g_inds.size() - g_start;

    uint32_t gvb_sz = (uint32_t)(g_verts.size() * sizeof(Vertex));
    uint32_t gib_sz = (uint32_t)(g_inds.size() * sizeof(uint16_t));

    SDL_GPUTransferBufferCreateInfo g_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, gvb_sz + gib_sz };
    SDL_GPUTransferBuffer* g_tb = SDL_CreateGPUTransferBuffer(device, &g_tb_info);
    void* g_map = SDL_MapGPUTransferBuffer(device, g_tb, false);
    memcpy(g_map, g_verts.data(), gvb_sz);
    memcpy((uint8_t*)g_map + gvb_sz, g_inds.data(), gib_sz);
    SDL_UnmapGPUTransferBuffer(device, g_tb);

    SDL_GPUBufferCreateInfo g_vinfo = { SDL_GPU_BUFFERUSAGE_VERTEX, gvb_sz };
    SDL_GPUBuffer* grass_vbuf = SDL_CreateGPUBuffer(device, &g_vinfo);
    SDL_GPUBufferCreateInfo g_iinfo = { SDL_GPU_BUFFERUSAGE_INDEX, gib_sz };
    SDL_GPUBuffer* grass_ibuf = SDL_CreateGPUBuffer(device, &g_iinfo);

    SDL_GPUCommandBuffer* g_cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* g_pass = SDL_BeginGPUCopyPass(g_cmd);
    SDL_GPUTransferBufferLocation gl1 = { g_tb, 0 }, gl2 = { g_tb, gvb_sz };
    SDL_GPUBufferRegion gr1 = { grass_vbuf, 0, gvb_sz }, gr2 = { grass_ibuf, 0, gib_sz };
    SDL_UploadToGPUBuffer(g_pass, &gl1, &gr1, false);
    SDL_UploadToGPUBuffer(g_pass, &gl2, &gr2, false);
    SDL_EndGPUCopyPass(g_pass);
    SDL_SubmitGPUCommandBuffer(g_cmd);
    SDL_ReleaseGPUTransferBuffer(device, g_tb);

    AppLog app_log;
    app_log.add("Res interna inicial: %dx%d", internal_w, internal_h);
    app_log.add("SkyParams alinhado: %zu bytes", sizeof(SkyParams));
    app_log.add("SunParams alinhado: %zu bytes", sizeof(SunParams));

    // M2.5-21: Leitura silenciosa dos parametros antes da simulacao
    load_tuning();
    // M2.5-26-18b: o load roda DEPOIS de a resolucao ter sido calculada e das
    // texturas criadas (a ordem do arquivo). Sem forcar o resize, a resolucao
    // vinda do ini so valeria apos um redimensionamento manual da janela.
    resize_needed = true;

    social::Relations rels;
    {
        std::ifstream f(std::string(SOCIAL_DIR) + "relacoes.txt");
        if (f) {
            std::stringstream buffer; buffer << f.rdbuf();
            rels = social::parse_relations(buffer.str().c_str());
        }
    }
    social::Barks barks;
    {
        std::ifstream f(std::string(SOCIAL_DIR) + "barks.txt");
        if (f) {
            std::stringstream buffer; buffer << f.rdbuf();
            barks = social::parse_barks(buffer.str().c_str());
        }
    }
    app_log.add("Carregado %zu relacoes e barks (A:%zu N:%zu F:%zu)", rels.entries.size(), barks.amistoso.size(), barks.neutro.size(), barks.frio.size());

    MeshRange nivel_range, tortura_range, cubo_multidao, cubo_jogador, cubo_jogador_agachado, cubo_plataforma, cubo_npc;
    size_t level_box_count = 0;
    std::vector<uint32_t> level_body_ids;
    std::vector<LoadedAsset> level_assets;
    std::vector<MeshRange> box_ranges;

    NpcVisual npc_visuals[5] = {};

    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUBuffer* index_buffer = nullptr;

    glm::vec3 current_player_pos(0.0f);
    glm::vec3 prev_player_pos(0.0f);
    phys::PhysState active_phys_state = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false };

    SDL_GPUTexture* active_grass_tex = nullptr;

    auto replant_grass = [&]() {
        if (grass_instbuf) {
            SDL_WaitForGPUIdle(device);
            SDL_ReleaseGPUBuffer(device, grass_instbuf);
            grass_instbuf = nullptr;
        }
        grass_inst_count = 0;

        if (terrain_indices.empty()) return;

        std::vector<InstanceData> g_insts;
        const uint32_t MAX_GRASS = 120000;
        // Pre-escala justa: se o pedido total estoura o cap, TODOS afinam por igual (fim da fome por ordem)
        float fair_total = 0.0f;
        for (size_t ft = 0; ft < terrain_indices.size() / 3; ++ft) {
            uint16_t fi0 = terrain_indices[ft * 3 + 0], fi1 = terrain_indices[ft * 3 + 1], fi2 = terrain_indices[ft * 3 + 2];
            glm::vec3 fv0(terrain_phys_verts[fi0 * 6 + 0], terrain_phys_verts[fi0 * 6 + 1], terrain_phys_verts[fi0 * 6 + 2]);
            glm::vec3 fv1(terrain_phys_verts[fi1 * 6 + 0], terrain_phys_verts[fi1 * 6 + 1], terrain_phys_verts[fi1 * 6 + 2]);
            glm::vec3 fv2(terrain_phys_verts[fi2 * 6 + 0], terrain_phys_verts[fi2 * 6 + 1], terrain_phys_verts[fi2 * 6 + 2]);
            glm::vec3 fe1 = fv1 - fv0, fe2 = fv2 - fv0;
            glm::vec3 fn = glm::normalize(glm::cross(fe1, fe2));
            if (fn.y < grass_slope_min) continue;
            fair_total += 0.5f * glm::length(glm::cross(fe1, fe2)) * grass_density;
        }
        float fair_scale = (fair_total > (float)MAX_GRASS) ? (float)MAX_GRASS / fair_total : 1.0f;

        auto hash_float = [](uint32_t seed) {
            seed = (seed ^ 61) ^ (seed >> 16);
            seed *= 9;
            seed = seed ^ (seed >> 4);
            seed *= 0x27d4eb2d;
            seed = seed ^ (seed >> 15);
            return (float)seed / 4294967296.0f;
            };

        size_t num_tris = terrain_indices.size() / 3;
        for (size_t t = 0; t < num_tris; ++t) {
            if (g_insts.size() >= MAX_GRASS) break;

            uint32_t i0 = terrain_indices[t * 3 + 0];
            uint32_t i1 = terrain_indices[t * 3 + 1];
            uint32_t i2 = terrain_indices[t * 3 + 2];

            glm::vec3 v0(terrain_phys_verts[i0 * 6 + 0], terrain_phys_verts[i0 * 6 + 1], terrain_phys_verts[i0 * 6 + 2]);
            glm::vec3 v1(terrain_phys_verts[i1 * 6 + 0], terrain_phys_verts[i1 * 6 + 1], terrain_phys_verts[i1 * 6 + 2]);
            glm::vec3 v2(terrain_phys_verts[i2 * 6 + 0], terrain_phys_verts[i2 * 6 + 1], terrain_phys_verts[i2 * 6 + 2]);

            glm::vec3 e1 = v1 - v0;
            glm::vec3 e2 = v2 - v0;
            glm::vec3 normal = glm::normalize(glm::cross(e1, e2));

            if (normal.y < grass_slope_min) continue;

            glm::vec3 c0(terrain_phys_verts[i0 * 6 + 3], terrain_phys_verts[i0 * 6 + 4], terrain_phys_verts[i0 * 6 + 5]);
            glm::vec3 c1(terrain_phys_verts[i1 * 6 + 3], terrain_phys_verts[i1 * 6 + 4], terrain_phys_verts[i1 * 6 + 5]);
            glm::vec3 c2(terrain_phys_verts[i2 * 6 + 3], terrain_phys_verts[i2 * 6 + 4], terrain_phys_verts[i2 * 6 + 5]);

            // Area do triangulo governa a quantidade BRUTA de tufos (sem filtro de verdice)
            float area = 0.5f * glm::length(glm::cross(e1, e2));
            float expected_tufts = area * grass_density * fair_scale;

            uint32_t base_tufts = (uint32_t)expected_tufts;
            float frac_tufts = expected_tufts - base_tufts;

            uint32_t tuft_count = base_tufts;
            if (hash_float((uint32_t)t * 1337) < frac_tufts) tuft_count++;

            for (uint32_t tufo_idx = 0; tufo_idx < tuft_count; ++tufo_idx) {
                if (g_insts.size() >= MAX_GRASS) break;

                float r1 = hash_float((uint32_t)t * 73856093 ^ tufo_idx * 19349663);
                float r2 = hash_float((uint32_t)t * 83492791 ^ tufo_idx * 39916801);

                float sqrt_r1 = std::sqrt(r1);
                float u = 1.0f - sqrt_r1;
                float v = sqrt_r1 * (1.0f - r2);
                float w = 1.0f - u - v;

                glm::vec3 inst_color = u * c0 + v * c1 + w * c2;

                // M2.5-18: Porteiro Baricêntrico e Hashed Alpha (Dureza da Borda)
                float verdice_ponto = std::clamp(inst_color.g - std::max(inst_color.r, inst_color.b), 0.0f, 1.0f);
                float threshold = std::clamp((verdice_ponto - grass_verdice_min) * grass_border_sharp, 0.0f, 1.0f);
                float brota_hash = hash_float((uint32_t)t * 45293047 ^ tufo_idx * 22468225);

                if (brota_hash >= threshold) continue;

                glm::vec3 pos = u * v0 + v * v1 + w * v2;

                glm::vec2 g_pos = glm::vec2(pos.x, pos.z) / grass_clump_cell;
                int gx = (int)std::floor(g_pos.x);
                int gy = (int)std::floor(g_pos.y);

                float min_d = 999.0f;
                glm::vec2 best_c = g_pos;
                float clump_hash = 0.0f;

                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int cx = gx + dx;
                        int cy = gy + dy;

                        uint32_t s1 = (uint32_t)(cx * 73856093 ^ cy * 19349663);
                        s1 = (s1 ^ 61) ^ (s1 >> 16); s1 *= 9; s1 = s1 ^ (s1 >> 4);
                        s1 *= 0x27d4eb2d; s1 = s1 ^ (s1 >> 15);
                        float hx = (float)s1 / 4294967296.0f;

                        uint32_t s2 = (uint32_t)(cx * 83492791 ^ cy * 39916801);
                        s2 = (s2 ^ 61) ^ (s2 >> 16); s2 *= 9; s2 = s2 ^ (s2 >> 4);
                        s2 *= 0x27d4eb2d; s2 = s2 ^ (s2 >> 15);
                        float hy = (float)s2 / 4294967296.0f;

                        glm::vec2 pt = glm::vec2(cx, cy) + glm::vec2(hx, hy);
                        float d = glm::length(g_pos - pt);
                        if (d < min_d) {
                            min_d = d;
                            best_c = pt * grass_clump_cell;
                            clump_hash = hx;
                        }
                    }
                }

                pos.x = glm::mix(pos.x, best_c.x, grass_clump_pull);
                pos.z = glm::mix(pos.z, best_c.y, grass_clump_pull);
                pos.y = v0.y - (normal.x * (pos.x - v0.x) + normal.z * (pos.z - v0.z)) / normal.y;

                float tint_shift = (clump_hash - 0.5f) * 0.15f;
                inst_color.r = std::clamp(inst_color.r + tint_shift, 0.0f, 1.0f);
                inst_color.g = std::clamp(inst_color.g + tint_shift, 0.0f, 1.0f);
                inst_color.b = std::clamp(inst_color.b + tint_shift, 0.0f, 1.0f);

                g_insts.push_back({ pos.x, pos.y, pos.z, inst_color.r, inst_color.g, inst_color.b });
            }
        }

        grass_inst_count = (uint32_t)g_insts.size();
        if (grass_inst_count >= MAX_GRASS) {
            app_log.add("Aviso: Limite MAX_GRASS atingido (%u)", MAX_GRASS);
        }
        app_log.add("Grama: %u tufos plantados", grass_inst_count);

        if (grass_inst_count > 0) {
            uint32_t ginst_sz = grass_inst_count * sizeof(InstanceData);
            SDL_GPUBufferCreateInfo g_instinfo = { SDL_GPU_BUFFERUSAGE_VERTEX, ginst_sz };
            grass_instbuf = SDL_CreateGPUBuffer(device, &g_instinfo);

            SDL_GPUTransferBufferCreateInfo g_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, ginst_sz };
            SDL_GPUTransferBuffer* g_tb = SDL_CreateGPUTransferBuffer(device, &g_tb_info);

            void* g_map = SDL_MapGPUTransferBuffer(device, g_tb, false);
            memcpy(g_map, g_insts.data(), ginst_sz);
            SDL_UnmapGPUTransferBuffer(device, g_tb);

            SDL_GPUCommandBuffer* l_cmd = SDL_AcquireGPUCommandBuffer(device);
            SDL_GPUCopyPass* l_pass = SDL_BeginGPUCopyPass(l_cmd);
            SDL_GPUTransferBufferLocation gl = { g_tb, 0 };
            SDL_GPUBufferRegion gr = { grass_instbuf, 0, ginst_sz };
            SDL_UploadToGPUBuffer(l_pass, &gl, &gr, false);
            SDL_EndGPUCopyPass(l_pass);
            SDL_SubmitGPUCommandBuffer(l_cmd);
            SDL_ReleaseGPUTransferBuffer(device, g_tb);
        }
        };

    auto build_level = [&](const level::LevelData& lvl) {
        SDL_WaitForGPUIdle(device);

        if (grass_instbuf) {
            SDL_ReleaseGPUBuffer(device, grass_instbuf);
            grass_instbuf = nullptr;
        }
        grass_inst_count = 0;

        for (uint32_t id : level_body_ids) phys::phys_destroy_body(id);
        level_body_ids.clear();

        if (vertex_buffer) SDL_ReleaseGPUBuffer(device, vertex_buffer);
        if (index_buffer) SDL_ReleaseGPUBuffer(device, index_buffer);

        for (auto& a : level_assets) {
            if (a.tex) SDL_ReleaseGPUTexture(device, a.tex);
        }
        level_assets.clear();

        for (int i = 0; i < 5; ++i) {
            if (npc_visuals[i].tex) {
                SDL_ReleaseGPUTexture(device, npc_visuals[i].tex);
                npc_visuals[i].tex = nullptr;
            }
            npc_visuals[i].has_mesh = false;
        }

        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;

        auto add_box = [&](float cx, float cy, float cz, float hx, float hy, float hz, float r, float g, float b, float rot_yaw = 0.0f, float rot_pitch = 0.0f) {
            uint32_t base = (uint32_t)vertices.size();
            uint32_t start_idx = (uint32_t)indices.size();

            glm::mat4 mat(1.0f);
            mat = glm::translate(mat, glm::vec3(cx, cy, cz));
            mat = glm::rotate(mat, rot_yaw, glm::vec3(0, 1, 0));
            mat = glm::rotate(mat, rot_pitch, glm::vec3(1, 0, 0));

            glm::mat4 rot_only = glm::mat4(1.0f);
            rot_only = glm::rotate(rot_only, rot_yaw, glm::vec3(0, 1, 0));
            rot_only = glm::rotate(rot_only, rot_pitch, glm::vec3(1, 0, 0));

            mat = glm::scale(mat, glm::vec3(hx, hy, hz));

            const glm::vec3 pos[24] = {
                {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1}, // Front (-z)
                { 1,-1, 1}, {-1,-1, 1}, {-1, 1, 1}, { 1, 1, 1}, // Back (+z)
                {-1,-1, 1}, {-1,-1,-1}, {-1, 1,-1}, {-1, 1, 1}, // Left (-x)
                { 1,-1,-1}, { 1,-1, 1}, { 1, 1, 1}, { 1, 1,-1}, // Right (+x)
                {-1, 1,-1}, { 1, 1,-1}, { 1, 1, 1}, {-1, 1, 1}, // Top (+y)
                {-1,-1, 1}, { 1,-1, 1}, { 1,-1,-1}, {-1,-1,-1}  // Bottom (-y)
            };
            const glm::vec3 norms[24] = {
                {0,0,-1}, {0,0,-1}, {0,0,-1}, {0,0,-1},
                {0,0,1},  {0,0,1},  {0,0,1},  {0,0,1},
                {-1,0,0}, {-1,0,0}, {-1,0,0}, {-1,0,0},
                {1,0,0},  {1,0,0},  {1,0,0},  {1,0,0},
                {0,1,0},  {0,1,0},  {0,1,0},  {0,1,0},
                {0,-1,0}, {0,-1,0}, {0,-1,0}, {0,-1,0}
            };
            const glm::vec2 uvs[4] = { {0,1}, {1,1}, {1,0}, {0,0} };

            for (int i = 0; i < 24; ++i) {
                glm::vec4 p = mat * glm::vec4(pos[i], 1.0f);
                glm::vec4 n = rot_only * glm::vec4(norms[i], 0.0f);
                vertices.push_back({ p.x, p.y, p.z, r, g, b, uvs[i % 4].x, uvs[i % 4].y, n.x, n.y, n.z });
            }

            const uint16_t inds[36] = {
                0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
                12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23
            };
            for (int i = 0; i < 36; ++i) indices.push_back(base + inds[i]);

            return MeshRange{ start_idx, 36 };
            };

        uint32_t lvl_start = indices.size();
        box_ranges.clear();
        for (const auto& b : lvl.boxes) {
            MeshRange r = add_box(b.cx, b.cy, b.cz, b.hx, b.hy, b.hz, b.r, b.g, b.b);
            box_ranges.push_back(r);
            uint32_t bid = phys::phys_create_static_box(b.cx, b.cy, b.cz, b.hx, b.hy, b.hz);
            level_body_ids.push_back(bid);
        }
        nivel_range = { lvl_start, (uint32_t)indices.size() - lvl_start };

        uint32_t tortura_start = indices.size();
        add_box(300.0f, -0.5f, 0.0f, 15.0f, 0.5f, 15.0f, 0.3f, 0.4f, 0.3f);
        add_box(295.0f, 1.0f, 5.0f, 5.0f, 1.0f, 0.5f, 0.4f, 0.3f, 0.3f, 0.785398f);
        add_box(305.0f, 1.0f, 5.0f, 5.0f, 1.0f, 0.5f, 0.4f, 0.3f, 0.3f, -0.785398f);
        add_box(300.0f, 2.5f, 12.0f, 5.0f, 2.5f, 0.5f, 0.3f, 0.3f, 0.4f);
        add_box(300.0f, 0.0f, -10.0f, 2.0f, 0.1f, 3.0f, 0.4f, 0.4f, 0.3f, 0.0f, 0.785398f);
        for (int i = 0; i < 8; i++) {
            add_box(305.0f, 0.125f + i * 0.25f, -12.0f - i * 0.5f, 2.0f, 0.125f, 0.25f, 0.5f, 0.5f, 0.5f);
        }
        tortura_range = { tortura_start, (uint32_t)indices.size() - tortura_start };

        struct PendingTex { SDL_GPUTexture* tex; uint32_t w, h; std::vector<unsigned char> data; };
        std::vector<PendingTex> pending_texs;

        int mesh_idx = 0;
        for (const auto& m : lvl.meshes) {
            std::string full_path = std::string(ASSETS_DIR) + m.path;
            MeshData asset = assets_load_glb(full_path.c_str());
            if (asset.ok) {
                uint32_t base = (uint32_t)vertices.size();
                uint32_t start_idx = (uint32_t)indices.size();

                size_t vert_count = asset.pos_rgb.size() / 6;
                for (size_t i = 0; i < vert_count; ++i) {
                    vertices.push_back({
                        asset.pos_rgb[i * 6 + 0] + m.x,
                        asset.pos_rgb[i * 6 + 1] + m.y,
                        asset.pos_rgb[i * 6 + 2] + m.z,
                        asset.pos_rgb[i * 6 + 3],
                        asset.pos_rgb[i * 6 + 4],
                        asset.pos_rgb[i * 6 + 5],
                        asset.uv[i * 2 + 0],
                        asset.uv[i * 2 + 1],
                        asset.normals[i * 3 + 0],
                        asset.normals[i * 3 + 1],
                        asset.normals[i * 3 + 2]
                        });
                }

                for (size_t i = 0; i < asset.indices.size(); ++i) {
                    indices.push_back(base + asset.indices[i]);
                }

                if (lvl.has_terrain && mesh_idx == lvl.terrain_mesh_index) {
                    std::vector<float> phys_verts;
                    phys_verts.reserve(vert_count * 3);
                    terrain_phys_verts.clear();
                    terrain_phys_verts.reserve(vert_count * 6);

                    for (size_t i = 0; i < vert_count; ++i) {
                        float vx = asset.pos_rgb[i * 6 + 0] + m.x;
                        float vy = asset.pos_rgb[i * 6 + 1] + m.y;
                        float vz = asset.pos_rgb[i * 6 + 2] + m.z;
                        phys_verts.push_back(vx);
                        phys_verts.push_back(vy);
                        phys_verts.push_back(vz);

                        terrain_phys_verts.push_back(vx);
                        terrain_phys_verts.push_back(vy);
                        terrain_phys_verts.push_back(vz);
                        terrain_phys_verts.push_back(asset.pos_rgb[i * 6 + 3]);
                        terrain_phys_verts.push_back(asset.pos_rgb[i * 6 + 4]);
                        terrain_phys_verts.push_back(asset.pos_rgb[i * 6 + 5]);
                    }
                    terrain_indices = asset.indices;

                    uint32_t bid = phys::phys_create_mesh_shape(phys_verts.data(), vert_count, asset.indices.data(), asset.indices.size());
                    level_body_ids.push_back(bid);
                }

                LoadedAsset la;
                la.range = { start_idx, (uint32_t)asset.indices.size() };
                la.tex = nullptr;
                la.is_terrain = (lvl.has_terrain && mesh_idx == lvl.terrain_mesh_index);

                if (asset.tex_w > 0 && asset.tex_h > 0 && !asset.tex_rgba.empty()) {
                    SDL_GPUTextureCreateInfo asset_tex_info = {};
                    asset_tex_info.type = SDL_GPU_TEXTURETYPE_2D;
                    asset_tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB; // 25-U6b: albedo e COR (Frostbite 4.1: "rely on the sRGB convention due to its hardware support"); alfa segue linear
                    asset_tex_info.width = asset.tex_w;
                    asset_tex_info.height = asset.tex_h;
                    asset_tex_info.layer_count_or_depth = 1;
                    asset_tex_info.num_levels = 1;
                    asset_tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
                    la.tex = SDL_CreateGPUTexture(device, &asset_tex_info);

                    pending_texs.push_back({ la.tex, (uint32_t)asset.tex_w, (uint32_t)asset.tex_h, std::move(asset.tex_rgba) });

                    if (m.path.find("grass_sprite") != std::string::npos) {
                        active_grass_tex = la.tex;
                    }
                }

                level_assets.push_back(la);
                app_log.add("Asset: %s (%zu verts, %zu tris)", m.path.c_str(), vert_count, asset.indices.size() / 3);
            }
            else {
                app_log.add("Aviso: Falha ao carregar %s (%s)", m.path.c_str(), asset.error.c_str());
            }
            mesh_idx++;
        }

        for (int i = 0; i < 5; ++i) {
            std::string npc_path = std::string(ASSETS_DIR) + "models/npc" + std::to_string(i) + ".glb";
            MeshData asset = assets_load_glb(npc_path.c_str());

            if (asset.ok) {
                uint32_t base = (uint32_t)vertices.size();
                uint32_t start_idx = (uint32_t)indices.size();

                size_t vert_count = asset.pos_rgb.size() / 6;
                for (size_t v = 0; v < vert_count; ++v) {
                    vertices.push_back({
                        asset.pos_rgb[v * 6 + 0],
                        asset.pos_rgb[v * 6 + 1],
                        asset.pos_rgb[v * 6 + 2],
                        asset.pos_rgb[v * 6 + 3],
                        asset.pos_rgb[v * 6 + 4],
                        asset.pos_rgb[v * 6 + 5],
                        asset.uv[v * 2 + 0],
                        asset.uv[v * 2 + 1],
                        asset.normals[v * 3 + 0],
                        asset.normals[v * 3 + 1],
                        asset.normals[v * 3 + 2]
                        });
                }

                for (size_t idx = 0; idx < asset.indices.size(); ++idx) {
                    indices.push_back(base + asset.indices[idx]);
                }

                npc_visuals[i].range = { start_idx, (uint32_t)asset.indices.size() };
                npc_visuals[i].has_mesh = true;
                npc_visuals[i].tex = nullptr;

                if (asset.tex_w > 0 && asset.tex_h > 0 && !asset.tex_rgba.empty()) {
                    SDL_GPUTextureCreateInfo asset_tex_info = {};
                    asset_tex_info.type = SDL_GPU_TEXTURETYPE_2D;
                    asset_tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB; // 25-U6b: albedo e COR (Frostbite 4.1: "rely on the sRGB convention due to its hardware support"); alfa segue linear
                    asset_tex_info.width = asset.tex_w;
                    asset_tex_info.height = asset.tex_h;
                    asset_tex_info.layer_count_or_depth = 1;
                    asset_tex_info.num_levels = 1;
                    asset_tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
                    npc_visuals[i].tex = SDL_CreateGPUTexture(device, &asset_tex_info);

                    pending_texs.push_back({ npc_visuals[i].tex, (uint32_t)asset.tex_w, (uint32_t)asset.tex_h, std::move(asset.tex_rgba) });
                }

                app_log.add("NPC %d: malha carregada (%zu verts)", i, vert_count);
            }
            else {
                npc_visuals[i].has_mesh = false;
                app_log.add("NPC %d: caixa fallback", i);
            }
        }

        cubo_multidao = add_box(0.0f, 0.15f, 0.0f, 0.15f, 0.15f, 0.15f, 1.0f, 1.0f, 1.0f);
        cubo_jogador = add_box(0.0f, 0.9f, 0.0f, 0.4f, 0.9f, 0.4f, 1.0f, 1.0f, 1.0f);
        cubo_jogador_agachado = add_box(0.0f, 0.45f, 0.0f, 0.4f, 0.45f, 0.4f, 1.0f, 1.0f, 1.0f);
        cubo_plataforma = add_box(0.0f, 0.0f, 0.0f, 2.0f, 0.25f, 2.0f, 1.0f, 1.0f, 1.0f);
        cubo_npc = add_box(0.0f, 0.0f, 0.0f, 0.25f, 0.85f, 0.25f, 1.0f, 1.0f, 1.0f);

        uint32_t vb_size = (uint32_t)(vertices.size() * sizeof(Vertex));
        uint32_t ib_size = (uint32_t)(indices.size() * sizeof(uint16_t));

        SDL_GPUTransferBufferCreateInfo tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vb_size + ib_size };
        SDL_GPUTransferBuffer* temp_transfer = SDL_CreateGPUTransferBuffer(device, &tb_info);

        void* map = SDL_MapGPUTransferBuffer(device, temp_transfer, false);
        memcpy(map, vertices.data(), vb_size);
        memcpy((uint8_t*)map + vb_size, indices.data(), ib_size);
        SDL_UnmapGPUTransferBuffer(device, temp_transfer);

        SDL_GPUBufferCreateInfo vb_info = { SDL_GPU_BUFFERUSAGE_VERTEX, vb_size };
        vertex_buffer = SDL_CreateGPUBuffer(device, &vb_info);

        SDL_GPUBufferCreateInfo ib_info = { SDL_GPU_BUFFERUSAGE_INDEX, ib_size };
        index_buffer = SDL_CreateGPUBuffer(device, &ib_info);

        SDL_GPUCommandBuffer* copy_cmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(copy_cmd);

        SDL_GPUTransferBufferLocation vb_loc = { temp_transfer, 0 };
        SDL_GPUBufferRegion vb_reg = { vertex_buffer, 0, vb_size };
        SDL_UploadToGPUBuffer(copy_pass, &vb_loc, &vb_reg, false);

        SDL_GPUTransferBufferLocation ib_loc = { temp_transfer, vb_size };
        SDL_GPUBufferRegion ib_reg = { index_buffer, 0, ib_size };
        SDL_UploadToGPUBuffer(copy_pass, &ib_loc, &ib_reg, false);

        std::vector<SDL_GPUTransferBuffer*> tbs;
        for (auto& pt : pending_texs) {
            uint32_t tex_size = pt.w * pt.h * 4;
            SDL_GPUTransferBufferCreateInfo a_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, tex_size };
            SDL_GPUTransferBuffer* asset_tb = SDL_CreateGPUTransferBuffer(device, &a_tb_info);

            void* a_map = SDL_MapGPUTransferBuffer(device, asset_tb, false);
            memcpy(a_map, pt.data.data(), tex_size);
            SDL_UnmapGPUTransferBuffer(device, asset_tb);

            SDL_GPUTextureTransferInfo a_tex_loc = { asset_tb, 0, 0, 0 };
            SDL_GPUTextureRegion a_tex_reg = { pt.tex, 0, 0, 0, 0, 0, pt.w, pt.h, 1 };
            SDL_UploadToGPUTexture(copy_pass, &a_tex_loc, &a_tex_reg, false);

            tbs.push_back(asset_tb);
        }

        SDL_EndGPUCopyPass(copy_pass);
        SDL_SubmitGPUCommandBuffer(copy_cmd);

        SDL_ReleaseGPUTransferBuffer(device, temp_transfer);
        for (auto tb : tbs) {
            SDL_ReleaseGPUTransferBuffer(device, tb);
        }

        phys::phys_teleport_character(lvl.spawn.x, lvl.spawn.y, lvl.spawn.z);
        phys::phys_refresh_contacts();

        active_phys_state = { lvl.spawn.x, lvl.spawn.y, lvl.spawn.z, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false };
        current_player_pos = glm::vec3(lvl.spawn.x, lvl.spawn.y, lvl.spawn.z);
        prev_player_pos = current_player_pos;

        level_box_count = lvl.boxes.size();
        replant_grass();
        };

    const char* npc_names[5] = { "Ana", "Bruno", "Cora", "Davi", "Eva" };
    float npc_colors[5][3] = { {1.0f, 0.3f, 0.3f}, {0.3f, 1.0f, 0.3f}, {0.3f, 0.3f, 1.0f}, {1.0f, 1.0f, 0.3f}, {1.0f, 0.3f, 1.0f} };
    social::Schedule npc_scheds[5];
    social::NpcState npcs[5];
    social::NpcState prev_npcs[5];

    for (int i = 0; i < 5; ++i) {
        std::string path = std::string(SCHEDULES_DIR) + "npc" + std::to_string(i) + ".txt";
        std::ifstream f(path);
        if (f) {
            std::stringstream buffer;
            buffer << f.rdbuf();
            npc_scheds[i] = social::parse_schedule(buffer.str().c_str());
            app_log.add("Agenda social carregada: npc%d.txt [%s]", i, npc_scheds[i].categoria);
        }
        social::init_npc(npcs[i], npc_scheds[i]);
        prev_npcs[i] = npcs[i];
    }

    std::string active_level_path = std::string(LEVELS_DIR) + "arena.txt";
    std::error_code fs_ec;
    std::filesystem::file_time_type active_level_mtime = std::filesystem::last_write_time(active_level_path, fs_ec);
    double watch_accum = 0.0;

    level::LevelData initial_lvl = level::load_level(active_level_path.c_str());
    if (initial_lvl.ok) {
        build_level(initial_lvl);
        app_log.add("Nivel recarregado: arena.txt (%zu caixas, %zu meshes)", level_box_count, level_assets.size());
    }
    else {
        app_log.add("Aviso: Falha ao carregar arena.txt");
    }

    InstanceData neutral_inst = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    uint32_t neutral_size = sizeof(InstanceData);

    SDL_GPUTransferBufferCreateInfo n_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, neutral_size };
    SDL_GPUTransferBuffer* n_transfer = SDL_CreateGPUTransferBuffer(device, &n_tb_info);
    void* n_map = SDL_MapGPUTransferBuffer(device, n_transfer, false);
    memcpy(n_map, &neutral_inst, neutral_size);
    SDL_UnmapGPUTransferBuffer(device, n_transfer);

    SDL_GPUBufferCreateInfo n_inst_info = { SDL_GPU_BUFFERUSAGE_VERTEX, neutral_size };
    SDL_GPUBuffer* neutral_instance_buffer = SDL_CreateGPUBuffer(device, &n_inst_info);

    SDL_GPUCommandBuffer* n_cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* n_pass = SDL_BeginGPUCopyPass(n_cmd);
    SDL_GPUTransferBufferLocation n_loc = { n_transfer, 0 };
    SDL_GPUBufferRegion n_reg = { neutral_instance_buffer, 0, neutral_size };
    SDL_UploadToGPUBuffer(n_pass, &n_loc, &n_reg, false);
    SDL_EndGPUCopyPass(n_pass);
    SDL_SubmitGPUCommandBuffer(n_cmd);
    SDL_ReleaseGPUTransferBuffer(device, n_transfer);

    SDL_GPUBufferCreateInfo c_inst_info = { SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(world.count * sizeof(InstanceData)) };
    SDL_GPUBuffer* crowd_instance_buffer = SDL_CreateGPUBuffer(device, &c_inst_info);

    SDL_GPUBufferCreateInfo char_inst_info = { SDL_GPU_BUFFERUSAGE_VERTEX, 2 * sizeof(InstanceData) };
    SDL_GPUBuffer* char_instance_buffer = SDL_CreateGPUBuffer(device, &char_inst_info);

    SDL_GPUBufferCreateInfo npc_inst_info = { SDL_GPU_BUFFERUSAGE_VERTEX, 5 * sizeof(InstanceData) };
    SDL_GPUBuffer* npc_instance_buffer = SDL_CreateGPUBuffer(device, &npc_inst_info);

    SDL_GPUTransferBufferCreateInfo dyn_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (uint32_t)(world.count * sizeof(InstanceData)) };
    SDL_GPUTransferBuffer* dyn_transfer_buf = SDL_CreateGPUTransferBuffer(device, &dyn_tb_info);

    SDL_GPUTransferBufferCreateInfo char_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 2 * sizeof(InstanceData) };
    SDL_GPUTransferBuffer* char_transfer_buf = SDL_CreateGPUTransferBuffer(device, &char_tb_info);

    SDL_GPUTransferBufferCreateInfo npc_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 5 * sizeof(InstanceData) };
    SDL_GPUTransferBuffer* npc_transfer_buf = SDL_CreateGPUTransferBuffer(device, &npc_tb_info);

    SDL_GPUGraphicsPipelineCreateInfo pipe_info = {};
    pipe_info.vertex_shader = vs;
    pipe_info.fragment_shader = ps;
    pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;

    pipe_info.depth_stencil_state.enable_depth_test = true;
    pipe_info.depth_stencil_state.enable_depth_write = true;
    pipe_info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

    SDL_GPUVertexAttribute attrs[6] = {};
    attrs[0].location = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[0].offset = 0;                 attrs[0].buffer_slot = 0;
    attrs[1].location = 1; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[1].offset = 3 * sizeof(float); attrs[1].buffer_slot = 0;
    attrs[2].location = 2; attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[2].offset = 0;                 attrs[2].buffer_slot = 1;
    attrs[3].location = 3; attrs[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[3].offset = 3 * sizeof(float); attrs[3].buffer_slot = 1;
    attrs[4].location = 4; attrs[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[4].offset = 6 * sizeof(float); attrs[4].buffer_slot = 0;
    attrs[5].location = 5; attrs[5].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[5].offset = 8 * sizeof(float); attrs[5].buffer_slot = 0;

    SDL_GPUVertexBufferDescription vb_desc[2] = {};
    vb_desc[0].slot = 0; vb_desc[0].pitch = sizeof(Vertex);       vb_desc[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vb_desc[1].slot = 1; vb_desc[1].pitch = sizeof(InstanceData); vb_desc[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;

    pipe_info.vertex_input_state.num_vertex_attributes = 6;
    pipe_info.vertex_input_state.vertex_attributes = attrs;
    pipe_info.vertex_input_state.num_vertex_buffers = 2;
    pipe_info.vertex_input_state.vertex_buffer_descriptions = vb_desc;

    SDL_GPUColorTargetDescription color_descs[2] = {};
    color_descs[0].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // M2.5-24I: cena em HDR (basic+sky+grass)
    color_descs[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    pipe_info.target_info.num_color_targets = 2;
    pipe_info.target_info.color_target_descriptions = color_descs;
    pipe_info.target_info.has_depth_stencil_target = true;
    pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipe_info);
    if (!pipeline) { SDL_Log("FALHA pipeline (basic): %s", SDL_GetError()); return 1; }

    // M2.5-24: Derivando a Shadow Pipeline via Struct Copying Atenciosa
    SDL_GPUGraphicsPipelineCreateInfo shadow_pipe_info = pipe_info;
    shadow_pipe_info.vertex_shader = shadow_vs;
    shadow_pipe_info.fragment_shader = shadow_ps;
    shadow_pipe_info.target_info.num_color_targets = 0;
    shadow_pipe_info.target_info.color_target_descriptions = nullptr;
    shadow_pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    shadow_pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
    shadow_pipe_info.rasterizer_state.enable_depth_bias = true;
    shadow_pipe_info.rasterizer_state.depth_bias_constant_factor = 1.25f;
    shadow_pipe_info.rasterizer_state.depth_bias_slope_factor = 2.0f;
    SDL_GPUGraphicsPipeline* shadow_pipeline = SDL_CreateGPUGraphicsPipeline(device, &shadow_pipe_info);
    if (!shadow_pipeline) { SDL_Log("FALHA shadow_pipeline: %s", SDL_GetError()); return 1; }

    SDL_GPUVertexAttribute sky_attr[1] = {};
    sky_attr[0].location = 0;
    sky_attr[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    sky_attr[0].offset = 0;
    sky_attr[0].buffer_slot = 0;

    SDL_GPUVertexBufferDescription sky_vb_desc[1] = {};
    sky_vb_desc[0].slot = 0;
    sky_vb_desc[0].pitch = 3 * sizeof(float);
    sky_vb_desc[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUGraphicsPipelineCreateInfo sky_pipe_info = {};
    sky_pipe_info.vertex_shader = sky_vs;
    sky_pipe_info.fragment_shader = sky_ps;
    sky_pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    sky_pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE; // Domo interno, nao descartar
    sky_pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    sky_pipe_info.target_info.num_color_targets = 2;
    sky_pipe_info.target_info.color_target_descriptions = color_descs;
    sky_pipe_info.target_info.has_depth_stencil_target = true;
    sky_pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    sky_pipe_info.depth_stencil_state.enable_depth_test = false;  // Sobreposto pela geometria
    sky_pipe_info.depth_stencil_state.enable_depth_write = false;

    sky_pipe_info.vertex_input_state.num_vertex_attributes = 1;
    sky_pipe_info.vertex_input_state.vertex_attributes = sky_attr;
    sky_pipe_info.vertex_input_state.num_vertex_buffers = 1;
    sky_pipe_info.vertex_input_state.vertex_buffer_descriptions = sky_vb_desc;

    SDL_GPUGraphicsPipeline* sky_pipeline = SDL_CreateGPUGraphicsPipeline(device, &sky_pipe_info);
    if (!sky_pipeline) { SDL_Log("FALHA sky_pipeline: %s", SDL_GetError()); return 1; }

    SDL_GPUGraphicsPipelineCreateInfo grass_pipe_info = pipe_info;
    grass_pipe_info.vertex_shader = grass_vs;
    grass_pipe_info.fragment_shader = grass_ps;
    grass_pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    SDL_GPUGraphicsPipeline* grass_pipeline = SDL_CreateGPUGraphicsPipeline(device, &grass_pipe_info);

    if (!grass_pipeline) { SDL_Log("FALHA grass_pipeline: %s", SDL_GetError()); return 1; }

    SDL_GPUColorTargetDescription out_color_desc = {};
    out_color_desc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // M2.5-24I: composite HDR

    SDL_GPUGraphicsPipelineCreateInfo out_pipe_info = {};
    out_pipe_info.vertex_shader = out_vs;
    out_pipe_info.fragment_shader = out_ps;
    out_pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    out_pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    out_pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    out_pipe_info.target_info.num_color_targets = 1;
    out_pipe_info.target_info.color_target_descriptions = &out_color_desc;

    SDL_GPUGraphicsPipeline* out_pipeline = SDL_CreateGPUGraphicsPipeline(device, &out_pipe_info);
    if (!out_pipeline) { SDL_Log("FALHA out_pipeline: %s", SDL_GetError()); return 1; }

    SDL_GPUColorTargetDescription fs_color_desc = {};
    // SONDA U6 (temporaria): o swapchain codifica sRGB por hardware ou nao?
    SDL_Log("[sonda U6] swapchain=%d | R8G8B8A8_UNORM=%d  R8G8B8A8_UNORM_SRGB=%d  B8G8R8A8_UNORM=%d  B8G8R8A8_UNORM_SRGB=%d",
            (int)SDL_GetGPUSwapchainTextureFormat(device, window),
            (int)SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, (int)SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
            (int)SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, (int)SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB);
    
fs_color_desc.format = SDL_GetGPUSwapchainTextureFormat(device, window);

    SDL_GPUGraphicsPipelineCreateInfo fs_pipe_info = {};
    fs_pipe_info.vertex_shader = fs_vs;
    fs_pipe_info.fragment_shader = fs_ps;
    fs_pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    fs_pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    fs_pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    fs_pipe_info.target_info.num_color_targets = 1;
    fs_pipe_info.target_info.color_target_descriptions = &fs_color_desc;

    SDL_GPUGraphicsPipeline* fs_pipeline = SDL_CreateGPUGraphicsPipeline(device, &fs_pipe_info);
    if (!fs_pipeline) { SDL_Log("FALHA fs_pipeline: %s", SDL_GetError()); return 1; }

    // ========================================================================
    // M2.5-26-25: O PIPELINE DA MARCHA VOLUMETRICA.
    // Alvo: o composite_color (R16G16B16A16_FLOAT), nao o swapchain - e a
    // diferenca inteira desta estacao. BLEND ADITIVO, o mesmo estado que o
    // bloom_up ja usa desde a 23: ONE/ONE, BLENDOP_ADD.
    // QUATRO samplers so: profundidade, shadow map, weather map e a exposicao
    // anterior. A marcha nunca precisou de mais que isso; ela e que estava
    // hospedada num shader de nove.
    // ========================================================================
    SDL_GPUShaderCreateInfo sh_vs_info = {};
    sh_vs_info.code = g_shaft_vs; sh_vs_info.code_size = sizeof(g_shaft_vs);
    sh_vs_info.entrypoint = "VSMain"; sh_vs_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    sh_vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShader* shaft_vs = SDL_CreateGPUShader(device, &sh_vs_info);
    if (!shaft_vs) { SDL_Log("FALHA shaft_vs: %s", SDL_GetError()); return 1; }

    SDL_GPUShaderCreateInfo sh_ps_info = {};
    sh_ps_info.code = g_shaft_ps; sh_ps_info.code_size = sizeof(g_shaft_ps);
    sh_ps_info.entrypoint = "PSMain"; sh_ps_info.format = SDL_GPU_SHADERFORMAT_DXBC;
    sh_ps_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    sh_ps_info.num_samplers = 4;
    sh_ps_info.num_uniform_buffers = 1;
    SDL_GPUShader* shaft_ps = SDL_CreateGPUShader(device, &sh_ps_info);
    if (!shaft_ps) { SDL_Log("FALHA shaft_ps: %s", SDL_GetError()); return 1; }

    SDL_GPUColorTargetDescription sh_color_desc = {};
    sh_color_desc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    sh_color_desc.blend_state.enable_blend = true;
    sh_color_desc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    sh_color_desc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    sh_color_desc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    sh_color_desc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    sh_color_desc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    sh_color_desc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo sh_pipe_info = {};
    sh_pipe_info.vertex_shader = shaft_vs;
    sh_pipe_info.fragment_shader = shaft_ps;
    sh_pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    sh_pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    sh_pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    sh_pipe_info.target_info.num_color_targets = 1;
    sh_pipe_info.target_info.color_target_descriptions = &sh_color_desc;
    SDL_GPUGraphicsPipeline* shaft_pipeline = SDL_CreateGPUGraphicsPipeline(device, &sh_pipe_info);
    if (!shaft_pipeline) { SDL_Log("FALHA shaft_pipeline: %s", SDL_GetError()); return 1; }

    // M2.5-23: Criacao Unificada das 3 Permutacoes do Bloom Pipeline (LDR/Additive)
    SDL_GPUGraphicsPipelineCreateInfo b_pipe_info = {};
    b_pipe_info.vertex_shader = bloom_vs;
    b_pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    b_pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    b_pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    b_pipe_info.target_info.num_color_targets = 1;

    SDL_GPUColorTargetDescription b_target_desc = {};
    b_target_desc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // M2.5-24I
    b_pipe_info.target_info.color_target_descriptions = &b_target_desc;

    b_pipe_info.fragment_shader = bloom_thresh_ps;
    SDL_GPUGraphicsPipeline* bloom_thresh_pipeline = SDL_CreateGPUGraphicsPipeline(device, &b_pipe_info);

    b_pipe_info.fragment_shader = bloom_down_ps;
    SDL_GPUGraphicsPipeline* bloom_down_pipeline = SDL_CreateGPUGraphicsPipeline(device, &b_pipe_info);

    // O pass de Up do Kawase requer Blend state Aditivo para fundir sobre a textura anterior
    b_target_desc.blend_state.enable_blend = true;
    b_target_desc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b_target_desc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b_target_desc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    b_target_desc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b_target_desc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b_target_desc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    b_pipe_info.fragment_shader = bloom_up_ps;
    SDL_GPUGraphicsPipeline* bloom_up_pipeline = SDL_CreateGPUGraphicsPipeline(device, &b_pipe_info);

    // M2.5-26-4: pipelines e alvos da exposicao medida. Tamanho FIXO (32x32 e 1x1):
    // nao dependem da resolucao interna, entao ficam fora da recriacao do resize.
    SDL_GPUGraphicsPipelineCreateInfo e_pipe_info = {};
    e_pipe_info.vertex_shader = expo_vs;
    e_pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    e_pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    e_pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    e_pipe_info.target_info.num_color_targets = 1;
    SDL_GPUColorTargetDescription e_target_desc = {};
    e_target_desc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    e_pipe_info.target_info.color_target_descriptions = &e_target_desc;

    e_pipe_info.fragment_shader = expo_luma_ps;
    SDL_GPUGraphicsPipeline* expo_luma_pipeline = SDL_CreateGPUGraphicsPipeline(device, &e_pipe_info);
    if (!expo_luma_pipeline) { SDL_Log("FALHA expo_luma_pipeline: %s", SDL_GetError()); return 1; }

    e_pipe_info.fragment_shader = expo_adapt_ps;
    SDL_GPUGraphicsPipeline* expo_adapt_pipeline = SDL_CreateGPUGraphicsPipeline(device, &e_pipe_info);
    if (!expo_adapt_pipeline) { SDL_Log("FALHA expo_adapt_pipeline: %s", SDL_GetError()); return 1; }

    // M2.5-26-14: pipelines do operador local. Mesmo molde da exposicao (alvo
    // R16G16B16A16_FLOAT, tamanho fixo), entao herdam o e_pipe_info.
    e_pipe_info.fragment_shader = lt_down_ps;
    SDL_GPUGraphicsPipeline* lt_down_pipeline = SDL_CreateGPUGraphicsPipeline(device, &e_pipe_info);
    if (!lt_down_pipeline) { SDL_Log("FALHA lt_down_pipeline: %s", SDL_GetError()); return 1; }

    e_pipe_info.fragment_shader = lt_blur_ps;
    SDL_GPUGraphicsPipeline* lt_blur_pipeline = SDL_CreateGPUGraphicsPipeline(device, &e_pipe_info);
    if (!lt_blur_pipeline) { SDL_Log("FALHA lt_blur_pipeline: %s", SDL_GetError()); return 1; }

    e_pipe_info.fragment_shader = lt_bilat_ps; // M2.5-26-16
    SDL_GPUGraphicsPipeline* lt_bilat_pipeline = SDL_CreateGPUGraphicsPipeline(device, &e_pipe_info);
    if (!lt_bilat_pipeline) { SDL_Log("FALHA lt_bilat_pipeline: %s", SDL_GetError()); return 1; }

    e_pipe_info.fragment_shader = lt_grid_ps; // M2.5-26-23a
    SDL_GPUGraphicsPipeline* lt_grid_pipeline = SDL_CreateGPUGraphicsPipeline(device, &e_pipe_info);
    if (!lt_grid_pipeline) { SDL_Log("FALHA lt_grid_pipeline: %s", SDL_GetError()); return 1; }

    SDL_GPUTextureCreateInfo expo_tex_info = {};
    expo_tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    expo_tex_info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    expo_tex_info.layer_count_or_depth = 1;
    expo_tex_info.num_levels = 1;
    expo_tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    expo_tex_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    expo_tex_info.width = 32; expo_tex_info.height = 32;
    SDL_GPUTexture* luma_map_tex = SDL_CreateGPUTexture(device, &expo_tex_info);
    expo_tex_info.width = 1; expo_tex_info.height = 1;
    SDL_GPUTexture* expo_tex[2] = { SDL_CreateGPUTexture(device, &expo_tex_info), SDL_CreateGPUTexture(device, &expo_tex_info) };
    if (!luma_map_tex || !expo_tex[0] || !expo_tex[1]) { SDL_Log("FALHA alvos da exposicao: %s", SDL_GetError()); return 1; }
    int expo_ping = 0;

    // M2.5-26-14: os dois alvos 64x32 do operador local. A resolucao vem da fonte
    // (Patry 2021, slide 135/140: grid 64 W x 32 H, blur de raio 13 sobre 64x32).
    // M2.5-26-16: passam a ser TRES. [0] a log-luminancia crua (o bilateral precisa
    // dela intacta), [2] o B gaussiano, [1] o B FINAL que o fullscreen le.
    expo_tex_info.width = 64; expo_tex_info.height = 32;
    SDL_GPUTexture* lt_tex[3] = { SDL_CreateGPUTexture(device, &expo_tex_info),
                                  SDL_CreateGPUTexture(device, &expo_tex_info),
                                  SDL_CreateGPUTexture(device, &expo_tex_info) };
    if (!lt_tex[0] || !lt_tex[1] || !lt_tex[2]) { SDL_Log("FALHA alvos do tonemap local: %s", SDL_GetError()); return 1; }

    // M2.5-26-23a: o atlas da grade bilateral. 12 fatias Z de 64x32, dispostas
    // 4 x 3 = 256x96. O layout de fatias lado a lado numa textura 2D e o do
    // proprio Chen et al. 2007, escolhido por eles para usar filtragem bilinear
    // de hardware e reduzir os lookups do slicing de 8 para 2.
    // PROFUNDIDADE 12, e ela e DERIVADA: faixa exposta medida / sigma_r =
    // 10.87 ln / 0.921 ln = 11.8. Dentro do [2.5, 20] publicado pelo Chen.
    expo_tex_info.width = 256; expo_tex_info.height = 96;
    SDL_GPUTexture* grid_tex = SDL_CreateGPUTexture(device, &expo_tex_info);
    if (!grid_tex) { SDL_Log("FALHA alvo da grade bilateral: %s", SDL_GetError()); return 1; }

    // Variaveis globais de Render Target mantidas nulas ate a hora certa
    SDL_GPUTexture* bloom_tex_a = nullptr;
    SDL_GPUTexture* bloom_tex_b = nullptr;
    SDL_GPUTexture* bloom_tex_c = nullptr;
    SDL_GPUTexture* bloom_tex_d = nullptr;

    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo imgui_info = {};
    imgui_info.Device = device;
    imgui_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    imgui_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&imgui_info);

    int max_hardware_threads = (int)std::thread::hardware_concurrency();
    if (max_hardware_threads < 1) max_hardware_threads = 1;
    int sim_threads = max_hardware_threads;

    app_log.add("JobSystem ativado em pool com %d workers físicos.", max_hardware_threads);
    app_log.add("Mundo seed=%llu", (unsigned long long)world.seed);
    app_log.add("AVISO: F6 para o Palco Social. F5 recarrega a Arena.");

    glm::vec3 cam_pos(0.0f, 35.0f, -70.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = -0.4f;

    float wheel_target = 9.0f;
    float zoom_arm = 9.0f;
    float actual_arm_length = 9.0f;

    bool exp_walk_stairs = true;
    float exp_stick_step_down = 0.5f;

    bool rmb_down = false;
    bool cam_follow = false;
    bool in_torture = false;
    bool is_crouched = false;
    bool draw_crowd = true;
    bool manual_sun = false;

    float sun_color_arr[3] = { 1.0f, 1.0f, 1.0f };

    int view_mode = 0;
    const char* view_modes[] = { "Final", "Normais+ID", "Sem outline", "Shafts (debug)", "MEDIDOR: EV100", "MEDIDOR: luminancia", "MEDIDOR: grade bilateral" };

    int toon_cuts = 3;
    float toon_soft = 1.0f;

    constexpr uint64_t SIM_HZ = 60;
    constexpr double DT = 1.0 / 60.0;

    uint64_t sim_tick = 0;
    double accumulator = 0.0;

    int64_t time_scale = 1;
    int64_t previous_time_scale = 1;

    uint64_t current_time = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();

    double last_frame_ms = 0.0;
    double last_sync_ms = 0.0;
    double last_sim_ms_per_tick = 0.0;
    double last_hash_ms = 0.0;
    double last_phys_ms = 0.0;
    int last_ticks_this_frame = 0;

    bool first_sync = true;
    bool arm_stop = false;
    uint64_t stop_at_tick = 6000;
    uint64_t last_calculated_hash = 0;
    uint64_t last_hash_tick = 0;
    bool audit_hash = false;

    std::vector<glm::vec3> pos_history(60, glm::vec3(0.0f));
    int history_idx = 0;
    int history_count = 0;
    int ticks_in_air = 0;

    std::vector<float> delta_pos_history(60, 0.0f);
    int dp_idx = 0;
    std::vector<float> delta_interp_history(60, 0.0f);
    int di_idx = 0;
    std::vector<bool> ground_history(60, false);
    int gh_idx = 0;

    std::vector<bool> stall_history(60, false);
    int sh_idx = 0;
    glm::vec2 last_stall_req(0.0f);
    glm::vec2 last_stall_got(0.0f);
    bool last_stall_ground = false;

    phys::PhysState plat_state = { 0.0f, 0.0f, 0.0f, false };
    glm::vec3 current_plat_pos(0.0f);
    glm::vec3 prev_plat_pos(0.0f);

    uint64_t last_displayed_second = ~0ULL;
    int64_t last_displayed_scale = -1;

    auto set_scale = [&](int64_t new_scale, const char* origin) {
        if (time_scale != new_scale) {
            time_scale = new_scale;
            if (new_scale != 0) previous_time_scale = new_scale;
            app_log.add("Velocidade alterada para %lldx (%s)", (long long)new_scale, origin);
        }
        };

    uint64_t gen_seed = 42;
    uint64_t last_layout_hash = 0;
    int last_room_count = 0;
    int last_box_count = 0;

    int coyote_counter = 999;
    int buffer_counter = 0;
    int dash_counter = 0;
    int dash_cooldown_counter = 0;
    int coyote_usados = 0;
    int buffer_usados = 0;
    glm::vec3 dash_dir(0.0f);
    bool prev_space = false;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);

            if (event.type == SDL_EVENT_QUIT) running = false;

            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                win_w = event.window.data1;
                win_h = event.window.data2;
                resize_needed = true;
            }

            if (!ImGui::GetIO().WantCaptureMouse) {
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
                    rmb_down = true;
                    SDL_SetWindowRelativeMouseMode(window, true);
                }
                else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT) {
                    rmb_down = false;
                    SDL_SetWindowRelativeMouseMode(window, false);
                }
                else if (event.type == SDL_EVENT_MOUSE_MOTION && rmb_down) {
                    float sens = 0.002f;
                    cam_yaw += event.motion.xrel * sens;
                    cam_pitch -= event.motion.yrel * sens;
                    cam_pitch = std::clamp(cam_pitch, -1.047f, 0.436f);
                }
                else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    wheel_target -= event.wheel.y;
                    wheel_target = std::clamp(wheel_target, 3.0f, 14.0f);
                }
            }
            else if (rmb_down) {
                rmb_down = false;
                SDL_SetWindowRelativeMouseMode(window, false);
            }

            if (!ImGui::GetIO().WantCaptureKeyboard) {
                if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (event.key.key == SDLK_ESCAPE) running = false;
                    else if (event.key.key == SDLK_P) {
                        if (time_scale == 0) set_scale(previous_time_scale, "Tecla");
                        else set_scale(0, "Tecla");
                    }
                    else if (event.key.key == SDLK_F) {
                        cam_follow = !cam_follow;
                        app_log.add("Camera: %s", cam_follow ? "FOLLOW" : "FLY");
                    }
                    else if (event.key.key == SDLK_F5) {
                        active_level_path = std::string(LEVELS_DIR) + "arena.txt";
                        level::LevelData new_lvl = level::load_level(active_level_path.c_str());
                        if (new_lvl.ok) {
                            build_level(new_lvl);
                            active_level_mtime = std::filesystem::last_write_time(active_level_path, fs_ec);
                            app_log.add("Nivel recarregado: arena.txt (%zu caixas, %zu meshes)", level_box_count, level_assets.size());
                        }
                        else app_log.add("Erro: Falha ao carregar arena.txt");
                    }
                    else if (event.key.key == SDLK_F6) {
                        active_level_path = std::string(LEVELS_DIR) + "palco.txt";
                        level::LevelData new_lvl = level::load_level(active_level_path.c_str());
                        if (new_lvl.ok) {
                            build_level(new_lvl);
                            active_level_mtime = std::filesystem::last_write_time(active_level_path, fs_ec);
                            app_log.add("Nivel carregado: palco.txt (%zu caixas, %zu meshes)", level_box_count, level_assets.size());
                        }
                        else app_log.add("Erro: Falha ao carregar palco.txt");
                    }
                    else if (event.key.key == SDLK_T) {
                        in_torture = !in_torture;
                        glm::vec3 target = in_torture ? glm::vec3(300.0f, 2.0f, 0.0f) : current_player_pos;

                        phys::phys_teleport_character(target.x, target.y, target.z);
                        phys::phys_refresh_contacts();

                        active_phys_state.x = target.x;
                        active_phys_state.y = target.y;
                        active_phys_state.z = target.z;
                        current_player_pos = target;
                        prev_player_pos = target;

                        app_log.add("Teleporte: %s", in_torture ? "Cena de Tortura" : "Volta a Masmorra/Arena");
                    }
                    else if (event.key.key == SDLK_C) {
                        is_crouched = !is_crouched;
                        if (!phys::phys_resize_character(is_crouched)) {
                            is_crouched = !is_crouched;
                            app_log.add("Falha: Sem espaco para alterar postura.");
                        }
                        else {
                            app_log.add("Postura: %s", is_crouched ? "Agachado" : "Em pe");
                        }
                    }
                    else if (event.key.key == SDLK_1) set_scale(1, "Tecla");
                    else if (event.key.key == SDLK_2) set_scale(10, "Tecla");
                    else if (event.key.key == SDLK_3) set_scale(100, "Tecla");
                    else if (event.key.key == SDLK_4) set_scale(1000, "Tecla");
                }
            }
        }

        uint64_t new_time = SDL_GetPerformanceCounter();
        double frame_time = (double)(new_time - current_time) / frequency;
        current_time = new_time;
        if (frame_time > 0.25) frame_time = 0.25;
        last_frame_ms = frame_time * 1000.0;

        const bool* keys = SDL_GetKeyboardState(nullptr);

        watch_accum += frame_time;
        if (watch_accum >= 0.5) {
            watch_accum = 0.0;
            std::error_code poll_ec;
            auto current_mtime = std::filesystem::last_write_time(active_level_path, poll_ec);
            if (!poll_ec && current_mtime != active_level_mtime) {
                active_level_mtime = current_mtime;
                level::LevelData new_lvl = level::load_level(active_level_path.c_str());
                if (new_lvl.ok) {
                    build_level(new_lvl);
                    app_log.add("Hot reload: %s (%zu caixas, %zu meshes)", active_level_path.c_str(), level_box_count, level_assets.size());
                }
                else {
                    app_log.add("Hot reload ignorado: parse falhou");
                }
            }
        }

        accumulator += frame_time * time_scale;

        uint64_t sim_start_timer = SDL_GetPerformanceCounter();
        int ticks_this_frame = 0;

        while (accumulator >= DT) {
            sim::step(world, sim_tick, js, sim_threads);

            int current_minute = (int)((sim_tick / 120) % 1440);
            int64_t current_abs_min = sim_tick / 120;

            for (int i = 0; i < 5; ++i) {
                prev_npcs[i] = npcs[i];
                social::social_tick(npcs[i], npc_scheds[i], current_minute);
            }

            auto enc_events = social::social_encounters(npcs, npc_scheds, 5, rels, current_abs_min);
            for (auto& ev : enc_events) {
                const std::vector<std::string>* pool = &barks.neutro;
                if (ev.tier == 1) pool = &barks.amistoso;
                else if (ev.tier == -1) pool = &barks.frio;

                std::string bark_text = "...";
                if (!pool->empty()) {
                    int idx = (int)(((int64_t)ev.npc_a * 7 + ev.npc_b + current_abs_min) % (int64_t)pool->size());
                    bark_text = (*pool)[idx];
                }
                app_log.add("%s & %s conversam: '%s'", npc_names[ev.npc_a], npc_names[ev.npc_b], bark_text.c_str());
            }

            uint64_t phys_start = SDL_GetPerformanceCounter();

            prev_player_pos = current_player_pos;
            prev_plat_pos = current_plat_pos;

            float t = (sim_tick % 240) / 240.0f * 2.0f * 3.14159265f;
            float sine = std::sin(t);
            plat_state.x = 303.0f + 3.0f * sine;
            plat_state.y = 2.5f + 1.5f * sine;
            plat_state.z = -5.0f + 3.0f * sine;
            phys::phys_move_kinematic(plat_id, plat_state.x, plat_state.y, plat_state.z);

            feel::update_derived();

            if (buffer_counter > 0) buffer_counter--;
            if (dash_counter > 0) dash_counter--;
            if (dash_cooldown_counter > 0) dash_cooldown_counter--;
            coyote_counter++;

            bool space_down = keys[SDL_SCANCODE_SPACE];
            bool space_pressed = space_down && !prev_space;
            bool space_released = !space_down && prev_space;
            prev_space = space_down;

            if (space_pressed) buffer_counter = feel::buffer_ticks;

            glm::vec3 front_loop(sin(cam_yaw) * cos(cam_pitch), sin(cam_pitch), cos(cam_yaw) * cos(cam_pitch));
            glm::vec3 front_xz(front_loop.x, 0.0f, front_loop.z);
            if (glm::length(front_xz) > 0.0001f) front_xz = glm::normalize(front_xz);
            else front_xz = glm::vec3(0, 0, 1);
            glm::vec3 right_xz = glm::normalize(glm::cross(glm::vec3(0, 1, 0), front_xz));

            glm::vec3 dir(0.0f);
            if (cam_follow && !ImGui::GetIO().WantCaptureKeyboard) {
                float w_s = 0.0f;
                float d_a = 0.0f;
                if (keys[SDL_SCANCODE_W]) w_s += 1.0f;
                if (keys[SDL_SCANCODE_S]) w_s -= 1.0f;
                if (keys[SDL_SCANCODE_A]) d_a -= 1.0f;
                if (keys[SDL_SCANCODE_D]) d_a += 1.0f;

                dir = front_xz * w_s + right_xz * d_a;
                if (glm::length(dir) > 0.0001f) dir = glm::normalize(dir);
            }

            if (keys[SDL_SCANCODE_LSHIFT] && dash_cooldown_counter == 0 && dash_counter == 0 && glm::length(dir) > 0.0001f) {
                dash_counter = feel::dash_ticks;
                dash_cooldown_counter = feel::dash_cooldown;
                dash_dir = dir;
            }

            glm::vec3 vel(active_phys_state.vx, active_phys_state.vy, active_phys_state.vz);
            glm::vec3 rel_vel = vel;
            if (active_phys_state.on_ground) {
                rel_vel -= glm::vec3(active_phys_state.gvx, active_phys_state.gvy, active_phys_state.gvz);
            }

            glm::vec2 target_h(dir.x * feel::max_speed, dir.z * feel::max_speed);
            glm::vec2 current_h(rel_vel.x, rel_vel.z);

            if (active_phys_state.on_ground) {
                if (glm::length(dir) > 0.0001f) {
                    current_h = move_towards(current_h, target_h, feel::ground_accel * DT);
                }
                else {
                    current_h = move_towards(current_h, glm::vec2(0), feel::ground_decel * DT);
                }
                rel_vel.x = current_h.x;
                rel_vel.z = current_h.y;
                rel_vel.y = 0.0f;
                vel = rel_vel + glm::vec3(active_phys_state.gvx, active_phys_state.gvy, active_phys_state.gvz);
            }
            else {
                if (glm::length(dir) > 0.0001f) {
                    current_h += glm::vec2(dir.x, dir.z) * (feel::ground_accel * feel::air_accel_mult * (float)DT);
                    float speed = glm::length(current_h);
                    float max_allowed = std::max((float)glm::length(glm::vec2(rel_vel.x, rel_vel.z)), feel::max_speed);
                    if (speed > max_allowed) current_h = (current_h / speed) * max_allowed;
                }
                vel.x = current_h.x;
                vel.z = current_h.y;
            }

            float current_g = feel::g;
            if (vel.y < 0.0f) {
                current_g *= feel::fall_gravity_mult;
            }
            else if (std::abs(vel.y) < 1.0f && space_down) {
                current_g *= feel::apex_gravity_mult;
            }

            if (!active_phys_state.on_ground) {
                vel.y -= current_g * DT;
            }

            if (space_released && vel.y > 0.0f && !active_phys_state.on_ground) {
                vel.y *= 0.4f;
            }

            if (active_phys_state.on_ground) {
                coyote_counter = 0;
            }

            if (buffer_counter > 0 && coyote_counter < feel::coyote_ticks && dash_counter == 0) {
                vel.y = feel::v0;
                if (active_phys_state.on_ground) vel.y += active_phys_state.gvy;

                if (coyote_counter > 0) coyote_usados++;
                if (buffer_counter < feel::buffer_ticks) buffer_usados++;

                coyote_counter = feel::coyote_ticks;
                buffer_counter = 0;
            }

            if (dash_counter > 0) {
                vel = dash_dir * feel::dash_speed;
                vel.y = 0.0f;
            }

            phys::PhysInput p_input = { vel.x, vel.y, vel.z, feel::g, exp_stick_step_down, exp_walk_stairs };

            active_phys_state = phys::phys_update_character(p_input);
            last_phys_ms = (double)(SDL_GetPerformanceCounter() - phys_start) * 1000.0 / frequency;

            current_player_pos = glm::vec3(active_phys_state.x, active_phys_state.y, active_phys_state.z);
            current_plat_pos = glm::vec3(plat_state.x, plat_state.y, plat_state.z);

            float dpos = glm::length(current_player_pos - prev_player_pos);
            delta_pos_history[dp_idx] = dpos;
            dp_idx = (dp_idx + 1) % 60;

            float req_speed = glm::length(glm::vec2(p_input.vx, p_input.vz));
            bool is_stall = (dpos < 0.001f && req_speed > 0.1f);
            stall_history[sh_idx] = is_stall;
            sh_idx = (sh_idx + 1) % 60;

            if (is_stall) {
                last_stall_req = glm::vec2(p_input.vx, p_input.vz);
                last_stall_got = glm::vec2(active_phys_state.vx, active_phys_state.vz);
                last_stall_ground = active_phys_state.on_ground;
            }

            ground_history[gh_idx] = active_phys_state.on_ground;
            gh_idx = (gh_idx + 1) % 60;

            pos_history[history_idx] = current_player_pos;
            history_idx = (history_idx + 1) % 60;
            if (history_count < 60) history_count++;

            if (active_phys_state.on_ground) ticks_in_air = 0;
            else ticks_in_air++;

            sim_tick++;
            accumulator -= DT;
            ticks_this_frame++;

            if (arm_stop && sim_tick == stop_at_tick) {
                time_scale = 0;
                arm_stop = false;

                uint64_t hash_start = SDL_GetPerformanceCounter();
                uint64_t exact_hash = sim::state_hash(world, sim_tick);
                exact_hash = social::hash_npcs(exact_hash, npcs, 5);
                last_hash_ms = (double)(SDL_GetPerformanceCounter() - hash_start) * 1000.0 / frequency;

                app_log.add("Parado no tick %llu | hash %016llX", (unsigned long long)sim_tick, (unsigned long long)exact_hash);

                last_calculated_hash = exact_hash;
                last_hash_tick = sim_tick;

                break;
            }

            uint64_t tick_timer_curr = SDL_GetPerformanceCounter();
            double elapsed_sim_ms = (double)(tick_timer_curr - sim_start_timer) * 1000.0 / frequency;
            if (elapsed_sim_ms >= 100.0) {
                accumulator = 0.0;
                break;
            }
        }

        uint64_t sim_end_timer = SDL_GetPerformanceCounter();
        last_ticks_this_frame = ticks_this_frame;

        if (last_ticks_this_frame > 0) {
            double frame_sim_cost_ms = (double)(sim_end_timer - sim_start_timer) * 1000.0 / frequency;
            last_sim_ms_per_tick = frame_sim_cost_ms / last_ticks_this_frame;

            if (audit_hash) {
                if (last_hash_tick != sim_tick) {
                    uint64_t hash_start = SDL_GetPerformanceCounter();
                    last_calculated_hash = sim::state_hash(world, sim_tick);
                    last_calculated_hash = social::hash_npcs(last_calculated_hash, npcs, 5);
                    last_hash_tick = sim_tick;
                    last_hash_ms = (double)(SDL_GetPerformanceCounter() - hash_start) * 1000.0 / frequency;
                }
            }
            else {
                last_hash_ms = 0.0;
            }
        }

        float alpha = (float)(accumulator / DT);
        glm::vec3 interp_player = glm::mix(prev_player_pos, current_player_pos, alpha);
        glm::vec3 interp_plat = glm::mix(prev_plat_pos, current_plat_pos, alpha);

        static glm::vec3 prev_interp_player = interp_player;
        float dinterp = glm::length(interp_player - prev_interp_player);
        delta_interp_history[di_idx] = dinterp;
        di_idx = (di_idx + 1) % 60;
        prev_interp_player = interp_player;

        glm::vec3 front(sin(cam_yaw) * cos(cam_pitch), sin(cam_pitch), cos(cam_yaw) * cos(cam_pitch));
        glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), front));
        glm::vec3 up(0, 1, 0);

        if (cam_follow) {
            zoom_arm = glm::mix(zoom_arm, wheel_target, 8.0f * (float)frame_time);

            glm::vec3 pivot = interp_player + glm::vec3(0.0f, 1.5f, 0.0f);
            glm::vec3 dir = -front;

            float fraction = phys::phys_camera_ray(pivot.x, pivot.y, pivot.z, dir.x, dir.y, dir.z, zoom_arm);

            float hit_arm = fraction * zoom_arm;
            if (fraction < 1.0f) hit_arm -= 0.3f;
            hit_arm = std::max(0.8f, hit_arm);

            if (hit_arm < actual_arm_length) {
                actual_arm_length = hit_arm;
            }
            else {
                actual_arm_length = glm::mix(actual_arm_length, hit_arm, 2.0f * (float)frame_time);
            }

            cam_pos = pivot - front * actual_arm_length;
        }
        else {
            if (!ImGui::GetIO().WantCaptureKeyboard) {
                float speed = 35.0f * (float)frame_time;
                if (keys[SDL_SCANCODE_LSHIFT]) speed *= 4.0f;

                if (keys[SDL_SCANCODE_W]) cam_pos += front * speed;
                if (keys[SDL_SCANCODE_S]) cam_pos -= front * speed;
                if (keys[SDL_SCANCODE_A]) cam_pos -= right * speed;
                if (keys[SDL_SCANCODE_D]) cam_pos += right * speed;
                if (keys[SDL_SCANCODE_E]) cam_pos += up * speed;
                if (keys[SDL_SCANCODE_Q]) cam_pos -= up * speed;
            }
        }

        glm::vec3 mean(0.0f);
        for (int i = 0; i < history_count; ++i) mean += pos_history[i];
        if (history_count > 0) mean /= history_count;

        float variance = 0.0f;
        for (int i = 0; i < history_count; ++i) {
            glm::vec3 d = pos_history[i] - mean;
            variance += glm::dot(d, d);
        }
        float std_dev = history_count > 1 ? std::sqrt(variance / (history_count - 1)) : 0.0f;

        float dpos_min = 999.0f, dpos_max = 0.0f, dpos_avg = 0.0f;
        for (int i = 0; i < 60; ++i) {
            float d = delta_pos_history[i];
            if (d < dpos_min) dpos_min = d;
            if (d > dpos_max) dpos_max = d;
            dpos_avg += d;
        }
        dpos_avg /= 60.0f;

        float dinterp_min = 999.0f, dinterp_max = 0.0f, dinterp_avg = 0.0f;
        for (int i = 0; i < 60; ++i) {
            float d = delta_interp_history[i];
            if (d < dinterp_min) dinterp_min = d;
            if (d > dinterp_max) dinterp_max = d;
            dinterp_avg += d;
        }
        dinterp_avg /= 60.0f;

        int ground_transitions = 0;
        for (int i = 0; i < 59; ++i) {
            int idx1 = (gh_idx + i) % 60;
            int idx2 = (gh_idx + i + 1) % 60;
            if (ground_history[idx1] != ground_history[idx2]) ground_transitions++;
        }

        int stall_ticks = 0;
        for (int i = 0; i < 60; ++i) {
            if (stall_history[i]) stall_ticks++;
        }

        uint64_t game_seconds = sim_tick / SIM_HZ;
        uint64_t game_minutes_total = sim_tick / 120;
        uint64_t g_days = game_minutes_total / 1440;
        uint64_t g_hours = (game_minutes_total / 60) % 24;
        uint64_t g_minutes = game_minutes_total % 60;

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Tempo");

        // M2.5-21: Controle de Persistencia Tuning
        if (ImGui::Button("Salvar Config")) {
            save_tuning();
        }
        ImGui::SameLine();
        if (ImGui::Button("Recarregar")) {
            load_tuning();
            replant_grass(); // Reflete imediatamente mudancas nos tufos (densidade, etc)
        }
        ImGui::Separator();

        if (ImGui::SliderInt("Threads", &sim_threads, 1, max_hardware_threads)) {
            app_log.add("Poder de processamento ajustado para %d threads.", sim_threads);
        }

        if (ImGui::Button("Pausa [P]")) set_scale(0, "Botão"); ImGui::SameLine();
        if (ImGui::Button("1x"))        set_scale(1, "Botão"); ImGui::SameLine();
        if (ImGui::Button("10x"))       set_scale(10, "Botão"); ImGui::SameLine();
        if (ImGui::Button("100x"))      set_scale(100, "Botão"); ImGui::SameLine();
        if (ImGui::Button("1000x"))     set_scale(1000, "Botão");

        ImGui::Text("Tick: %llu", (unsigned long long)sim_tick);
        ImGui::Text("Mundo: Dia %llu %02llu:%02llu", (unsigned long long)g_days, (unsigned long long)g_hours, (unsigned long long)g_minutes);

        static int stop_hh = 12, stop_mm = 0;
        ImGui::PushItemWidth(40);
        ImGui::InputInt("##HH", &stop_hh, 0); ImGui::SameLine(); ImGui::Text(":"); ImGui::SameLine();
        ImGui::InputInt("##MM", &stop_mm, 0); ImGui::SameLine();
        ImGui::PopItemWidth();
        if (ImGui::Button("Pausar nesta hora")) {
            uint64_t curr_min = sim_tick / 120;
            uint64_t curr_day = curr_min / 1440;
            uint64_t target_min = curr_day * 1440 + (uint64_t)stop_hh * 60 + (uint64_t)stop_mm;
            if (target_min <= curr_min) target_min += 1440;
            stop_at_tick = target_min * 120;
            arm_stop = true;
            audit_hash = true;
            app_log.add("Auditoria/pausa armada para Dia %llu %02d:%02d (tick %llu).", (unsigned long long)(target_min / 1440), stop_hh, stop_mm, (unsigned long long)stop_at_tick);
        }

        ImGui::Text("Level Info: N/A (%zu caixas)", level_box_count);
        ImGui::Text("Camera Pos: %.2f, %.2f, %.2f", cam_pos.x, cam_pos.y, cam_pos.z);
        ImGui::Text("FPS: %.1f", last_frame_ms > 0.0 ? 1000.0 / last_frame_ms : 0.0);

        ImGui::Checkbox("Multidao", &draw_crowd);
        ImGui::SameLine();

        ImGui::Checkbox("Grama", &draw_grass);
        ImGui::SliderFloat("Escala do tufo", &grass_scale, 0.3f, 2.0f);
        ImGui::SliderFloat("Backlight", &grass_backlight, 0.0f, 1.0f);
        ImGui::SliderFloat("Brilho da ponta", &grass_specular, 0.0f, 0.6f);
        ImGui::SliderFloat("Densidade", &grass_density, 0.0f, 30.0f);
        ImGui::SliderFloat("Verdice min", &grass_verdice_min, 0.0f, 0.3f);
        ImGui::SetItemTooltip("verde mínimo p/ brotar (borracha = marrom)");
        ImGui::SliderFloat("Dureza da borda", &grass_border_sharp, 1.0f, 40.0f);
        ImGui::SliderFloat("Slope min", &grass_slope_min, 0.0f, 1.0f);

        ImGui::Separator();
        ImGui::Text("Clump-lite & Vento");
        ImGui::SliderFloat("Clump Pull", &grass_clump_pull, 0.0f, 1.0f);
        ImGui::SliderFloat("Clump Cell Size", &grass_clump_cell, 0.5f, 5.0f);
        ImGui::SliderFloat("Vento: Direcao", &wind::dir_deg, 0.0f, 360.0f);
        ImGui::SliderFloat("Vento: Forca", &wind::strength, 0.0f, 2.0f);

        if (ImGui::Button("Replantar Grama")) {
            replant_grass();
        }

        // M2.5-26-18: o NATIVO entra como terceira opcao e vira o default.
        const char* modos_pixel[] = { "Divisor", "Altura fixa", "Nativo (1:1)" };
        if (ImGui::Combo("Resolucao de render", &pixel_mode, modos_pixel, 3)) {
            resize_needed = true;
        }
        ImGui::SetItemTooltip("Nativo: um texel = um pixel, sem upscale. A escada continua aqui para quem quiser voltar ao pixel-art. CUSTO: a marcha de nuvem e POR PIXEL - de 360p para 1080p ela sai de 22 M para 199 M passos, 9x. Se a taxa cair, o primeiro alivio e 'Passos da nuvem', nao voltar a resolucao.");
        if (pixel_mode == 0) {
            if (ImGui::SliderInt("Pixel divisor", &pixel_divisor, 1, 8)) {
                resize_needed = true;
            }
        }
        else if (pixel_mode == 1) {
            const char* alturas_str[] = { "180", "240", "270", "360", "450", "540", "720", "900", "1080" };
            if (ImGui::Combo("Altura interna", &altura_idx, alturas_str, 9)) {
                resize_needed = true;
            }
        }
        else {
            ImGui::TextDisabled("Render em %dx%d (janela).", internal_w, internal_h);
        }

        ImGui::Combo("Vista", &view_mode, view_modes, 7);
        if (view_mode == 4) ImGui::TextDisabled("Tela cinza uniforme. EV100 = (nivel/255)*32 - 6");
        if (view_mode == 5) ImGui::TextDisabled("Cada pixel = log10(cd/m2) do proprio pixel. nivel/255*8 - 2");
        if (view_mode == 6) ImGui::TextDisabled("Atlas 256x96: 12 fatias Z de 64x32 (4x3), da mais ESCURA (topo-esq) para a mais CLARA (base-dir). VERMELHO = celula vazia.");

        ImGui::Text("Instancias: 100000");
        ImGui::Text("Frame ms: %.3f", last_frame_ms);
        double effective_speed = frame_time > 0.0 ? (last_ticks_this_frame * DT) / frame_time : 0.0;
        ImGui::Text("Velocidade efetiva: %.1fx", effective_speed);
        ImGui::Text("Sim ms/tick: %.3f", last_sim_ms_per_tick);
        ImGui::Text("Phys ms: %.3f", last_phys_ms);
        ImGui::Text("Sync ms: %.3f", last_sync_ms);
        ImGui::Text("Hash ms: %.3f", last_hash_ms);
        ImGui::Text("Ticks no frame: %d", last_ticks_this_frame);

        ImGui::Separator();
        ImGui::Text("Masmorra Procedural");
        ImGui::InputScalar("Seed", ImGuiDataType_U64, &gen_seed);
        if (ImGui::Button("Gerar")) {
            SDL_WaitForGPUIdle(device);
            gen::GenResult res = gen::gen_dungeon(gen_seed);
            last_layout_hash = res.layout_hash;
            last_room_count = res.room_count;
            last_box_count = res.box_count;
            build_level(res.level);
            app_log.add("Masmorra gerada: Seed %llu | Salas %d | Caixas %d", (unsigned long long)gen_seed, last_room_count, last_box_count);
        }
        ImGui::Text("Layout hash: %016llX", (unsigned long long)last_layout_hash);
        ImGui::Text("Salas: %d | Caixas procedurais: %d", last_room_count, last_box_count);

        ImGui::Separator();
        ImGui::Text("Spike / Instrumentacao");
        ImGui::Text("Pos Fis: %.4f, %.4f, %.4f", active_phys_state.x, active_phys_state.y, active_phys_state.z);
        ImGui::Text("Pos Vis: %.4f, %.4f, %.4f (alpha=%.2f)", interp_player.x, interp_player.y, interp_player.z, alpha);
        ImGui::Text("Delta Pos (ticks): min=%.4f, max=%.4f, avg=%.4f", dpos_min, dpos_max, dpos_avg);
        ImGui::Text("Delta Interp (frames): min=%.4f, max=%.4f, avg=%.4f", dinterp_min, dinterp_max, dinterp_avg);
        ImGui::Text("Jitter (StdDev 60t): %.6f", std_dev);
        ImGui::Text("Trocas de chao (60t): %d", ground_transitions);
        ImGui::Text("Stall ticks (60t): %d", stall_ticks);
        ImGui::Text("Ultimo stall: pediu=(%.2f,%.2f) obteve=(%.2f,%.2f) ground=%s",
            last_stall_req.x, last_stall_req.y, last_stall_got.x, last_stall_got.y,
            last_stall_ground ? "sim" : "nao");
        ImGui::Text("No chao: %s", active_phys_state.on_ground ? "sim" : "nao");
        ImGui::Text("Ticks no ar: %d", ticks_in_air);

        ImGui::Separator();
        ImGui::Text("Determinismo:");
        ImGui::Checkbox("Auditoria de hash", &audit_hash);
        ImGui::Text("Hash @ tick %llu: %016llX", (unsigned long long)last_hash_tick, (unsigned long long)last_calculated_hash);

        ImGui::InputScalar("Alvo", ImGuiDataType_U64, &stop_at_tick);
        if (stop_at_tick > sim_tick) {
            if (ImGui::Button("Rodar até e pausar via Tick Absoluto")) {
                arm_stop = true;
                audit_hash = true;
                app_log.add("Auditoria armada para o tick %llu.", (unsigned long long)stop_at_tick);
            }
        }
        else {
            ImGui::BeginDisabled();
            ImGui::Button("Rodar até e pausar via Tick Absoluto");
            ImGui::EndDisabled();
        }

        ImGui::End();

        ImGui::Begin("Social");
        ImGui::Text("Relogio: Dia %llu %02llu:%02llu", (unsigned long long)g_days, (unsigned long long)g_hours, (unsigned long long)g_minutes);
        if (ImGui::Button("Alarme de teste")) {
            npcs[0].action = 1;
            npcs[0].flee_x = 0.0f;
            npcs[0].flee_z = 0.0f;
            app_log.add("Alarme! %s esta em FUGA para (0,0)", npc_names[0]);
        }
        ImGui::Separator();
        for (int i = 0; i < 5; ++i) {
            const char* ativ = "Nenhuma";
            if (npc_scheds[i].ok && !npc_scheds[i].entries.empty()) {
                ativ = npc_scheds[i].entries[npcs[i].entry_atual].atividade;
            }
            const char* act_str = "";
            if (npcs[i].action == 1) act_str = "[FUGA]";
            else if (npcs[i].action == 2) act_str = "[COMER]";
            else if (npcs[i].action == 3) {
                static char buf[32];
                snprintf(buf, sizeof(buf), "[CONVERSA c/ %s]", npc_names[npcs[i].partner]);
                act_str = buf;
            }

            ImGui::Text("%s: %s (%.1f, %.1f) %s", npc_names[i], ativ, npcs[i].x, npcs[i].z, act_str);
            ImGui::ProgressBar(npcs[i].fome, ImVec2(80, 0));
        }
        ImGui::End();

        ImGui::Begin("Feel (Parametros Vivos)");
        ImGui::SliderFloat("Jump Height", &feel::jump_height, 0.5f, 5.0f);
        ImGui::SliderInt("Ticks to Apex", &feel::ticks_to_apex, 10, 60);
        ImGui::SliderFloat("Fall Grav Mult", &feel::fall_gravity_mult, 0.5f, 3.0f);
        ImGui::SliderFloat("Apex Grav Mult", &feel::apex_gravity_mult, 0.1f, 1.0f);
        ImGui::SliderInt("Coyote Ticks", &feel::coyote_ticks, 0, 20);
        ImGui::SliderInt("Buffer Ticks", &feel::buffer_ticks, 0, 20);
        ImGui::SliderFloat("Ground Accel", &feel::ground_accel, 10.0f, 150.0f);
        ImGui::SliderFloat("Ground Decel", &feel::ground_decel, 10.0f, 150.0f);
        ImGui::SliderFloat("Air Accel Mult", &feel::air_accel_mult, 0.0f, 1.0f);
        ImGui::SliderFloat("Max Speed", &feel::max_speed, 1.0f, 20.0f);
        ImGui::SliderFloat("Dash Speed", &feel::dash_speed, 5.0f, 30.0f);
        ImGui::SliderInt("Dash Ticks", &feel::dash_ticks, 1, 30);
        ImGui::SliderInt("Dash Cooldown", &feel::dash_cooldown, 0, 120);
        ImGui::Separator();
        ImGui::Text("Derivadas: g=%.2f, v0=%.2f", feel::g, feel::v0);
        ImGui::Text("Perdoes usados: Coyote=%d, Buffer=%d", coyote_usados, buffer_usados);
        const char* estado = dash_counter > 0 ? "DASH" : (active_phys_state.on_ground ? "CHAO" : (coyote_counter < feel::coyote_ticks ? "COYOTE" : "AR"));
        ImGui::Text("Estado: %s", estado);

        ImGui::Separator();
        ImGui::Text("Experimentos (Gate M1-6)");
        ImGui::Checkbox("WalkStairs", &exp_walk_stairs);
        ImGui::SliderFloat("StickToFloor StepDown", &exp_stick_step_down, 0.0f, 2.0f);

        ImGui::Separator();
        ImGui::Text("Outline Pass (M2.5-8)");
        ImGui::SliderFloat("Z Cutoff", &out_z_cutoff, 0.0001f, 0.02f, "%.4f");
        ImGui::SliderFloat("Grazing Scale", &out_grazing_scale, 0.0f, 8.0f);
        ImGui::SliderFloat("Crease Low", &out_crease_low, 0.0f, 0.5f);
        ImGui::SliderFloat("Crease High", &out_crease_high, 0.0f, 1.0f);
        ImGui::SliderFloat("Line Darken", &out_line_darken, 0.0f, 1.0f);
        ImGui::SliderFloat("Crease Brighten", &out_crease_brighten, 1.0f, 2.0f);
        ImGui::End();

        ImGui::Begin("Sol");
        // M2.5-24I-6: um clique aplica os valores COM FONTE (Nubis/Frostbite/Koschmieder).
        // Antidoto da calibracao fossilizada: tuning velho nunca mais engole estacao nova.
        // M2.5-24I-6b: o botao toca SOMENTE valores com fonte (fisica). Arte -
        // escala, cobertura, silhueta, densidade, alturas, cores - e territorio
        // do diretor e NUNCA e resetada por estacao (licao do ceu de pipoca).
        if (ImGui::Button("Fisica da Pesquisa")) {
            shaft_sigma = 0.0006f;   // Koschmieder: OD 0.1-0.4 em 500u
            shaft_g = 0.55f;         // linhagem Killzone p/ shafts
            shaft_cloud_occ = 0.95f; // T ~ 0.05 sob nuvem (quase-binario)
            tonemap_shoulder = 0.75f;
            bloom_threshold = 1.1f;  // so highlights HDR reais
            cloud_absorcao = 1.0f;   // Beer canonico
            cloud_powder_str = 0.5f; // Schneider 2015
            exposure_key = 0.1041667f; // 25-17: K/(1.2*100), K=12.5 - Frostbite eq.69 + Listing 28
            // M2.5-26-22: este botao restaura a FISICA, e EC = 0 e o neutro fisico.
            // Ele NAO restaura a calibracao de arte: apertar isto achata o
            // meio-dia de volta para 39..97 de 255. O valor de arte e 2.30 (ver o
            // comentario da declaracao de exposure_bias_ev) e volta digitando no
            // slider "Compensacao (EV)", ou com "Recarregar" se ja estiver salvo.
            exposure_bias_ev = 0.0f;   // 25-17: EC=0 = sem compensacao; a compensacao e ARTE, mas o zero e o neutro
            app_log.add("Fisica da Pesquisa aplicada (arte intacta). Salvar Config p/ persistir.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(valores com fonte)");
        ImGui::Checkbox("Ceu (Processual)", &draw_sky);
        // M2.5-26-26: mesmo conserto de EIXO do Sigma dos shafts. O passo era
        // 1e-4 e o ar limpo vale 2.1716e-05: nao havia valor digitavel correto.
        ImGui::SliderFloat("Densidade do Fog", &fog_density, 1.0e-6f, 0.02f, "%.6f /m", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Coeficiente de extincao da nevoa da CENA, por metro. Ar limpo da Tabela 1 = 2.1716e-05 /m. Ate a 26-26 o passo do controle era 1e-4 e nao existia valor digitavel certo. ATENCAO: hoje a atmosfera, os shafts e esta nevoa sao TRES donos do mesmo ar; o dono unico e o WeatherContext. Enquanto ele nao existe, mantenha este e o Sigma dos shafts na mesma ordem de grandeza, senao os raios e a nevoa contam historias diferentes sobre o mesmo mundo.");

        ImGui::Separator();
        ImGui::Text("Nuvens Volumetricas");
        ImGui::Checkbox("Renderizar Nuvens", &draw_clouds);
        ImGui::SliderFloat("Tipo de Nuvem", &cloud_type_blend, 0.0f, 1.0f, "Stratus -> Cumulonimbus");
        ImGui::SliderFloat("Erosao de Topo", &cloud_erosao_str, 0.0f, 1.0f);
        ImGui::SliderFloat("Silhueta", &cloud_silhouette, 0.0f, 0.5f);
        ImGui::SliderFloat("Bandas de luz", &cloud_bands, 1.0f, 5.0f);
        ImGui::SliderFloat("Cobertura", &cloud_cobertura, 0.0f, 1.0f);
        ImGui::SliderFloat("Densidade Volume", &cloud_densidade, 0.05f, 8.0f, "%.2f"); // 24J-2d: piso da regua honesta
        ImGui::SliderFloat("Escala do Ruido", &cloud_escala, 0.001f, 0.02f, "%.4f");
        ImGui::SliderFloat("Sombra de nuvem", &shadow_str, 0.0f, 1.0f);
        ImGui::SliderFloat("Velocidade", &cloud_speed, 0.0f, 3.0f);
        ImGui::SliderInt("Passos", &cloud_passos, 8, 120); // 24J-2c: teto do Nutshell ("maximum of 120 samples at the horizon")
        ImGui::SliderFloat("Powder", &cloud_powder_str, 0.0f, 1.0f);
        ImGui::SliderFloat("G frontal", &cloud_hg_g_frontal, 0.5f, 0.85f);
        ImGui::SliderFloat("Ambiente da nuvem", &cloud_ambient_str, 0.0f, 2.0f);
        ImGui::SliderFloat("Silver lining", &cloud_silver_k, 0.0f, 1.5f);
        ImGui::SliderFloat("Brilho interno (MS)", &cloud_ms_str, 0.0f, 2.0f);
        ImGui::SliderFloat("Fase do MS (g2)", &cloud_ms_g2, 0.0f, 0.6f);
        ImGui::SliderFloat("Luz isotropica", &cloud_iso_str, 0.0f, 0.5f);
        ImGui::SliderFloat("Ganho do ceu (LEGADO - deixe em 1)", &sky_gain, 0.0f, 4.0f, "%.2f"); // 26-2b: sem funcao desde as unidades fisicas
        ImGui::SliderInt("Debug atmosfera", &atmo_debug_mode, 0, 2);
        ImGui::SliderFloat("Silver spread", &cloud_silver_spread, 0.05f, 0.5f);
        ImGui::SliderFloat("Camada: base", &cloud_h_min, 60.0f, 200.0f);
        ImGui::SliderFloat("Camada: topo", &cloud_h_max, 150.0f, 500.0f);
        ImGui::SliderFloat("Clima: teto cobertura", &clima_cov_teto, 0.2f, 1.0f);
        ImGui::SliderFloat("Clima: teto tipo", &clima_type_teto, 0.0f, 1.0f);
        ImGui::SliderFloat("Alcance (cap)", &cloud_cap, 300.0f, 3000.0f);
        ImGui::SliderFloat("Curvatura", &cloud_curvatura, 0.0f, 0.0001f, "%.6f");
        ImGui::SliderFloat("Fade inicio", &cloud_fade_ini, 0.2f, 0.9f);
        ImGui::SliderFloat("Passo no longe", &cloud_passo_far, 0.0f, 4.0f, "%.2f");

        ImGui::Separator();
        ImGui::Checkbox("Sol automatico", &sol_automatico);
        ImGui::SameLine();
        ImGui::Checkbox("Hora manual", &sol_hora_manual);
        if (sol_hora_manual) ImGui::SliderFloat("Hora do dia", &sol_hora, 0.0f, 24.0f, "%.2fh"); // 26-5: 0.1h = 1,5 grau de arco; fino demais para julgar transicao
        if (sol_automatico) {
            ImGui::SliderFloat("Inclinacao do arco", &arco_inclinacao, 0.0f, 45.0f);
        }
        ImGui::BeginDisabled(sol_automatico);
        ImGui::SliderFloat("Yaw", &sun_yaw, 0.0f, 360.0f);
        ImGui::SliderFloat("Pitch", &sun_pitch, -89.0f, -10.0f);
        ImGui::EndDisabled();
        ImGui::SliderFloat("Ceu como luz (1 = fisico)", &sky_light_str, 0.0f, 4.0f, "%.2f"); // 26-7
        ImGui::SetItemTooltip("A luz do CEU na cena (Frostbite Tabela 8: fonte separada do sol). 1.0 = a luminancia que a marcha do Hillaire devolve.");
        ImGui::Checkbox("Perspectiva aerea fisica", &aerial_cena); // 26-8
        ImGui::SetItemTooltip("Ligado: a cena usa a MESMA marcha atmosferica do ceu. Desligado: a nevoa exponencial antiga (fog_color constante) - so para comparar.");
        ImGui::SliderInt("Aerea: passos", &aerial_steps, 1, 16); // 26-8
        // M2.5-26-9a: alcance e escala do controle CORRIGIDOS.
        // Medido: a extincao real do ar (Tabela 1, verde) vale 0,022 por km, entao
        // sobre os ~340 m da ilha ela tira 0,7% - invisivel, e CORRETO. A nevoa antiga
        // usava 0,006 por METRO = 6,0 por km, ou seja 273x o ar de verdade: nunca foi
        // fisica, era PISTA DE PROFUNDIDADE artistica, e fazia um trabalho real.
        // Logo o multiplicador precisa alcancar ~300 para reproduzir o que ela dava, e
        // o meu teto de 20 era 15x curto. Escala LOGARITMICA porque a grandeza cobre
        // quatro ordens - o mesmo erro de forma do Defeito 3 (controle linear para
        // grandeza que se pensa em fatores), que ja nos custou uma estacao.
        // Quando o mundo crescer ate o horizonte, o 1.0 fisico passa a ter o que fazer
        // sozinho: a 20 km o ar tira 36%.
        ImGui::SliderFloat("Aerea: bruma (1 = fisico)", &aerial_cena_str, 0.1f, 1000.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic); // 26-9a
        ImGui::SetItemTooltip("Multiplica o sigma_t da Tabela 1. 1.0 = ar de verdade. Acima disso e bruma por direcao de arte - a integral se auto-normaliza, entao satura na cor do ceu em vez de inventar energia.");
        ImGui::SliderFloat("Ambient (LEGADO - sem funcao)", &sun_ambient, 0.0f, 1.0f); // 26-7: o ceu real substituiu
        ImGui::SliderFloat3("Color", sun_color_arr, 0.0f, 1.0f);
        ImGui::SliderFloat("Sol: iluminancia (lux)", &sun_intensity, 0.0f, 200000.0f, "%.0f lx"); // 26-2b: Tabela 11

        ImGui::Separator();
        ImGui::Text("Sombras Projetadas");
        ImGui::Checkbox("Sombras Oclusoras", &draw_shadows);
        ImGui::SameLine();
        ImGui::Checkbox("Debug Sombra", &debug_shadows);
        ImGui::SliderFloat("Sombra obj: Forca", &obj_shadow_str, 0.0f, 1.0f);
        ImGui::SliderFloat("Bias", &obj_shadow_bias, 0.0005f, 0.01f, "%.4f");

        ImGui::Separator();
        ImGui::Text("Sun Shafts Volumetricos (24H)");
        ImGui::SliderFloat("Forca dos raios", &ray_strength, 0.0f, 2.0f);
        ImGui::SliderInt("Amostras", &ray_samples, 8, 32);
        ImGui::SliderFloat("Fase g", &shaft_g, 0.3f, 0.9f);
        ImGui::SetItemTooltip("0.80 = Cornette-Shanks, Tabela 1 do Hillaire - o MESMO g que a marcha da atmosfera usa no atmo_common.hlsli. Ate a 26-26 isto estava em 0.55 (linhagem Killzone, anterior ao Hillaire) e o motor discordava de si mesmo sobre o mesmo ar: olhando para o sol, 0.80 espalha 5,88x mais que 0.55, ou +2,56 stops.");
        // M2.5-26-26: O EIXO, nao o valor. O minimo era 0.0002 e o ar limpo da
        // Tabela 1 vale 2.1716e-05 - NOVE VEZES abaixo do que o controle
        // conseguia expressar. Agora e logaritmico, chega a 1e-6, e mostra seis
        // casas. Nao era calibracao ruim; era o eixo errado.
        ImGui::SliderFloat("Sigma (densidade do ar)", &shaft_sigma, 1.0e-6f, 0.02f, "%.6f /m", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Coeficiente de extincao do ar, por METRO. O ar limpo ao nivel do mar vale 2.1716e-05 /m, derivado dos coeficientes da Tabela 1 do Hillaire (Rayleigh 5.802/13.558/33.1e-3 por km + Mie 3.996e-3 + 4.40e-3), ponderado por luminancia porque este slider e escalar. Ate a 26-26 o minimo do controle era 0.0002 - 9,2x acima do fisico, impossivel de digitar. Valores altos sao NEVOA, o que e legitimo como clima e ilegitimo como default.");
        ImGui::SliderFloat("Queda por altura", &shaft_height_falloff, 0.0f, 0.05f, "%.4f");
        // ====================================================================
        // M2.5-26-26: O BOTAO DE CONFORMIDADE.
        // Ele existe por um motivo pratico: mudar um DEFAULT no codigo nao muda
        // o tuning.ini que ja esta salvo. Sem este botao, um valor corrigido na
        // fonte continuaria divergente na maquina de quem ja rodou o jogo -
        // e o conserto ficaria invisivel.
        // So entram aqui numeros que a documentacao FIXA. O que a fonte deixa
        // para o artista nao e tocado.
        // ====================================================================
        // O tooltip vem COLADO no botao, e o corpo depois: SetItemTooltip se
        // liga ao ultimo item, e por um bloco de chaves no meio a leitura fica
        // ambigua para quem revisa (a guarda G7 desta casa reprova, com razao).
        const bool conf_clicou = ImGui::Button("Conformidade: por os valores da documentacao");
        ImGui::SetItemTooltip("Poe de uma vez os quatro numeros que a documentacao FIXA e que estavam divergentes: Fase g = 0.80 (Cornette-Shanks, Tabela 1 do Hillaire - o mesmo da atmosfera), Sigma dos shafts e Densidade do Fog = 2.1716e-05 /m (ar limpo da Tabela 1, derivado dos coeficientes, nao digitado), e Local: bilateral sigma = 0.921 (Durand & Dorsey 2002, 0.4 em log10 convertido para ln). NAO toca em nada que a fonte deixe para a arte - iluminancia do sol, EC, forca do bloom, forca dos raios e o resto continuam seus. Existe porque mudar um default no codigo nao muda o tuning.ini ja salvo.");
        if (conf_clicou)
        {
            shaft_g      = 0.80f;              // Cornette-Shanks, Hillaire T1
            shaft_sigma  = sigma_ar_limpo();   // Tabela 1, ar limpo
            fog_density  = sigma_ar_limpo();   // idem - um ar so
            lt_sr        = 0.921f;             // Durand & Dorsey 2002 (0.4 log10)
        }
        ImGui::SliderFloat("Dither (de-banding)", &dither_amt, 0.0f, 2.0f, "%.2f"); // 25-U6d: 0 = desligado, 1 = meio degrau de 8 bits

        ImGui::Separator();
        ImGui::Text("Dual-Kawase Bloom");
        ImGui::Checkbox("Bloom", &draw_bloom);
        ImGui::SliderFloat("Bloom: Forca", &bloom_intensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Bloom: Limiar", &bloom_threshold, 0.4f, 2.5f); // 24I: alcance HDR
        // M2.5-26-11: a curva vira escolha, com o legado atras do seletor.
        const char* tm_nomes[] = { "Ombro exponencial (24I - LEGADO)", "Khronos PBR Neutral" };
        ImGui::Combo("Curva de tonemap", &tonemap_modo, tm_nomes, 2);
        ImGui::SetItemTooltip("PBR Neutral: satura em tc 13,45 contra 1,757 do legado (+2,94 stops) e preserva bandas entre 0.08 e 0.8 por translacao rigida, sem desvio de matiz. Especificacao Khronos.");
        ImGui::SliderFloat("Tonemap: ombro (so no LEGADO)", &tonemap_shoulder, 0.5f, 0.95f);
        // M2.5-26-24: o balanco de branco, na interface que a fonte descreve.
        ImGui::ColorEdit3("Balanco: a cor que o BRANCO vira", wb_alvo);
        ImGui::SetItemTooltip("Ghost of Tsushima, SIGGRAPH 2021 slide 156, verbatim: \"we let the artist choose the color they wanted white to change *to* [...] if you want the scene to be bluer, you pick a blue color, which the artists found more intuitive\". E o inverso do Lightroom, onde voce clica no que DEVERIA ser branco. Adaptacao cromatica de Von Kries em espaco Bradford LMS, em luz linear, antes da curva. A LUMINANCIA e preservada de proposito - so a cromaticidade anda, entao isto nao briga com o fotometro. Branco (1,1,1) = identidade exata (medido: desvio 3.3e-07). Para tirar o tom areia do luar, puxe para o azul: medido, uma nuvem com R/B 1.45 cai para 1.17 com (0.80,0.88,1.00) e para 0.92 com (0.62,0.78,1.00).");
        if (ImGui::Button("Balanco: neutro")) { wb_alvo[0] = wb_alvo[1] = wb_alvo[2] = 1.0f; }
        ImGui::SetItemTooltip("Volta para (1,1,1). Identidade exata, nao aproximada.");
        ImGui::SliderFloat("Bloom: teto (entrada)", &bloom_cap, 1.0f, 200.0f, "%.1f", ImGuiSliderFlags_Logarithmic); // 26-11
        ImGui::SetItemTooltip("Limita o valor EXPOSTO que entra na piramide. Frostbite 5.1.3: a celula do sensor tem capacidade finita. O numero e arte (precedente: Godot glow_hdr_luminance_cap = 12.0).");
        ImGui::Checkbox("Tonemap LOCAL (Patry 2021)", &lt_on); // 26-14
        ImGui::SetItemTooltip("Io = c*(B-M) + d*(Ii-B) + M, em log-luminancia. Comprime o contraste GLOBAL e devolve o detalhe LOCAL - dodging & burning automatico. Desligado = so a curva global.");
        ImGui::SliderFloat("Local: contraste (c)", &lt_c, 0.1f, 1.0f, "%.2f"); // 26-14
        ImGui::SetItemTooltip("1.0 = sem compressao. A fonte usa 0.75 como ajuste tipico em jogo. Quanto menor, mais o quadro e puxado para o cinza medio.");
        ImGui::SliderFloat("Local: detalhe (d)", &lt_d, 0.5f, 2.0f, "%.2f"); // 26-14
        ImGui::SetItemTooltip("Multiplica (Ii - B), que E o detalhe local - a diferenca entre o pixel e a vizinhanca dele. 1.0 = detalhe INTACTO. ABAIXO DE 1.0 ELE E DESTRUIDO: 0.50 joga fora metade de todo o contraste local do quadro, e le como imagem borrada. A fonte mostra 1.5 como 'punchier' (slide 146, '50%% Detail Strength Boost' - o que confirma que 1.0 e o neutro), usado com parcimonia. Ficou em 0.50 da 26-14 ate a 26-22 e ninguem viu por seis estacoes.");
        ImGui::SliderFloat("Local: contraste BAIXO", &lt_c_lo, 0.1f, 1.0f, "%.2f"); // 26-15
        // M2.5-26-15b: o "%" precisa ser ESCAPADO. ImGui::SetItemTooltip e printf-style:
        // em "100% Low Contrast" o parser le %% + espaco (flag) + L (modificador de long
        // double) + o (conversao INTEIRA) = "%%Lo", combinacao ilegal. O CRT do MSVC aborta
        // com 'Invalid integer length modifier'. Um %% literal se escreve %%%%.
        ImGui::SetItemTooltip("Vale para o que esta ABAIXO do ponto medio. 1.00 = sombras intocadas, que e o ajuste de jogo da fonte (slide 145: 100%% Low Contrast). Abaixar isto CLAREIA o quadro - era o defeito da 26-14.");
        ImGui::SliderFloat("Local: mistura bilateral", &lt_mix, 0.0f, 1.0f, "%.2f"); // 26-16
        ImGui::SetItemTooltip("0.40 = o valor da fonte (slide 140). 0.00 = so a gaussiana, identico a 26-15. Medido aqui: a nossa gaussiana e cega a faixas finas, entao abaixo de ~0.80 nenhum contraste positivo alcanca o horizonte. Subir traz ringing, que e justamente o que a mistura da fonte existe para evitar.");
        ImGui::SliderFloat("Local: bilateral sigma (log)", &lt_sr, 0.1f, 4.0f, "%.2f"); // 26-16
        ImGui::SetItemTooltip("Sigma da FAIXA do bilateral, em ln de luminancia (o PS_Down grava log() natural, entao a unidade e ln). A FONTE PUBLICA ESTE NUMERO, e ele nao era orfao: Durand & Dorsey 2002, verbatim, \"The value sigma_r = 0.4 performed consistently well for all our experiments\" - em log10, ou seja 0.4 x ln(10) = 0.921 ln. O default de 1.00 esta a +8.6%% dele. Menor preserva mais bordas e produz mais ringing; maior vira gaussiana.");
        ImGui::Separator();
        ImGui::Checkbox("Grade bilateral (constroi)", &grid_on); // 26-23a
        ImGui::SetItemTooltip("Constroi o atlas 256x96 (12 fatias Z de 64x32). Precisa estar LIGADA para o seletor abaixo funcionar.");
        ImGui::Checkbox("Local: B vem da GRADE", &lt_b_grade); // 26-23c
        ImGui::SetItemTooltip("DESMARCADO = B do caminho antigo (PS_Down faz media de blocos de ~40x44 px ANTES de qualquer bilateral, e por isso e cego a faixas finas). MARCADO = B da grade bilateral, amostrada trilinearmente e normalizada (Patry slide 136). Compare os dois no MESMO quadro - o [MEDIDA] le o composite, ANTES do operador local, e nunca vai mostrar a diferenca. Aqui quem julga e o olho. Para o blend ficar EXATO como o slide 140, ponha 'Local: mistura bilateral' em 0.00.");
        ImGui::SliderFloat("Grade: mistura no B", &grid_mix, 0.0f, 1.0f, "%.2f"); // 26-23c
        ImGui::SetItemTooltip("Peso da GRADE no B final. Patry slide 140, verbatim: \"Blend between bilateral filter and very wide 2D Gaussian\" - 40%% bilateral, 60%% gaussiana, e o 0.40 e o default por isso. 1.00 = so a grade (sem o anti-halo da fonte, entao espere halos). 0.00 = identico ao B antigo. Varra o intervalo inteiro uma vez para ver o que a grade faz antes de decidir onde parar.");
        ImGui::SetItemTooltip("Constroi o atlas 256x96 (12 fatias Z de 64x32). NESTA ESTACAO NADA LE O RESULTADO alem da Vista 'MEDIDOR: grade bilateral' - a imagem final nao muda. Desligue para medir o custo.");
        ImGui::SliderFloat("Grade: sigma_r (ln)", &grid_sr, 0.2f, 3.0f, "%.3f"); // 26-23a
        ImGui::SetItemTooltip("Durand & Dorsey 2002: 0.4 log10 = 0.921 ln, \"performed consistently well for all our experiments\". Chen 2007 sec. 3.2: a largura da caixa Z E o range sigma - sao um parametro so, por decisao de projeto declarada.");
        ImGui::SliderFloat("Grade: Z abaixo de M (sigma_r)", &grid_lo, 0.5f, 12.0f, "%.2f"); // 26-23a
        ImGui::SetItemTooltip("Onde o eixo Z comeca, em sigma_r abaixo do ponto medio exposto M. 2.0 = 2.66 stops. Medido no quadro das 5h54: o piso esta a 2.22 sigma_r de M. LACUNA DA FONTE: nem Patry nem Chen publicam o intervalo absoluto; Chen normaliza a guia e diz que a faixa e ENTRADA da aplicacao. Por isso e slider.");
        ImGui::SliderFloat("Grade: Z acima de M (sigma_r)", &grid_hi, 0.5f, 20.0f, "%.2f"); // 26-23a
        ImGui::SetItemTooltip("Onde o eixo Z termina. 10.0 = 13.3 stops acima de M. Medido: a faixa do horizonte das 5h54 esta a 9.58 sigma_r de M, entao 9 a cortava. A assimetria 2/10 espelha a da camera: Frostbite 5.1.2 poe a media em 12,7%% da saturacao, entao sobra pouco abaixo e muito acima. MANTENHA lo + hi = 12: essa e a profundidade derivada (10.87 ln / 0.921) e e o que faz a largura da caixa valer sigma_r, a regra do Chen.");
        ImGui::SliderInt("Grade: taps por eixo da celula", &grid_taps, 2, 24); // 26-23a
        ImGui::SetItemTooltip("N x N amostras por celula. 13 nao e escolhido, e derivado: a celula tem 40x44.3 = 1771 pixels e 13x13 = 169 = 9.54%% deles. Chen 2007 sec. 3.5, verbatim: \"using only 10%% of the input pixels produces an output with no visual artifacts\". Custo total: 24.576 texels x N x N.");
        { // leitura derivada, so para conferir na tela
            float _sp = (grid_lo + grid_hi) * grid_sr;
            ImGui::TextDisabled("Z: %.2f ln = %.2f stops | %.3f ln/fatia = %.2f stops/fatia | %d taps/celula",
                _sp, _sp / 0.6931472f, _sp / 12.0f, _sp / 12.0f / 0.6931472f, grid_taps * grid_taps);
        }
        ImGui::Separator();
        ImGui::SliderFloat("Exposicao manual (EV100)", &exposure_ev, -6.0f, 22.0f, "%.2f EV"); // 26-6b
        ImGui::SetItemTooltip("So vale com o automatico DESLIGADO. EV100 15 = Sunny 16 (meio-dia claro); 0 = interior a noite; -6 = luar.");
        ImGui::Checkbox("Exposicao automatica", &auto_exposure); // M2.5-25-4
        ImGui::SliderFloat("Chave (cinza medio)", &exposure_key, 0.01f, 0.25f, "%.3f"); // 25-4b: regua honesta
        ImGui::SliderFloat("Compensacao (EV)", &exposure_bias_ev, -4.0f, 4.0f, "%.2f");
        ImGui::SetItemTooltip("Frostbite eq. 68: EV100' = EV100 - EC. E o parametro que a fonte reserva para a ARTE decidir o brilho da cena, e ela nao publica valor. 2.30 = calibrado no olho, ate o meio-dia ter branco (com 0.00 o quadro inteiro vivia entre 39 e 97 de 255). CUIDADO: e GLOBAL. Medido as 5h54, cada stop daqui vai inteiro para o estouro do amanhecer. A saida certa e uma tabela de EC por HORA."); // 26-22
        ImGui::SliderFloat("Adaptacao: p/ o CLARO (stops/s)", &expo_vel_up, 0.1f, 12.0f, "%.2f"); // 26-19
        ImGui::SetItemTooltip("Quantos stops por segundo a exposicao FECHA quando a cena clareia. 3.0 = default do Unreal (Speed Up); a faixa que a fonte sugere e 3 a 6. O olho fecha rapido na luz.");
        ImGui::SliderFloat("Adaptacao: p/ o ESCURO (stops/s)", &expo_vel_dn, 0.1f, 12.0f, "%.2f"); // 26-19
        ImGui::SetItemTooltip("Quantos stops por segundo a exposicao ABRE quando a cena escurece. 1.0 = default do Unreal (Speed Down); a faixa sugerida e 1 a 2. Unity HDRP, verbatim: \"The human eye adapts slower to darkness than to lightness\".");
        ImGui::SliderFloat("Adaptacao: assentamento (stops)", &expo_ev_trans, 0.1f, 6.0f, "%.2f"); // 26-19
        ImGui::SetItemTooltip("A que distancia do alvo a rampa de taxa constante vira exponencial. 1.5 = default do Unreal (ExponentialTransitionDistance). Medido aqui: a continuidade C1 exata cai em 1/ln2 = 1.4427, entao o 1.5 da fonte esta a 4%% dela.");
        ImGui::SliderFloat("Histograma: corte inferior", &expo_perc_lo, 0.0f, 0.9f, "%.2f"); // 26-12
        ImGui::SetItemTooltip("Borda INFERIOR da janela de medida, em percentil. Unreal, verbatim: \"Low Percent - a good starting range is 70 to 80\". Isto NAO apara caudas: junto com o corte superior ele define uma janela ESTREITA e ALTA (10%% do quadro), que mede o decil claro em vez do brilho medio.");
        ImGui::SliderFloat("Histograma: corte superior", &expo_perc_hi, 0.1f, 1.0f, "%.2f"); // 26-12
        ImGui::SetItemTooltip("Borda SUPERIOR da janela. Unreal, verbatim: \"High Percent - a good starting range is 80 to 95\". Medido no histograma real das 5h50: mover de 5/95 para 75/85 mede 4,4x mais brilhante = 2,1 stops A MENOS de exposicao.");
        if (ImGui::Button("Medir agora (EV100 + histograma no log)")) { g_medir_pedido = true; } // 26-M
        ImGui::SliderFloat("EV100 min (limite do escuro)", &ev_min, -10.0f, 22.0f, "%.2f EV"); // 26-19: faixa aberta ate -10
        ImGui::SetItemTooltip("Quanto o medidor pode ABRIR. -8 = limite de captura de luz de estrelas (EV100 = log2(lux/pi)). Frostbite 5.1 NAO tem grampo nenhum: subir isto e uma decisao de ARTE sobre quao escura a noite tem direito de ser, nao um limite do instrumento.");
        ImGui::SliderFloat("EV100 max (limite do claro)", &ev_max, -10.0f, 22.0f, "%.2f EV"); // 26-19
        ImGui::SetItemTooltip("Quanto o medidor pode FECHAR. 18 = sol direto com margem. Sunny 16: o meio-dia deve cair perto de EV100 15, entao 18 nunca morde numa cena real.");
        ImGui::SliderFloat("Piso do ceu noturno (cd/m2)", &night_sky_floor, 0.0f, 0.01f, "%.5f"); // 26-2b: N3.1 = 0.0002
        ImGui::SliderFloat3("Cor do ceu noturno", night_tint, 0.0f, 1.0f); // 25-8: ARTE, nao vem da fonte
        ImGui::SliderFloat("Lua: fase (elongacao)", &moon_elong_deg, 0.0f, 360.0f, "%.0f graus"); // 25-9a: 0=nova, 180=cheia
        ImGui::SliderFloat("Lua: disco (raio, graus)", &moon_disk_deg, 0.05f, 3.0f, "%.3f");
        ImGui::SetItemTooltip("GEOMETRIA: 0.26 = real. A luminancia do disco NAO depende do raio, entao aumentar o raio MULTIPLICA a energia pelo quadrado. 3.0 emite 133x demais.");
        ImGui::SliderFloat("Lua: brilho (x fisico)", &moon_disk_bright, 0.0f, 4.0f, "%.2f"); // 26-2b: 1.0 = rho/pi * E_sol
        ImGui::SliderFloat("Lua: iluminancia (lux)", &moon_scene_bright, 0.0f, 5.0f, "%.3f lx"); // 26-2b: N2.4 -> 1.754
        ImGui::SliderFloat("Nevoa aerea (1 = fisico)", &aerial_str, 0.0f, 8.0f, "%.2f"); // 25-13
        ImGui::SliderFloat3("Lua: cor da luz", moon_tint, 0.0f, 1.0f);
        // M2.5-25-18: as estrelas
        ImGui::SliderFloat("Estrelas: forca (1 = fisico)", &stars_str, 0.0f, 4.0f, "%.2f");
        ImGui::SetItemTooltip("1.00 = a luminancia que a fotometria manda, sem ganho. A escala absoluta NAO e ajustavel e vive no shader: E(m) = 2.96606e-6 * 10^(-0.4 m) lux, derivada dos itens N3.1 (ceu sem lua = 2e-4 cd/m2 = 22,0 mag/arcsec2, tres fontes convergentes) e N3.4 (Pogson) da checklist da noite. Contraprova: a fotometria padrao da 2,54e-6 para m=0 e a nossa derivacao da 2,966e-6 - 0,22 stop entre dois caminhos independentes. Acima de 1.00 e ARTE declarada, nao fisica.");
        ImGui::SliderFloat("Estrelas: magnitude limite", &stars_mag_lim, 2.0f, 9.0f, "%.2f");
        ImGui::SetItemTooltip("A magnitude mais FRACA desenhada. 6.50 = o limite do olho nu a olho desarmado em ceu escuro. Numeros maiores acendem estrelas mais fracas (e mais numerosas); menores deixam so as brilhantes. Cada passo de 1 magnitude vale 2,512x em fluxo (Pogson).");
        ImGui::SliderFloat("Estrelas: tamanho (px)", &stars_size, 0.10f, 3.0f, "%.2f");
        ImGui::SetItemTooltip("Sigma do borrao gaussiano, em PIXELS, medido pelo proprio hardware com fwidth(dir). NAO muda o brilho da estrela: a luminancia de pico e E/(pi*sigma^2) e a integral sobre o borrao devolve E exatamente, para qualquer sigma - verificado numericamente, erro 0,000%%. Por isso o resultado nao depende da resolucao nem do campo de visao. Abaixo de ~0.5 as estrelas cintilam ao mover a camera (aliasing); acima de ~1.5 viram bolinhas.");
        ImGui::SliderFloat("Estrelas: densidade", &stars_dens, 4.0f, 200.0f, "%.0f");
        ImGui::SetItemTooltip("Celulas por lado de face do cubo. O total e 6*N^2: em 40 sao 9.600 estrelas. A grade e em FACE DE CUBO e nao em azimute/elevacao porque a segunda amontoa tudo nos polos; a face de cubo varia ~1,9x entre centro e canto, aproximacao declarada e invisivel contra a aleatoriedade da magnitude.");
        ImGui::SliderFloat("Estrelas: inclinacao da contagem", &stars_slope, 0.10f, 1.20f, "%.2f");
        ImGui::SetItemTooltip("LACUNA DECLARADA - este e o unico numero da estacao sem fonte no acervo. O numero de estrelas por magnitude segue N(<m) proporcional a 10^(a*m), e usamos a CDF inversa m = mag_lim + log10(u)/a. O 'a' entra como ARTE, com 0.50 de partida, e esta na fornada. Menor = mais estrelas brilhantes; maior = quase todas fracas. A N3.6 (catalogo Hipparcos/Tycho-2 x procedural) tambem segue aberta: isto e o caminho procedural.");
        ImGui::SliderFloat("Disco do sol (raio, graus)", &sun_disk_deg, 0.05f, 3.0f, "%.4f"); // 25-7: 0.2725 = real
        ImGui::SetItemTooltip("GEOMETRIA: 0.2725 = real (Frostbite 4.6 nota 28). Este disco divide por Omega, entao mudar o raio CONSERVA energia - so espalha.");
        ImGui::SliderFloat("Disco do sol: brilho (x fisico)", &sun_disk_bright, 0.0f, 1.0f, "%.4f"); // 26-2d: %.2f mostrava 0.005 como "0.00"
        { // M2.5-26-6b: a leitura tambem em EV100 - o numero decimal sozinho (2,6e-05)
          // nao diz nada ao olho; o stop diz. EV = log2(1/(1.2*exposicao)).
            // M2.5-26-17: com a automatica LIGADA nao existe numero honesto por frame
            // sem readback (e readback no caminho de render e proibido pela doutrina).
            // Entao mostramos o ultimo retrato, datado. Com a automatica DESLIGADA o
            // valor e exato: nasce do EV100 do slider, na CPU, neste frame.
            float e_eff = auto_exposure ? (float)g_med_expo : exposure;
            ImGui::Text("Exposicao efetiva: %.3e  (EV100 %.2f)", e_eff,
                        std::log2(1.0f / (1.2f * std::max(e_eff, 1e-30f))));
        }
        ImGui::SliderFloat("Shaft: oclusao de nuvem", &shaft_cloud_occ, 0.0f, 1.0f);

        ImGui::Separator();
        ImGui::SliderInt("Cortes", &toon_cuts, 2, 5);
        ImGui::SliderFloat("Suavidade", &toon_soft, 0.5f, 3.0f);
        ImGui::End();

        ImGui::Begin("Log");
        for (const std::string& line : app_log.lines) ImGui::TextUnformatted(line.c_str());
        if (app_log.auto_scroll) { ImGui::SetScrollHereY(1.0f); app_log.auto_scroll = false; }
        ImGui::End();

        if (game_seconds != last_displayed_second || time_scale != last_displayed_scale) {
            last_displayed_second = game_seconds;
            last_displayed_scale = time_scale;

            char title_buffer[256];
            if (time_scale == 0) {
                snprintf(title_buffer, sizeof(title_buffer), "Minha Engine | dia %llu %02llu:%02llu | tick %llu | pausado",
                    (unsigned long long)g_days, (unsigned long long)g_hours, (unsigned long long)g_minutes, (unsigned long long)sim_tick);
            }
            else {
                snprintf(title_buffer, sizeof(title_buffer), "Minha Engine | dia %llu %02llu:%02llu | tick %llu | %lldx",
                    (unsigned long long)g_days, (unsigned long long)g_hours, (unsigned long long)g_minutes, (unsigned long long)sim_tick, (long long)time_scale);
            }
            SDL_SetWindowTitle(window, title_buffer);
        }

        if (resize_needed && win_w > 0 && win_h > 0) {
            SDL_WaitForGPUIdle(device);
            if (depth_texture) SDL_ReleaseGPUTexture(device, depth_texture);
            if (scene_color) SDL_ReleaseGPUTexture(device, scene_color);
            if (normal_id) SDL_ReleaseGPUTexture(device, normal_id);
            if (composite_color) SDL_ReleaseGPUTexture(device, composite_color);

            if (pixel_mode == 0) {
                internal_w = std::max(1, win_w / pixel_divisor);
                internal_h = std::max(1, win_h / pixel_divisor);
            }
            else {
                internal_h = alturas[altura_idx];
                internal_w = std::max(2, (int)std::llround(internal_h * (double)win_w / (double)win_h)) & ~1;
            }
            if (pixel_mode == 2) {                 // M2.5-26-18: NATIVO
                internal_w = std::max(2, win_w) & ~1;
                internal_h = std::max(1, win_h);
            }

            depth_info.width = (uint32_t)internal_w;
            depth_info.height = (uint32_t)internal_h;
            depth_texture = SDL_CreateGPUTexture(device, &depth_info);

            scene_color_info.width = (uint32_t)internal_w;
            scene_color_info.height = (uint32_t)internal_h;
            scene_color = SDL_CreateGPUTexture(device, &scene_color_info);

            normal_id_info.width = (uint32_t)internal_w;
            normal_id_info.height = (uint32_t)internal_h;
            normal_id = SDL_CreateGPUTexture(device, &normal_id_info);

            comp_color_info.width = (uint32_t)internal_w;
            comp_color_info.height = (uint32_t)internal_h;
            composite_color = SDL_CreateGPUTexture(device, &comp_color_info);

            // M2.5-23: Bloom Rendertargets Dinamicos
            if (bloom_tex_a) SDL_ReleaseGPUTexture(device, bloom_tex_a);
            if (bloom_tex_b) SDL_ReleaseGPUTexture(device, bloom_tex_b);
            if (bloom_tex_c) SDL_ReleaseGPUTexture(device, bloom_tex_c);
            if (bloom_tex_d) SDL_ReleaseGPUTexture(device, bloom_tex_d);

            SDL_GPUTextureCreateInfo b_tex_info = {};
            b_tex_info.type = SDL_GPU_TEXTURETYPE_2D;
            b_tex_info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // M2.5-24I: bloom HDR
            b_tex_info.layer_count_or_depth = 1;
            b_tex_info.num_levels = 1;
            b_tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            b_tex_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

            b_tex_info.width = std::max(1, internal_w / 2); b_tex_info.height = std::max(1, internal_h / 2);
            bloom_tex_a = SDL_CreateGPUTexture(device, &b_tex_info);
            b_tex_info.width = std::max(1, internal_w / 4); b_tex_info.height = std::max(1, internal_h / 4);
            bloom_tex_b = SDL_CreateGPUTexture(device, &b_tex_info);
            b_tex_info.width = std::max(1, internal_w / 8); b_tex_info.height = std::max(1, internal_h / 8);
            bloom_tex_c = SDL_CreateGPUTexture(device, &b_tex_info);
            b_tex_info.width = std::max(1, internal_w / 16); b_tex_info.height = std::max(1, internal_h / 16);
            bloom_tex_d = SDL_CreateGPUTexture(device, &b_tex_info);

            app_log.add("Res interna: %dx%d", internal_w, internal_h);

            resize_needed = false;
        }

        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();

        SDL_GPUCommandBuffer* cmd_buffer = SDL_AcquireGPUCommandBuffer(device);
        if (!cmd_buffer) continue;

        if (last_ticks_this_frame > 0 || first_sync) {
            uint64_t sync_start_timer = SDL_GetPerformanceCounter();

            InstanceData* mapped_instances = (InstanceData*)SDL_MapGPUTransferBuffer(device, dyn_transfer_buf, true);
            for (size_t i = 0; i < world.count; i++) {
                mapped_instances[i].x = world.position[i].x;
                mapped_instances[i].y = world.position[i].y;
                mapped_instances[i].z = world.position[i].z;
                mapped_instances[i].r = world.color[i].x;
                mapped_instances[i].g = world.color[i].y;
                mapped_instances[i].b = world.color[i].z;
            }
            SDL_UnmapGPUTransferBuffer(device, dyn_transfer_buf);

            InstanceData* mapped_char = (InstanceData*)SDL_MapGPUTransferBuffer(device, char_transfer_buf, true);

            mapped_char[0].x = interp_player.x;
            mapped_char[0].y = interp_player.y;
            mapped_char[0].z = interp_player.z;
            mapped_char[0].r = 1.0f; mapped_char[0].g = 1.0f; mapped_char[0].b = 1.0f;

            mapped_char[1].x = interp_plat.x;
            mapped_char[1].y = interp_plat.y;
            mapped_char[1].z = interp_plat.z;
            mapped_char[1].r = 0.8f; mapped_char[1].g = 0.5f; mapped_char[1].b = 0.1f;

            SDL_UnmapGPUTransferBuffer(device, char_transfer_buf);

            InstanceData* mapped_npc = (InstanceData*)SDL_MapGPUTransferBuffer(device, npc_transfer_buf, true);
            for (int i = 0; i < 5; ++i) {
                float ix = glm::mix(prev_npcs[i].x, npcs[i].x, alpha);
                float iz = glm::mix(prev_npcs[i].z, npcs[i].z, alpha);

                if (npc_visuals[i].has_mesh) {
                    mapped_npc[i] = { ix, 0.0f, iz, 1.0f, 1.0f, 1.0f };
                }
                else {
                    mapped_npc[i] = { ix, 0.85f, iz, npc_colors[i][0], npc_colors[i][1], npc_colors[i][2] };
                }
            }
            SDL_UnmapGPUTransferBuffer(device, npc_transfer_buf);

            SDL_GPUCopyPass* char_copy_pass = SDL_BeginGPUCopyPass(cmd_buffer);

            if (draw_crowd) {
                SDL_GPUTransferBufferLocation loc = { dyn_transfer_buf, 0 };
                SDL_GPUBufferRegion reg = { crowd_instance_buffer, 0, (uint32_t)(world.count * sizeof(InstanceData)) };
                SDL_UploadToGPUBuffer(char_copy_pass, &loc, &reg, true);
            }

            SDL_GPUTransferBufferLocation char_loc = { char_transfer_buf, 0 };
            SDL_GPUBufferRegion char_reg = { char_instance_buffer, 0, 2 * sizeof(InstanceData) };
            SDL_UploadToGPUBuffer(char_copy_pass, &char_loc, &char_reg, true);

            SDL_GPUTransferBufferLocation npc_loc = { npc_transfer_buf, 0 };
            SDL_GPUBufferRegion npc_reg = { npc_instance_buffer, 0, 5 * sizeof(InstanceData) };
            SDL_UploadToGPUBuffer(char_copy_pass, &npc_loc, &npc_reg, true);

            SDL_EndGPUCopyPass(char_copy_pass);

            uint64_t sync_end_timer = SDL_GetPerformanceCounter();
            last_sync_ms = (double)(sync_end_timer - sync_start_timer) * 1000.0 / frequency;

            first_sync = false;
        }

        SDL_GPUTexture* swapchain_texture = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_buffer, window, &swapchain_texture, nullptr, nullptr)) {
            SDL_SubmitGPUCommandBuffer(cmd_buffer);
            continue;
        }

        if (swapchain_texture && win_w > 0 && win_h > 0) {
            float aspect = (float)internal_w / (float)internal_h;
            glm::mat4 proj = glm::perspectiveLH_ZO(glm::radians(70.0f), aspect, 0.1f, 500.0f);
            glm::mat4 view = glm::lookAtLH(cam_pos, cam_pos + front, glm::vec3(0, 1, 0));
            glm::mat4 mvp = proj * view;

            SDL_PushGPUVertexUniformData(cmd_buffer, 0, glm::value_ptr(mvp), sizeof(mvp));

            glm::vec3 to_sun_vec;
            glm::vec3 to_moon_vec; // M2.5-25-9a
            // M2.5-26-2a: a exposicao do quadro ANTERIOR, que sera pre-aplicada por
            // todos os shaders de iluminacao deste quadro (Frostbite 5.2 / Listing 31).
            // Um valor SO para o quadro inteiro: cena, ceu, grama, shafts e o medidor
            // tem de concordar, senao a conta de desfazer a pre-exposicao nao fecha.
            SDL_GPUTexture* expo_prev = expo_tex[1 - expo_ping];
            float effective_intensity = sun_intensity;
            float effective_ambient = sun_ambient;

            if (sol_automatico) {
                // M2.5-26-5: RELOGIO CONTINUO para a posicao dos astros.
                // g_hours/g_minutes sao inteiros (game_minutes_total = sim_tick/120),
                // entao o sol e a lua saltavam 0,25 grau de uma vez a cada 2 segundos
                // reais e ficavam parados no meio - o "movimento duro" que a retina
                // pegou. A posicao dos astros e APRESENTACAO: nao entra no tick, nao
                // entra no golden hash. O relogio inteiro segue mandando na simulacao,
                // na UI e nas agendas dos NPCs.
                float t_frac = (float)(std::fmod((double)sim_tick / 120.0, 1440.0) / 1440.0);
                if (sol_hora_manual) t_frac = sol_hora / 24.0f; // M2.5-24G-5: so a LUZ obedece; o tick segue vivo
                float theta = (t_frac - 0.25f) * 2.0f * 3.14159265f;
                float phi = glm::radians(arco_inclinacao);

                to_sun_vec = glm::normalize(glm::vec3(
                    std::cos(theta),
                    std::sin(theta) * std::cos(phi),
                    std::sin(theta) * std::sin(phi)
                ));

                // M2.5-25-9a: a lua percorre a MESMA circunferencia do sol, deslocada da
                // elongacao. Isso reproduz a fase por construcao E faz a lua cheia nascer no
                // poente, como na natureza. Simplificacao declarada: a orbita lunar real e
                // inclinada ~5 graus em relacao a eclitica; usamos a mesma inclinacao do arco.
                float theta_moon = theta + glm::radians(moon_elong_deg);
                to_moon_vec = glm::normalize(glm::vec3(
                    std::cos(theta_moon),
                    std::sin(theta_moon) * std::cos(phi),
                    std::sin(theta_moon) * std::sin(phi)
                ));

                // M2.5-25-F: sol_fator removido - era calculado aqui e SOBRESCRITO
                // pela linha da "Lua minima" logo abaixo. Codigo morto desde a origem.

                auto saturate = [](float x) { return std::max(0.0f, std::min(1.0f, x)); };
                // M2.5-25-F: a paleta do crepusculo foi DELETADA. Tudo o que ela
                // pintava (ceu, nevoa, desbotamento das nuvens) e fisico desde a 25-10.
                float night_w = saturate(-to_sun_vec.y);

                // M2.5-19C: Base lunar noturna - piso garantido de luz
                // M2.5-26-2b: a "Lua minima" da M2.5-19C MORREU aqui. Ela era um piso de
                // luz sem direcao, inventado quando nao existia lua no motor. A lua real
                // entrou na 25-9c e desde entao esse termo era uma SEGUNDA lua invisivel
                // que nunca se punha. Com o sol em lux, ela tambem sabotava a razao
                // sol/lua - a unica coisa que esta estacao existe para acertar.
                // A iluminancia do sol NAO muda com a hora: o que muda e a geometria e a
                // transmitancia, e as duas ja estao no modelo.
                (void)night_w;
            }
            else {
                float sy_rad = glm::radians(sun_yaw);
                float sp_rad = glm::radians(sun_pitch);
                to_sun_vec = glm::normalize(glm::vec3(
                    std::cos(sp_rad) * std::sin(sy_rad),
                    std::sin(sp_rad),
                    std::cos(sp_rad) * std::cos(sy_rad)
                ));

                to_moon_vec = -to_sun_vec; // 25-9a: sem orbita no modo manual, lua cheia oposta
                effective_ambient = sun_ambient;
            }

            // A luz vem DO sol PARA O CHAO (convencao da iluminacao do cenario mantida)
            // M2.5-25-1: sol fisico por hora do dia (Hillaire 2020) + interim noturno neutro (ate a lua do S7)
            // M2.5-25-11: transmitancia do sol PURAMENTE FISICA.
            // O interino da 25-1 (que empurrava t_sol para branco puro conforme o sol
            // afundava) foi REMOVIDO: ele era o inverso da fisica e produzia um degrau
            // de ~11x na iluminacao da cena bem no nascer/poente. Ele existia so para
            // segurar a noite ate a lua existir - e a lua chegou na 25-9c.
            glm::vec3 t_sol = sun_transmittance(to_sun_vec.y);
            // ================= M2.5-25-9c: O ASTRO DOMINANTE =================
            // A luz direcional da CENA deixa de ser sempre o sol. Sol enquanto ele
            // esta no ceu; lua quando ela domina. Direcao, cor, intensidade e o
            // SHADOW MAP acompanham - o luar projeta sombra na direcao dela.
            // Hillaire 2020 Â§3 trata sol e lua como N luzes direcionais; aqui a lua
            // SUBSTITUI em vez de somar, porque os shaders da cena tem uma luz so.
            // Simplificacao declarada: as duas quase nunca dominam ao mesmo tempo.
            glm::vec3 t_moon = sun_transmittance(to_moon_vec.y); // o luar tambem avermelha no horizonte

            // Lei de fase da esfera lambertiana: f(a) = (sin a + (pi - a) cos a) / pi
            // a = angulo de fase (Sol-Lua-observador) = 180 - elongacao.
            // Cheia -> 1 Â· quarto -> 1/pi ~ 0.318 Â· nova -> 0.
            // A Lua real desvia disto (Hapke); a tese CosmoScout chama esta forma de
            // "Ideal Diffuse Moon" e a documenta como alternativa valida. DECLARADO.
            float elong = std::acos(std::clamp(glm::dot(to_sun_vec, to_moon_vec), -1.0f, 1.0f));
            float fase_a = 3.14159265f - elong;
            float moon_phase_f = (std::sin(fase_a) + (3.14159265f - fase_a) * std::cos(fase_a)) / 3.14159265f;

            float sun_up_w  = glm::smoothstep(-0.06f, 0.03f, to_sun_vec.y);
            float moon_up_w = glm::smoothstep(-0.02f, 0.06f, to_moon_vec.y);

            float I_sol = sun_intensity * sun_up_w;
            float I_lua = moon_scene_bright * moon_phase_f * moon_up_w * (1.0f - sun_up_w);

            bool lua_manda = (I_lua > I_sol);
            glm::vec3 astro_dir = lua_manda ? to_moon_vec : to_sun_vec;
            glm::vec3 astro_col = lua_manda
                ? glm::vec3(moon_tint[0], moon_tint[1], moon_tint[2]) * t_moon * I_lua
                : glm::vec3(sun_color_arr[0], sun_color_arr[1], sun_color_arr[2]) * t_sol * I_sol;

            glm::vec3 s_dir = -astro_dir;

            glm::vec3 c_cloud_day(0.95f, 0.95f, 0.95f);
            glm::vec3 c_cloud_sunset(0.95f, 0.85f, 0.70f);
            glm::vec3 c_cloud_night(0.08f, 0.1f, 0.15f);


            glm::vec3 c_cloud_shad_day(0.55f, 0.60f, 0.75f); // Azul lavanda opaco
            glm::vec3 c_cloud_shad_sunset(0.50f, 0.35f, 0.40f); // Roxo quente
            glm::vec3 c_cloud_shad_night(0.01f, 0.01f, 0.04f); // Azul escuro profundo

            glm::vec3 effective_cloud_color = c_cloud_day;
            glm::vec3 effective_cloud_shad = c_cloud_shad_day;

            // M2.5-24: Computacao da Matriz Orto p/ Sombra
            glm::mat4 light_vp = glm::mat4(1.0f);
            if (draw_shadows && astro_dir.y > 0.01f) { // 25-9c: a lua tambem projeta sombra
                float ortho_size = 340.0f; // cobre a geometria de teste em x=300
                glm::mat4 light_proj = glm::orthoLH_ZO(-ortho_size, ortho_size, -ortho_size, ortho_size, -300.0f, 300.0f);
                glm::mat4 light_view = glm::lookAtLH(astro_dir * 200.0f, glm::vec3(0.0f), glm::vec3(0, 1, 0)); // 25-9c
                light_vp = light_proj * light_view;

                glm::vec3 origin_ndc = glm::vec3(light_vp * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                float texel_ndc = 2.0f / 2048.0f;
                float snap_x = std::fmod(origin_ndc.x, texel_ndc);
                float snap_y = std::fmod(origin_ndc.y, texel_ndc);
                light_proj[3][0] -= snap_x;
                light_proj[3][1] -= snap_y;
                light_vp = light_proj * light_view;

                SDL_GPUDepthStencilTargetInfo shadow_target = {};
                shadow_target.texture = shadow_map;
                shadow_target.clear_depth = 1.0f;
                shadow_target.load_op = SDL_GPU_LOADOP_CLEAR;
                shadow_target.store_op = SDL_GPU_STOREOP_STORE;
                shadow_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
                shadow_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
                shadow_target.cycle = true;

                SDL_GPURenderPass* spass = SDL_BeginGPURenderPass(cmd_buffer, nullptr, 0, &shadow_target);
                SDL_BindGPUGraphicsPipeline(spass, shadow_pipeline);
                SDL_PushGPUVertexUniformData(cmd_buffer, 0, glm::value_ptr(light_vp), sizeof(light_vp));

                SDL_GPUBufferBinding vb_bind_s = { vertex_buffer, 0 };
                SDL_BindGPUVertexBuffers(spass, 0, &vb_bind_s, 1);
                SDL_GPUBufferBinding ib_bind_s = { index_buffer, 0 };
                SDL_BindGPUIndexBuffer(spass, &ib_bind_s, SDL_GPU_INDEXELEMENTSIZE_16BIT);

                SDL_GPUBufferBinding inst_bind_floor_s = { neutral_instance_buffer, 0 };
                SDL_BindGPUVertexBuffers(spass, 1, &inst_bind_floor_s, 1);
                for (size_t i = 0; i < box_ranges.size(); ++i) {
                    SDL_DrawGPUIndexedPrimitives(spass, box_ranges[i].index_count, 1, box_ranges[i].first_index, 0, 0);
                }
                SDL_DrawGPUIndexedPrimitives(spass, tortura_range.index_count, 1, tortura_range.first_index, 0, 0);

                for (const auto& a : level_assets) {
                    SDL_DrawGPUIndexedPrimitives(spass, a.range.index_count, 1, a.range.first_index, 0, 0);
                }

                if (draw_crowd) {
                    SDL_GPUBufferBinding inst_bind_crowd_s = { crowd_instance_buffer, 0 };
                    SDL_BindGPUVertexBuffers(spass, 1, &inst_bind_crowd_s, 1);
                    SDL_DrawGPUIndexedPrimitives(spass, cubo_multidao.index_count, 100000, cubo_multidao.first_index, 0, 0);
                }

                SDL_GPUBufferBinding inst_bind_char_s = { char_instance_buffer, 0 };
                SDL_BindGPUVertexBuffers(spass, 1, &inst_bind_char_s, 1);
                MeshRange range_active_player = is_crouched ? cubo_jogador_agachado : cubo_jogador;
                SDL_DrawGPUIndexedPrimitives(spass, range_active_player.index_count, 1, range_active_player.first_index, 0, 0);
                SDL_DrawGPUIndexedPrimitives(spass, cubo_plataforma.index_count, 1, cubo_plataforma.first_index, 0, 1);

                SDL_GPUBufferBinding inst_bind_npc_s = { npc_instance_buffer, 0 };
                SDL_BindGPUVertexBuffers(spass, 1, &inst_bind_npc_s, 1);
                for (int i = 0; i < 5; ++i) {
                    if (npc_visuals[i].has_mesh) {
                        SDL_DrawGPUIndexedPrimitives(spass, npc_visuals[i].range.index_count, 1, npc_visuals[i].range.first_index, 0, i);
                    }
                    else {
                        SDL_DrawGPUIndexedPrimitives(spass, cubo_npc.index_count, 1, cubo_npc.first_index, 0, i);
                    }
                }

                SDL_EndGPURenderPass(spass);
            }

            if (sol_automatico) {
                auto saturate = [](float x) { return std::max(0.0f, std::min(1.0f, x)); };
                float day_w = saturate(to_sun_vec.y);
                float sunset_w = saturate(1.0f - std::fabs(to_sun_vec.y));
                float night_w = saturate(-to_sun_vec.y);

                effective_cloud_color = day_w * c_cloud_day + sunset_w * c_cloud_sunset + night_w * c_cloud_night;
                effective_cloud_shad = day_w * c_cloud_shad_day + sunset_w * c_cloud_shad_sunset + night_w * c_cloud_shad_night;
            }

            SkyParams sky_params;
            sky_params.to_sun[0] = to_sun_vec.x;
            sky_params.to_sun[1] = to_sun_vec.y;
            sky_params.to_sun[2] = to_sun_vec.z;
            sky_params.pad0 = 0.0f;
            sky_params.sun_color[0] = sun_color_arr[0] * t_sol.x * effective_intensity; // 25-1
            sky_params.sun_color[1] = sun_color_arr[1] * t_sol.y * effective_intensity;
            sky_params.sun_color[2] = sun_color_arr[2] * t_sol.z * effective_intensity;
            sky_params.pad3 = 0.0f;

            sky_params.cam_pos[0] = cam_pos.x;
            sky_params.cam_pos[1] = cam_pos.y;
            sky_params.cam_pos[2] = cam_pos.z;
            sky_params.pad4 = 0.0f;

            float w_rad = glm::radians(wind::dir_deg);
            sky_params.wind_dir[0] = cos(w_rad);
            sky_params.wind_dir[1] = sin(w_rad);
            sky_params.time = (float)(sim_tick * DT);
            sky_params.cloud_speed = cloud_speed;

            sky_params.cloud_color[0] = effective_cloud_color.r;
            sky_params.cloud_color[1] = effective_cloud_color.g;
            sky_params.cloud_color[2] = effective_cloud_color.b;
            sky_params.cobertura = draw_clouds ? cloud_cobertura : -2.0f;

            sky_params.densidade = cloud_densidade;
            sky_params.escala = cloud_escala;
            sky_params.h_min = cloud_h_min;
            sky_params.h_max = cloud_h_max;

            sky_params.cap = cloud_cap;
            sky_params.passos = (float)cloud_passos;
            sky_params.absorcao = cloud_absorcao;
            sky_params.hg_g_frontal = cloud_hg_g_frontal;
            sky_params.powder_str = cloud_powder_str;
            sky_params.sun_intensity = effective_intensity;
            sky_params.sun_ambient = effective_ambient;
            sky_params.cloud_type = cloud_type_blend;
            sky_params.erosao_str = cloud_erosao_str;

            sky_params.cloud_silhouette = cloud_silhouette;
            sky_params.cloud_bands = cloud_bands;
            sky_params.cloud_ambient_str = cloud_ambient_str;

            // M2.5-25-3 (S10): a nuvem bebe do CEU FISICO. A paleta hardcoded do
            // ambiente sai de cena de dia; o crepusculo passa a tingir as nuvens
            // sozinho, pela mesma atmosfera que pinta o ceu. Mesmo ganho de unidades.
            // Interim: abaixo do horizonte a paleta antiga volta, ate a lua (S7).
            // M2.5-25-12 (B): o interino amb_noite_w MORREU. O ambiente das nuvens
            // passa a ser a SOMA DAS FONTES, como a Eq. 11 do Hillaire soma sobre N
            // luzes: hemisferio iluminado pelo sol + hemisferio iluminado pela lua +
            // piso noturno. A paleta antiga (c_sky_amb_night, quase preta) sai de cena.
            float atmo_amb[3], atmo_amb_lua[3];
            atmo::sky_ambient(to_sun_vec.y, atmo_amb);
            atmo::sky_ambient(to_moon_vec.y, atmo_amb_lua);
            for (int ci = 0; ci < 3; ++ci)
            {
                float por_sol = atmo_amb[ci] * sun_color_arr[ci] * effective_intensity;
                float por_lua = atmo_amb_lua[ci] * moon_tint[ci] * I_lua;
                sky_params.sky_ambient_color[ci] = (por_sol + por_lua + night_sky_floor * night_tint[ci]) * sky_gain;
            }

            // M2.5-25-4: EXPOSICAO AUTOMATICA ANALITICA.
            // Pratica de producao: Lagarde & de Rousiers, "Moving Frostbite to PBR"
            // (2014) - auto-exposure com chave sobre a luminancia media (Reinhard).
            // ADAPTACAO DA CASA: a Frostbite MEDE o frame anterior; nos CALCULAMOS a
            // iluminancia do sol e do ceu, porque a temporada 25 nos deu essa fisica.
            // Sem readback GPU->CPU (regra da doutrina), sem latencia, sem reducao nova.
            {
                const float kR = 0.2126f, kG = 0.7152f, kB = 0.0722f; // luma Rec.709
                // M2.5-25-16: o estimador le as MESMAS grandezas que o renderizador
                // usa neste frame, em vez de recalcular a partir do sol. Antes ele nao
                // enxergava a lua (que entrou em 25-9c/25-12/25-15) e a noite reportava
                // luminancia ~0, cravando a exposicao no teto - o mesmo numero do
                // meio-dia era usado a meia-noite. Agora qualquer luz nova aparece de
                // graca, porque ele mede o que existe.
                //   astro_col / astro_dir        -> a luz direcional da cena (sol ou lua)
                //   sky_params.sky_ambient_color -> o ceu (sol + lua + piso), com o ganho
                // Continua sendo estimativa: a fonte (Frostbite 2014, sec. 5.1) manda MEDIR
                // Lavg por histograma. Isto e coerencia, nao absolvicao - a remocao do
                // estimador analitico segue marcada para a 26-4.
                // M2.5-26-17: O SEGUNDO AVALIADOR MORREU AQUI.
                // O comentario acima ja marcava a remocao "para a 26-4" e ela nunca
                // aconteceu. Este bloco calculava uma exposicao ANALITICA completa -
                // media de cena, alvo, clamp, adaptacao no tempo - e o resultado ia
                // para exatamente UM lugar: o texto "Exposicao efetiva" do painel.
                // Nao era o que renderizava. O que renderiza vem da textura 1x1 medida
                // na GPU desde a 26-4.
                // CUSTO REAL: o painel mostrou o numero errado por quatro estacoes, e
                // eu consertei as coisas erradas olhando para ele. Um medidor que mente
                // e pior que nenhum, porque produz decisao confiante e errada.
                // Doutrina 24F-1: avaliador UNICO.
                (void)kR; (void)kG; (void)kB;
            }
            sky_params.detail_str = cloud_detail_str;

            sky_params.cloud_shadow_color[0] = effective_cloud_shad.r;
            sky_params.cloud_shadow_color[1] = effective_cloud_shad.g;
            sky_params.cloud_shadow_color[2] = effective_cloud_shad.b;
            sky_params.curl_str = cloud_curl_str;
            sky_params.silver_k = cloud_silver_k;
            sky_params.silver_spread = cloud_silver_spread;
            sky_params.clima_cov_teto = clima_cov_teto;
            sky_params.clima_type_teto = clima_type_teto;
            sky_params.curvatura = cloud_curvatura;
            sky_params.fade_ini = cloud_fade_ini;
            sky_params.passo_far = cloud_passo_far; sky_params.cloud_ms_str = cloud_ms_str; sky_params.cloud_ms_g2 = cloud_ms_g2;
            sky_params.cloud_iso_str = cloud_iso_str;
            sky_params.sky_gain = sky_gain; sky_params.atmo_debug = (float)atmo_debug_mode;
            // 25-2: SEM t_sol aqui - a marcha da atmosfera calcula a propria transmitancia
            sky_params.sun_illum[0] = sun_color_arr[0] * effective_intensity;
            sky_params.sun_illum[1] = sun_color_arr[1] * effective_intensity;
            sky_params.sun_illum[2] = sun_color_arr[2] * effective_intensity; sky_params.night_floor = night_sky_floor;
            sky_params.sun_disk_deg = sun_disk_deg; sky_params.sun_disk_bright = sun_disk_bright;
            sky_params.stars_str = stars_str; sky_params.stars_mag_lim = stars_mag_lim; // 25-18
            sky_params.night_tint[0] = night_tint[0]; sky_params.night_tint[1] = night_tint[1];
            sky_params.night_tint[2] = night_tint[2]; sky_params.pad_n = 0.0f;
            sky_params.to_moon[0] = to_moon_vec.x; sky_params.to_moon[1] = to_moon_vec.y;
            sky_params.to_moon[2] = to_moon_vec.z; sky_params.moon_disk_deg = moon_disk_deg;
            sky_params.moon_disk_bright = moon_disk_bright;
            sky_params.stars_size = stars_size; sky_params.stars_dens = stars_dens;
            sky_params.stars_slope = stars_slope; // 25-18

            // M2.5-25-12 (A): o ASTRO DOMINANTE tambem ilumina as nuvens - o mesmo
            // que a cena usa desde a 25-9c, entao chao e nuvem concordam sobre de onde
            // vem a luz. De dia isto e IDENTICO ao que havia: to_sun e o produto
            // sun_color * sun_intensity, que era o que o direct_term ja multiplicava.
            sky_params.to_astro[0] = astro_dir.x;
            sky_params.to_astro[1] = astro_dir.y;
            sky_params.to_astro[2] = astro_dir.z;
            sky_params.pad_p = 0.0f;
            for (int ci = 0; ci < 3; ++ci)
            {
                // M2.5-26-2d: O QUADRADO. Esta linha multiplicava por effective_intensity
                // DUAS VEZES desde a 25-12. Com a regua antiga (intensidade ~1,2) o erro
                // valia 1,44x - some dentro de qualquer calibracao de nuvem e ninguem ve.
                // Com o sol em lux passou a valer 116.216x, e a luz direta das nuvens
                // virou 1,35x10^10 cd/m2. Como astro_sky_col so usa por_sol quando o SOL
                // manda, o estrago aparecia exatamente ao nascer do sol.
                // Licao: bug latente escondido por um valor perto de 1. Trocar a regua
                // nao criou o defeito - revelou. Vale reler as outras linhas de luz com
                // esse olho antes de fechar a temporada.
                float por_sol = sun_color_arr[ci] * (&t_sol.x)[ci] * effective_intensity;
                float por_lua = moon_tint[ci] * (&t_moon.x)[ci] * I_lua;
                sky_params.astro_sky_col[ci] = lua_manda ? por_lua : por_sol;
            }
            sky_params.aerial_str = aerial_str; // 25-13
            // 25-15: iluminancia CRUA da lua (a marcha calcula a propria transmitancia,
            // como ja acontece com o sol em sun_illum)
            sky_params.moon_illum[0] = moon_tint[0] * I_lua;
            sky_params.moon_illum[1] = moon_tint[1] * I_lua;
            sky_params.moon_illum[2] = moon_tint[2] * I_lua;
            sky_params.pad_r = 0.0f;

            SunParams sun_params;
            sun_params.dir[0] = s_dir.x;
            sun_params.dir[1] = s_dir.y;
            sun_params.dir[2] = s_dir.z;
            sun_params.ambient = effective_ambient;
            // M2.5-26-7: o ceu entra como FONTE. sky_ambient_color ja e a luminancia
            // media do hemisferio (atmo::sky_ambient, marcha do Hillaire), calculada
            // acima nesta mesma funcao e ja usada pelas nuvens desde a 25-3.
            sun_params.sky_light[0] = sky_params.sky_ambient_color[0];
            sun_params.sky_light[1] = sky_params.sky_ambient_color[1];
            sun_params.sky_light[2] = sky_params.sky_ambient_color[2];
            sun_params.sky_light_str = sky_light_str;
            // M2.5-26-8: a cena marcha a MESMA atmosfera do ceu. Copiado de
            // sky_params (ja preenchido acima) e nao recalculado, de proposito:
            // um valor so, uma fonte so.
            for (int k = 0; k < 3; ++k) {
                sun_params.ap_to_sun[k]     = sky_params.to_sun[k];
                sun_params.ap_sun_illum[k]  = sky_params.sun_illum[k];
                sun_params.ap_to_moon[k]    = sky_params.to_moon[k];
                sun_params.ap_moon_illum[k] = sky_params.moon_illum[k];
            }
            sun_params.ap_steps = (float)aerial_steps;
            sun_params.ap_str   = aerial_cena_str;
            sun_params.ap_on    = aerial_cena ? 1.0f : 0.0f;
            sun_params.ap_pad   = 0.0f;
            // M2.5-25-6 (BUG A): a CENA passa a usar effective_intensity, como o ceu,
            // as nuvens e os shafts ja usavam. A "Lua minima" (0.1 a noite, calculada
            // logo acima) era jogada fora aqui - a cena rodava com a intensidade solar
            // de meio-dia durante a madrugada.
            // M2.5-25-9c: a cor/intensidade vem do ASTRO DOMINANTE (sol ou lua)
            sun_params.color[0] = astro_col.r;
            sun_params.color[1] = astro_col.g;
            sun_params.color[2] = astro_col.b;
            sun_params.cam_pos[0] = cam_pos.x;
            sun_params.cam_pos[1] = cam_pos.y;
            sun_params.cam_pos[2] = cam_pos.z;
            sun_params.toon_cuts = (float)toon_cuts;
            sun_params.toon_soft = toon_soft;
            sun_params.wind_dir[0] = cos(w_rad);
            sun_params.wind_dir[1] = sin(w_rad);
            // M2.5-25-10: a nevoa da cena bebe do CEU FISICO. A paleta hardcoded do
            // crepusculo morre aqui: os pesos dela davam 50% de laranja de poente com
            // o sol 30 graus ABAIXO do horizonte, e lavavam o meio-dia de azul.
            // Mesma formula do ceu: fisico + piso noturno, tudo vezes o ganho.
            // ESCOPO: corrige a COR. A integracao por distancia (perspectiva aerea,
            // Hillaire 5.4) continua na fila.
            {
                float atmo_hz[3];
                atmo::sky_horizon(to_sun_vec.y, atmo_hz);
                for (int ci = 0; ci < 3; ++ci)
                {
                    float fisico = atmo_hz[ci] * sun_color_arr[ci] * effective_intensity;
                    sun_params.fog_color[ci] = (fisico + night_sky_floor * night_tint[ci]) * sky_gain;
                }
            }
            sun_params.fog_density = fog_density;
            sun_params.cloud_time = (float)(sim_tick * DT) * cloud_speed; // sombra anda NA velocidade da nuvem
            sun_params.cloud_cover = draw_clouds ? cloud_cobertura : -2.0f;
            sun_params.shadow_str = shadow_str;
            sun_params.grass_backlight = grass_backlight;
            sun_params.grass_specular = grass_specular;
            sun_params.obj_shadow_str = draw_shadows ? obj_shadow_str : 0.0f;
            sun_params.obj_shadow_bias = obj_shadow_bias;
            sun_params.debug_shadows = debug_shadows ? 1.0f : 0.0f;
            sun_params.pad_0[0] = 0.0f; sun_params.pad_0[1] = 0.0f;
            memcpy(sun_params.light_vp, glm::value_ptr(light_vp), 64);

            ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd_buffer);

            SDL_GPUColorTargetInfo color_targets[2] = {};
            color_targets[0].texture = scene_color;
            color_targets[0].clear_color = { 0.05f, 0.07f, 0.12f, 1.0f };
            color_targets[0].load_op = SDL_GPU_LOADOP_CLEAR;
            color_targets[0].store_op = SDL_GPU_STOREOP_STORE;

            color_targets[1].texture = normal_id;
            color_targets[1].clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };
            color_targets[1].load_op = SDL_GPU_LOADOP_CLEAR;
            color_targets[1].store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPUDepthStencilTargetInfo depth_target = {};
            depth_target.texture = depth_texture;
            depth_target.clear_depth = 1.0f;
            depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
            depth_target.store_op = SDL_GPU_STOREOP_STORE;
            depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            depth_target.cycle = false;

            SDL_GPURenderPass* scene_pass = SDL_BeginGPURenderPass(cmd_buffer, color_targets, 2, &depth_target);

            if (draw_sky) {
                SDL_BindGPUGraphicsPipeline(scene_pass, sky_pipeline);

                glm::mat4 sky_model = glm::translate(glm::mat4(1.0f), cam_pos) * glm::scale(glm::mat4(1.0f), glm::vec3(10.0f));
                glm::mat4 sky_mvp = proj * view * sky_model;
                SDL_PushGPUVertexUniformData(cmd_buffer, 0, glm::value_ptr(sky_mvp), sizeof(sky_mvp));
                SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &sky_params, sizeof(SkyParams));

                SDL_GPUBufferBinding sky_vb_bind = { dome_vertex_buffer, 0 };
                SDL_BindGPUVertexBuffers(scene_pass, 0, &sky_vb_bind, 1);

                SDL_GPUBufferBinding sky_ib_bind = { dome_index_buffer, 0 };
                SDL_BindGPUIndexBuffer(scene_pass, &sky_ib_bind, SDL_GPU_INDEXELEMENTSIZE_16BIT);

                SDL_GPUTextureSamplerBinding sky_samps[6] = {
                    { weather_tex, sampler }, // M2.5-24G-1: weather map no t0 do ceu
                    { cloud_tex, cloud_sampler },
                    { cloud_detail_tex, cloud_sampler },
                    { trans_tex, lut_sampler }, // M2.5-25-2: t3 = transmitancia
                    { ms_tex, lut_sampler },    // M2.5-25-2: t4 = multi-scattering
                    { expo_prev, upscale_sampler } // M2.5-26-2a: t5 = exposicao anterior
                };
                SDL_BindGPUFragmentSamplers(scene_pass, 0, sky_samps, 6);

                SDL_DrawGPUIndexedPrimitives(scene_pass, dome_indices.size(), 1, 0, 0, 0);
            }

            SDL_BindGPUGraphicsPipeline(scene_pass, pipeline);
            SDL_PushGPUVertexUniformData(cmd_buffer, 0, glm::value_ptr(mvp), sizeof(mvp)); // restaura o MVP da cena (o ceu sobrescreveu o slot 0)

            SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &sun_params, sizeof(SunParams));

            SDL_GPUBufferBinding vb_bind = { vertex_buffer, 0 };
            SDL_BindGPUVertexBuffers(scene_pass, 0, &vb_bind, 1);

            SDL_GPUBufferBinding ib_bind = { index_buffer, 0 };
            SDL_BindGPUIndexBuffer(scene_pass, &ib_bind, SDL_GPU_INDEXELEMENTSIZE_16BIT);

            SDL_GPUBufferBinding inst_bind_floor = { neutral_instance_buffer, 0 };
            SDL_BindGPUVertexBuffers(scene_pass, 1, &inst_bind_floor, 1);

            auto bind_3_samps = [&](SDL_GPUTexture* color_tex) {
                // M2.5-24G-2: 4o par = weather map (sombra de nuvem sincronizada com o ceu)
                SDL_GPUTextureSamplerBinding samps[7] = {
                    { color_tex ? color_tex : fallback_tex, sampler },
                    { shadow_map, shadow_cmp },
                    { shadow_map, shadow_diag },
                    { weather_tex, sampler },
                    { expo_prev, upscale_sampler }, // M2.5-26-2a: t4 = exposicao anterior
                    { trans_tex, lut_sampler },     // M2.5-26-8: t5 = transmitancia
                    { ms_tex, lut_sampler }         // M2.5-26-8: t6 = multi-scattering
                };
                SDL_BindGPUFragmentSamplers(scene_pass, 0, samps, 7);
                };

            bind_3_samps(fallback_tex);

            ObjIdParams obj_id;

            int mesh_id_counter = 0;
            for (const auto& a : level_assets) {
                if (a.is_terrain) {
                    obj_id.id = 6.0f / 255.0f; // ID estrito para terreno
                }
                else {
                    obj_id.id = (float)(10 + (mesh_id_counter % 240)) / 255.0f;
                }
                obj_id.pad[0] = 0; obj_id.pad[1] = 0; obj_id.pad[2] = 0;
                SDL_PushGPUFragmentUniformData(cmd_buffer, 1, &obj_id, sizeof(ObjIdParams));

                bind_3_samps(a.tex);
                SDL_DrawGPUIndexedPrimitives(scene_pass, a.range.index_count, 1, a.range.first_index, 0, 0);
                mesh_id_counter++;
            }

            for (size_t i = 0; i < box_ranges.size(); ++i) {
                obj_id.id = (float)(10 + (mesh_id_counter % 240)) / 255.0f;
                obj_id.pad[0] = 0; obj_id.pad[1] = 0; obj_id.pad[2] = 0;
                SDL_PushGPUFragmentUniformData(cmd_buffer, 1, &obj_id, sizeof(ObjIdParams));
                SDL_DrawGPUIndexedPrimitives(scene_pass, box_ranges[i].index_count, 1, box_ranges[i].first_index, 0, 0);
                mesh_id_counter++;
            }

            obj_id.id = 1.0f / 255.0f;
            SDL_PushGPUFragmentUniformData(cmd_buffer, 1, &obj_id, sizeof(ObjIdParams));
            SDL_DrawGPUIndexedPrimitives(scene_pass, tortura_range.index_count, 1, tortura_range.first_index, 0, 0);

            bind_3_samps(fallback_tex);

            if (draw_crowd) {
                obj_id.id = 2.0f / 255.0f;
                SDL_PushGPUFragmentUniformData(cmd_buffer, 1, &obj_id, sizeof(ObjIdParams));
                SDL_GPUBufferBinding inst_bind_crowd = { crowd_instance_buffer, 0 };
                SDL_BindGPUVertexBuffers(scene_pass, 1, &inst_bind_crowd, 1);
                SDL_DrawGPUIndexedPrimitives(scene_pass, cubo_multidao.index_count, 100000, cubo_multidao.first_index, 0, 0);
            }

            obj_id.id = 3.0f / 255.0f;
            SDL_PushGPUFragmentUniformData(cmd_buffer, 1, &obj_id, sizeof(ObjIdParams));
            SDL_GPUBufferBinding inst_bind_char = { char_instance_buffer, 0 };
            SDL_BindGPUVertexBuffers(scene_pass, 1, &inst_bind_char, 1);

            MeshRange range_active_player = is_crouched ? cubo_jogador_agachado : cubo_jogador;
            SDL_DrawGPUIndexedPrimitives(scene_pass, range_active_player.index_count, 1, range_active_player.first_index, 0, 0);
            SDL_DrawGPUIndexedPrimitives(scene_pass, cubo_plataforma.index_count, 1, cubo_plataforma.first_index, 0, 1);

            obj_id.id = 4.0f / 255.0f;
            SDL_PushGPUFragmentUniformData(cmd_buffer, 1, &obj_id, sizeof(ObjIdParams));
            SDL_GPUBufferBinding inst_bind_npc = { npc_instance_buffer, 0 };
            SDL_BindGPUVertexBuffers(scene_pass, 1, &inst_bind_npc, 1);

            for (int i = 0; i < 5; ++i) {
                if (npc_visuals[i].has_mesh) {
                    bind_3_samps(npc_visuals[i].tex);
                    SDL_DrawGPUIndexedPrimitives(scene_pass, npc_visuals[i].range.index_count, 1, npc_visuals[i].range.first_index, 0, i);
                }
                else {
                    bind_3_samps(fallback_tex);
                    SDL_DrawGPUIndexedPrimitives(scene_pass, cubo_npc.index_count, 1, cubo_npc.first_index, 0, i);
                }
            }

            if (draw_grass && grass_inst_count > 0 && grass_instbuf) {
                SDL_BindGPUGraphicsPipeline(scene_pass, grass_pipeline);
                bind_3_samps(active_grass_tex);

                SDL_GPUBufferBinding g_vbs[2] = { { grass_vbuf, 0 }, { grass_instbuf, 0 } };
                SDL_BindGPUVertexBuffers(scene_pass, 0, g_vbs, 2);
                SDL_GPUBufferBinding g_ib = { grass_ibuf, 0 };
                SDL_BindGPUIndexBuffer(scene_pass, &g_ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);

                GrassVSParams gvs;
                memcpy(gvs.mvp, glm::value_ptr(mvp), 64);
                gvs.cam_pos[0] = cam_pos.x; gvs.cam_pos[1] = cam_pos.y; gvs.cam_pos[2] = cam_pos.z;
                gvs.scale = grass_scale;

                float w_rad = glm::radians(wind::dir_deg);
                gvs.wind_dir[0] = cos(w_rad);
                gvs.wind_dir[1] = sin(w_rad);
                gvs.wind_strength = wind::strength;
                gvs.time = (float)(sim_tick * DT);

                SDL_PushGPUVertexUniformData(cmd_buffer, 0, &gvs, sizeof(GrassVSParams));
                SDL_DrawGPUIndexedPrimitives(scene_pass, g_count, grass_inst_count, 0, 0, 0);

                // Restaura estado para o restante dos renders da fase
                SDL_BindGPUGraphicsPipeline(scene_pass, pipeline);
                SDL_PushGPUVertexUniformData(cmd_buffer, 0, glm::value_ptr(mvp), sizeof(mvp));
                SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &sun_params, sizeof(SunParams));
                SDL_GPUBufferBinding vb_r = { vertex_buffer, 0 };
                SDL_BindGPUVertexBuffers(scene_pass, 0, &vb_r, 1);
                SDL_GPUBufferBinding ib_r = { index_buffer, 0 };
                SDL_BindGPUIndexBuffer(scene_pass, &ib_r, SDL_GPU_INDEXELEMENTSIZE_16BIT);
                SDL_GPUBufferBinding in_r = { neutral_instance_buffer, 0 };
                SDL_BindGPUVertexBuffers(scene_pass, 1, &in_r, 1);
            }

            SDL_EndGPURenderPass(scene_pass);

            SDL_GPUColorTargetInfo outline_target = {};
            outline_target.texture = composite_color;
            outline_target.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
            outline_target.load_op = SDL_GPU_LOADOP_CLEAR;
            outline_target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* o_pass = SDL_BeginGPURenderPass(cmd_buffer, &outline_target, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(o_pass, out_pipeline);

            OutlineParams op;
            // M2.5-26-18b: ESPESSURA INVARIANTE. O outline amostra os vizinhos a
            // UM texel, e foi calibrado com a interna em 360p - onde um texel virava
            // ~3 pixels de tela depois do upscale. Em nativo um texel E um pixel:
            // o contorno ficaria 3x mais fino e praticamente sumiria, e o z_cutoff
            // dispararia menos porque a diferenca de profundidade entre vizinhos
            // encolhe junto.
            // Referenciando a amostragem na grade de 360p, o deslocamento em UV fica
            // CONSTANTE em qualquer resolucao - logo a espessura aparente e o delta de
            // profundidade tambem ficam, e o z_cutoff continua valendo sem recalibrar.
            // Isso e o que torna o dial um experimento honesto: trocar a resolucao
            // passa a mudar so o DETALHE, nao a assinatura do tracado.
            const float kOutRefH = 360.0f;
            float esp_out = std::max(1.0f, (float)internal_h / kOutRefH);
            op.texel[0] = esp_out / (float)internal_w;
            op.texel[1] = esp_out / (float)internal_h;
            op.z_cutoff = out_z_cutoff;
            op.grazing_scale = out_grazing_scale;
            op.crease_low = out_crease_low;
            op.crease_high = out_crease_high;
            op.line_darken = out_line_darken;
            op.crease_brighten = out_crease_brighten;
            op.cam_fwd[0] = front.x;
            op.cam_fwd[1] = front.y;
            op.cam_fwd[2] = front.z;
            op.pad = 0.0f;
            SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &op, sizeof(OutlineParams));

            SDL_GPUTextureSamplerBinding o_samp_binds[3] = {
                { scene_color, point_sampler },
                { normal_id, point_sampler },
                { depth_texture, point_sampler }
            };
            SDL_BindGPUFragmentSamplers(o_pass, 0, o_samp_binds, 3);

            SDL_DrawGPUPrimitives(o_pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(o_pass);

            // ================================================================
            // M2.5-26-25: A MARCHA DOS SHAFTS, AGORA ANTES DO MEDIDOR.
            //
            // Roda entre o outline e o passe da exposicao - ou seja, o
            // composite_color ja carrega os shafts quando o medidor o le. Com
            // isso os CINCO consumidores que eram cegos passam a enxerga-los:
            // a exposicao, o bloom, o B do operador local, a grade, e o
            // [MEDIDA]/[GRADE]/[OPER].
            //
            // O load_op: em modo normal e LOAD, e o blend aditivo soma a marcha
            // no composite. No modo 3 da Vista (depuracao dos shafts) e CLEAR,
            // entao o composite passa a conter SO a marcha - a mesma vista de
            // antes, por outro mecanismo, e agora MENSURAVEL: com o modo 3
            // ligado o [MEDIDA] reporta os shafts em cd/m2.
            //
            // O gate: se a forca esta em zero o passe nao roda. Isso preserva o
            // comportamento antigo (o 'if (shaft_intensity > 0.001f)' que vivia
            // dentro do PSMain) e evita um passe de tela cheia a toa.
            // ================================================================
            float sh_forca = (view_mode == 0 || view_mode == 3) ? ray_strength : 0.0f;
            if (sh_forca > 0.001f)
            {
                ShaftBlock sb = {};
                glm::mat4 sh_inv_vp = glm::inverse(mvp);
                memcpy(sb.inv_viewproj, glm::value_ptr(sh_inv_vp), 64);
                memcpy(sb.light_vp, glm::value_ptr(light_vp), 64);
                sb.cam_pos[0] = cam_pos.x; sb.cam_pos[1] = cam_pos.y; sb.cam_pos[2] = cam_pos.z;
                sb.to_sun[0] = to_sun_vec.x; sb.to_sun[1] = to_sun_vec.y; sb.to_sun[2] = to_sun_vec.z;
                sb.shaft_intensity = sh_forca;
                sb.shaft_g = shaft_g;
                // MESMA expressao do fs_params (25-1): o gate que apaga a marcha
                // quando o sol afunda, vezes a transmitancia e a intensidade.
                float sh_gate = std::clamp((to_sun_vec.y + 0.02f) / 0.14f, 0.0f, 1.0f);
                sb.sun_color[0] = sun_color_arr[0] * t_sol.x * effective_intensity * sh_gate;
                sb.sun_color[1] = sun_color_arr[1] * t_sol.y * effective_intensity * sh_gate;
                sb.sun_color[2] = sun_color_arr[2] * t_sol.z * effective_intensity * sh_gate;
                sb.shaft_samples = (float)ray_samples;
                sb.wind_off[0] = cos(w_rad) * sun_params.cloud_time * -200.0f;
                sb.wind_off[1] = sin(w_rad) * sun_params.cloud_time * -200.0f;
                sb.shaft_sigma = shaft_sigma;
                sb.shaft_height_falloff = shaft_height_falloff;
                sb.shaft_cloud_occ = shaft_cloud_occ;
                sb.cloud_mid = (cloud_h_min + cloud_h_max) * 0.5f;
                sb.cloud_cover = draw_clouds ? cloud_cobertura : 0.0f;
                sb.shaft_pad0 = 0.0f;

                SDL_GPUColorTargetInfo sh_target = {};
                sh_target.texture = composite_color;
                sh_target.load_op = (view_mode == 3) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
                sh_target.store_op = SDL_GPU_STOREOP_STORE;
                sh_target.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
                SDL_GPURenderPass* sh_pass = SDL_BeginGPURenderPass(cmd_buffer, &sh_target, 1, nullptr);
                SDL_BindGPUGraphicsPipeline(sh_pass, shaft_pipeline);
                SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &sb, sizeof(ShaftBlock));
                SDL_GPUTextureSamplerBinding sh_binds[4] = {
                    { depth_texture, point_sampler },
                    { shadow_map, shadow_cmp },
                    { weather_tex, sampler },
                    { expo_prev, upscale_sampler }   // a MESMA exposicao que pre-expos o composite
                };
                SDL_BindGPUFragmentSamplers(sh_pass, 0, sh_binds, 4);
                SDL_DrawGPUPrimitives(sh_pass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(sh_pass);
            }

            // ============================================================================
            // M2.5-26-4: EXPOSICAO MEDIDA - dois passes minusculos, tudo na GPU.
            //   composite_color            -> luma_map_tex (32x32, log-luminancia)
            //   luma_map_tex + expo[ant.]  -> expo[atual]   (1x1, alvo + adaptacao)
            // Roda DEPOIS do outline (composite pronto) e ANTES do fullscreen: a
            // exposicao aplicada no quadro mede ESTE quadro, sem atraso de frame.
            // A CPU nunca ve o numero - a regra da casa contra readback fica intacta.
            // ============================================================================
            // M2.5-26-2a: os passes rodam SEMPRE. Com "Exposicao automatica" desligada,
            // min = max = o slider manual, o que crava o alvo nele. Assim a textura 1x1
            // carrega SEMPRE a exposicao de fato em uso, e todos os shaders leem a mesma
            // fonte de verdade - nao existe caminho manual paralelo para divergir.
            {
                // M2.5-26-17: entram texw/texh. O PS_Luma passa a ler TEXEL EXATO
                // (Load) em vez de amostra bilinear, e para isso precisa do tamanho.
                // M2.5-26-19: 'speed' vira 'vel_up' e os dois pads viram 'vel_dn'
                // e 'ev_trans'. O tamanho do bloco NAO muda: continua 12 floats /
                // 48 bytes, mesmo layout, sem risco de deslocamento de campo.
                struct ExpoBlock { float key, ec, emin, emax, vel_up, dt, plo, phi;
                                   float texw, texh, vel_dn, ev_trans; } ep;
                ep.key = exposure_key;
                ep.ec = auto_exposure ? exposure_bias_ev : 0.0f;
                // M2.5-26-2c: os limites vem de EV100 (Listing 28): exposure = 1/(1.2*2^EV).
                // EV alto = pouca exposicao, entao ev_max define o MINIMO de exposicao.
                // M2.5-26-6b: a exposicao MANUAL nasce aqui, do EV100, pela mesma
                // conversao dos limites (Listing 28). Fica neste bloco - e nao na UI -
                // porque este bloco roda em TODO frame, com ou sem o painel aberto.
                exposure = 1.0f / (1.2f * std::pow(2.0f, exposure_ev));
                float e_from_ev_hi = 1.0f / (1.2f * std::pow(2.0f, ev_max)); // menos exposicao
                float e_from_ev_lo = 1.0f / (1.2f * std::pow(2.0f, ev_min)); // mais exposicao
                ep.emin = auto_exposure ? e_from_ev_hi : exposure;
                ep.emax = auto_exposure ? e_from_ev_lo : exposure;
                ep.vel_up = expo_vel_up;   // 26-19: stops/s indo para o CLARO
                ep.vel_dn = expo_vel_dn;   // 26-19: stops/s indo para o ESCURO
                ep.ev_trans = expo_ev_trans;
                ep.plo = expo_perc_lo; ep.phi = expo_perc_hi; // 26-12
                ep.texw = (float)internal_w; ep.texh = (float)internal_h; // 26-17
                // 26-19: os pads viraram campos com nome - ver a declaracao acima.
                static uint64_t expo_last_ms = SDL_GetTicks();
                static int expo_warm = 0;
                uint64_t expo_now_ms = SDL_GetTicks();
                ep.dt = std::min((float)(expo_now_ms - expo_last_ms) / 1000.0f, 0.25f);
                expo_last_ms = expo_now_ms;
                // Os dois primeiros frames leem uma textura que ainda nao foi escrita.
                // Um dt enorme faz a adaptacao saltar direto para o alvo e engolir o lixo.
                if (expo_warm < 2 || !auto_exposure) { ep.dt = 1000.0f; if (expo_warm < 2) ++expo_warm; }

                SDL_GPUColorTargetInfo lt = {};
                lt.texture = luma_map_tex;
                lt.load_op = SDL_GPU_LOADOP_CLEAR;
                lt.store_op = SDL_GPU_STOREOP_STORE;
                lt.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
                SDL_GPURenderPass* lpass = SDL_BeginGPURenderPass(cmd_buffer, &lt, 1, nullptr);
                SDL_BindGPUGraphicsPipeline(lpass, expo_luma_pipeline);
                SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &ep, sizeof(ExpoBlock));
                SDL_GPUTextureSamplerBinding lb[2] = {
                    { composite_color, upscale_sampler },
                    { expo_prev, upscale_sampler } // 26-2a: para desfazer a pre-exposicao
                };
                SDL_BindGPUFragmentSamplers(lpass, 0, lb, 2);
                SDL_DrawGPUPrimitives(lpass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(lpass);

                SDL_GPUColorTargetInfo at = {};
                at.texture = expo_tex[expo_ping];
                at.load_op = SDL_GPU_LOADOP_CLEAR;
                at.store_op = SDL_GPU_STOREOP_STORE;
                at.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
                SDL_GPURenderPass* apass = SDL_BeginGPURenderPass(cmd_buffer, &at, 1, nullptr);
                SDL_BindGPUGraphicsPipeline(apass, expo_adapt_pipeline);
                SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &ep, sizeof(ExpoBlock));
                SDL_GPUTextureSamplerBinding ab[2] = {
                    { luma_map_tex, upscale_sampler },
                    { expo_prev, upscale_sampler }
                };
                SDL_BindGPUFragmentSamplers(apass, 0, ab, 2);
                SDL_DrawGPUPrimitives(apass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(apass);
            }

            // ============================================================================
            // M2.5-26-23a: CONSTRUCAO DA GRADE BILATERAL.
            // Roda DEPOIS do passe da exposicao (precisa do ponto medio M que o
            // PS_Adapt acabou de escrever) e e independente do resto - nada le o
            // resultado nesta estacao alem do modo de Vista.
            // NAO altera um pixel da imagem final.
            if (grid_on)
            {
                struct GridBlock { float texel[2]; float dir; float bloom;
                                   float sr, mix, srcw, srch;
                                   float gz_sr, gz_lo, gz_hi, gz_taps; } gb = {};
                gb.texel[0] = 1.0f / 256.0f; gb.texel[1] = 1.0f / 96.0f;
                gb.bloom = (draw_bloom && view_mode == 0) ? bloom_intensity : 0.0f;
                gb.sr = lt_sr; gb.mix = lt_mix;
                gb.srcw = (float)internal_w; gb.srch = (float)internal_h;
                gb.gz_sr = grid_sr; gb.gz_lo = grid_lo;
                gb.gz_hi = grid_hi; gb.gz_taps = (float)grid_taps;

                SDL_GPUColorTargetInfo gt = {};
                gt.texture = grid_tex;
                gt.load_op = SDL_GPU_LOADOP_CLEAR;
                gt.store_op = SDL_GPU_STOREOP_STORE;
                gt.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
                SDL_GPURenderPass* gp = SDL_BeginGPURenderPass(cmd_buffer, &gt, 1, nullptr);
                SDL_BindGPUGraphicsPipeline(gp, lt_grid_pipeline);
                SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &gb, sizeof(GridBlock));
                SDL_GPUTextureSamplerBinding gb_binds[3] = {
                    { composite_color, upscale_sampler },
                    { bloom_tex_a ? bloom_tex_a : fallback_tex, upscale_sampler },
                    { expo_prev, upscale_sampler }
                };
                SDL_BindGPUFragmentSamplers(gp, 0, gb_binds, 3);
                SDL_DrawGPUPrimitives(gp, 3, 1, 0, 0);
                SDL_EndGPURenderPass(gp);
            }

            // ============================================================================
            // M2.5-23: Dual-Kawase Bloom Ping-Pong Pass
            // ============================================================================
            if (draw_bloom && view_mode == 0) {
                BloomParams bp;
                bp.threshold = bloom_threshold;
                bp.knee = 0.5f;
                bp.cap = bloom_cap; // 26-11
                bp.pad_bl[0] = bp.pad_bl[1] = bp.pad_bl[2] = 0.0f;

                auto run_bloom_pass = [&](SDL_GPUTexture* dst, SDL_GPUTexture* src, SDL_GPUGraphicsPipeline* pipe, int w, int h) {
                    SDL_GPUColorTargetInfo t = {};
                    t.texture = dst;
                    // Se for o pipeline UP (add), precisamos de LOAD, senao CLEAR destrutivo padrao
                    t.load_op = (pipe == bloom_up_pipeline) ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
                    t.store_op = SDL_GPU_STOREOP_STORE;
                    t.clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };

                    SDL_GPURenderPass* bpass = SDL_BeginGPURenderPass(cmd_buffer, &t, 1, nullptr);
                    SDL_BindGPUGraphicsPipeline(bpass, pipe);

                    bp.texel[0] = 1.0f / (float)w;
                    bp.texel[1] = 1.0f / (float)h;
                    SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &bp, sizeof(BloomParams));

                    SDL_GPUTextureSamplerBinding sb = { src, upscale_sampler };
                    SDL_BindGPUFragmentSamplers(bpass, 0, &sb, 1);

                    SDL_DrawGPUPrimitives(bpass, 3, 1, 0, 0);
                    SDL_EndGPURenderPass(bpass);
                    };

                // Threshold pass: composite -> A
                run_bloom_pass(bloom_tex_a, composite_color, bloom_thresh_pipeline, internal_w, internal_h);
                // Downsamples
                run_bloom_pass(bloom_tex_b, bloom_tex_a, bloom_down_pipeline, std::max(1, internal_w / 2), std::max(1, internal_h / 2));
                run_bloom_pass(bloom_tex_c, bloom_tex_b, bloom_down_pipeline, std::max(1, internal_w / 4), std::max(1, internal_h / 4));
                run_bloom_pass(bloom_tex_d, bloom_tex_c, bloom_down_pipeline, std::max(1, internal_w / 8), std::max(1, internal_h / 8));
                // Upsamples (Add)
                run_bloom_pass(bloom_tex_c, bloom_tex_d, bloom_up_pipeline, std::max(1, internal_w / 16), std::max(1, internal_h / 16));
                run_bloom_pass(bloom_tex_b, bloom_tex_c, bloom_up_pipeline, std::max(1, internal_w / 8), std::max(1, internal_h / 8));
                run_bloom_pass(bloom_tex_a, bloom_tex_b, bloom_up_pipeline, std::max(1, internal_w / 4), std::max(1, internal_h / 4));
            }

            // ====================================================================
            // M2.5-26-14/15: OPERADOR LOCAL - tres passes de tela cheia, 64x32.
            //   composite + bloom -> lt_tex[0]  (log-luminancia)
            //   lt_tex[0]         -> lt_tex[1]  (gaussiana horizontal, raio 13)
            //   lt_tex[1]         -> lt_tex[0]  (gaussiana vertical)
            // O fullscreen le lt_tex[0] com upsample bicubico B-spline.
            //
            // M2.5-26-15 DEFEITO CORRIGIDO: na 26-14 este bloco estava DEPOIS do
            // passe fullscreen, apesar do comentario dizer o contrario. O fullscreen
            // lia entao a log-luminancia do quadro ANTERIOR - um quadro de atraso
            // que ninguem pediu. Agora roda onde sempre devia: depois do bloom (que
            // ele precisa ler) e antes do fullscreen (que le o resultado).
            // ====================================================================
            if (lt_on)
            {
                // M2.5-26-23a: o bloco cresce de 8 para 12 floats (48 B).
                struct LtBlock { float texel[2]; float dir; float bloom;
                                 float sr, mix, srcw, srch;
                                 float gz_sr, gz_lo, gz_hi, gz_taps; } lt = {};
                lt.srcw = (float)internal_w; lt.srch = (float)internal_h;
                lt.gz_sr = grid_sr; lt.gz_lo = grid_lo;
                lt.gz_hi = grid_hi; lt.gz_taps = (float)grid_taps;
                lt.texel[0] = 1.0f / 64.0f; lt.texel[1] = 1.0f / 32.0f;
                // exatamente a mesma expressao que alimenta fs_params.bloom_intensity
                lt.bloom = (draw_bloom && view_mode == 0) ? bloom_intensity : 0.0f;
                lt.sr = lt_sr; lt.mix = lt_mix; // 26-16

                // M2.5-26-16: QUATRO passes. A segunda fonte de cada etapa muda de
                // papel conforme o passe - no downsample e o bloom, no bilateral e o
                // B gaussiano. Quem nao usa recebe o fallback e le 1 sampler so.
                struct { SDL_GPUTexture* alvo; SDL_GPUTexture* fonte; SDL_GPUTexture* fonte2;
                         SDL_GPUGraphicsPipeline* pipe; float dir; int nsamp; } etapas[4] = {
                    { lt_tex[0], composite_color, bloom_tex_a ? bloom_tex_a : fallback_tex, lt_down_pipeline,  0.0f, 2 },
                    { lt_tex[1], lt_tex[0],       fallback_tex,                             lt_blur_pipeline,  1.0f, 1 },
                    { lt_tex[2], lt_tex[1],       fallback_tex,                             lt_blur_pipeline,  0.0f, 1 },
                    { lt_tex[1], lt_tex[0],       lt_tex[2],                                lt_bilat_pipeline, 0.0f, 2 }
                };
                for (int k = 0; k < 4; ++k)
                {
                    SDL_GPUColorTargetInfo lti = {};
                    lti.texture = etapas[k].alvo;
                    lti.load_op = SDL_GPU_LOADOP_CLEAR;
                    lti.store_op = SDL_GPU_STOREOP_STORE;
                    lti.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
                    SDL_GPURenderPass* ltp = SDL_BeginGPURenderPass(cmd_buffer, &lti, 1, nullptr);
                    SDL_BindGPUGraphicsPipeline(ltp, etapas[k].pipe);
                    lt.dir = etapas[k].dir;
                    SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &lt, sizeof(LtBlock));
                    SDL_GPUTextureSamplerBinding ltb[2] = {
                        { etapas[k].fonte,  upscale_sampler },
                        { etapas[k].fonte2, upscale_sampler }
                    };
                    SDL_BindGPUFragmentSamplers(ltp, 0, ltb, etapas[k].nsamp);
                    SDL_DrawGPUPrimitives(ltp, 3, 1, 0, 0);
                    SDL_EndGPURenderPass(ltp);
                }
            }

            SDL_GPUColorTargetInfo upscale_color_target = {};
            upscale_color_target.texture = swapchain_texture;
            upscale_color_target.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
            upscale_color_target.load_op = SDL_GPU_LOADOP_CLEAR;
            upscale_color_target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* fs_pass = SDL_BeginGPURenderPass(cmd_buffer, &upscale_color_target, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(fs_pass, fs_pipeline);

            // M2.5-20: Projecao do Astro Sol na Matriz de View (Screen Space Pos)
            glm::mat4 view_rot_only = glm::lookAtLH(glm::vec3(0.0f), front, glm::vec3(0, 1, 0));
            glm::mat4 mvp_no_trans = proj * view_rot_only;
            glm::vec4 sun_clip = mvp_no_trans * glm::vec4(to_sun_vec, 1.0f);
            glm::vec3 sun_ndc = glm::vec3(sun_clip) / sun_clip.w;
            glm::vec2 sun_screen_pos = glm::vec2(sun_ndc.x * 0.5f + 0.5f, -sun_ndc.y * 0.5f + 0.5f);

            // Mascara de Fade: se a camera apontar p/ o lado oposto do Sol, os raios esmaecem suavemente
            float dot_sun_cam = glm::dot(front, to_sun_vec);
            float r_fade = std::clamp(dot_sun_cam * 2.0f, 0.0f, 1.0f);

            FsParams fs_params;
            glm::mat4 inv_vp = glm::inverse(mvp);
            memcpy(fs_params.inv_viewproj, glm::value_ptr(inv_vp), 64);
            memcpy(fs_params.light_vp, glm::value_ptr(light_vp), 64);
            fs_params.cam_pos[0] = cam_pos.x; fs_params.cam_pos[1] = cam_pos.y; fs_params.cam_pos[2] = cam_pos.z;
            fs_params.to_sun[0] = to_sun_vec.x; fs_params.to_sun[1] = to_sun_vec.y; fs_params.to_sun[2] = to_sun_vec.z;
            fs_params.shaft_g = shaft_g;
            float shaft_gate = std::clamp((to_sun_vec.y + 0.02f) / 0.14f, 0.0f, 1.0f);
            fs_params.sun_color[0] = sun_color_arr[0] * t_sol.x * effective_intensity * shaft_gate; // 25-1
            fs_params.sun_color[1] = sun_color_arr[1] * t_sol.y * effective_intensity * shaft_gate;
            fs_params.sun_color[2] = sun_color_arr[2] * t_sol.z * effective_intensity * shaft_gate;
            fs_params.shaft_samples = (float)ray_samples;
            fs_params.tex_size[0] = (float)internal_w;
            fs_params.tex_size[1] = (float)internal_h;
            fs_params.wind_off[0] = cos(w_rad) * sun_params.cloud_time * -200.0f;
            fs_params.wind_off[1] = sin(w_rad) * sun_params.cloud_time * -200.0f;
            fs_params.shaft_sigma = shaft_sigma;
            fs_params.cloud_cover = draw_clouds ? cloud_cobertura : 0.0f;
            fs_params.shadow_str_fs = shadow_str;
            fs_params.tonemap_shoulder = tonemap_shoulder;
            fs_params.tonemap_modo = (float)tonemap_modo; // 26-11
            wb_matriz(wb_alvo, fs_params.wb_m); // 26-24: 9 multiplicacoes 3x3 por quadro
            fs_params.wb_pad0 = fs_params.wb_pad1 = fs_params.wb_pad2 = 0.0f;
            // M2.5-26-23c: lt_on vira TRES estados, sem campo novo:
            //   0 = operador local desligado
            //   1 = ligado com o B antigo (PS_Down + blur, caminho da 26-14/16)
            //   2 = ligado com o B da GRADE BILATERAL (26-23a)
            fs_params.lt_on = lt_on ? (lt_b_grade ? 2.0f : 1.0f) : 0.0f; // 26-23c
            fs_params.lt_c  = lt_c;
            fs_params.lt_d  = lt_d;
            fs_params.lt_c_lo = lt_c_lo; // 26-15
            fs_params.bg_sr = grid_sr; fs_params.bg_lo = grid_lo; fs_params.bg_hi = grid_hi; // 26-23c
            fs_params.bg_mix = grid_mix;
            fs_params.bg_pad0 = 0.0f; fs_params.bg_pad1 = 0.0f; fs_params.bg_pad2 = 0.0f;
            // M2.5-26-15: este M vira FALLBACK. O de verdade e lido na GPU da textura
            // 1x1 da exposicao (canal G = a media medida), porque amarrar o ponto medio
            // na CHAVE assume que a medicao esta no alvo - e o ev_min = 4 garante que
            // ela NAO esta no crepusculo. Medido as 5h50: chave 0.10417 contra media
            // real 0.06218, 0.74 stop de desvio.
            fs_params.lt_m  = std::log(std::max(exposure_key, 1e-6f));
            fs_params.shaft_cloud_occ = shaft_cloud_occ;
            fs_params.cloud_mid = (cloud_h_min + cloud_h_max) * 0.5f;
            fs_params.shaft_debug = (view_mode == 3) ? 1.0f : ((view_mode == 4) ? 2.0f : ((view_mode == 5) ? 3.0f : ((view_mode == 6) ? 4.0f : 0.0f))); // 26-23a: + a grade
            fs_params.shaft_height_falloff = shaft_height_falloff; fs_params.dither_amt = dither_amt;
            fs_params.expo_auto = auto_exposure ? 1.0f : 0.0f; // 26-4: liga a leitura da textura 1x1
            // Anular shafts nas views de debug para nao sujar a validacao
            fs_params.shaft_intensity = (view_mode == 0 || view_mode == 3) ? ray_strength : 0.0f; // 26-I: medidores tambem sem shafts (a grade, 6, tambem)
            fs_params.bloom_intensity = (draw_bloom && view_mode == 0) ? bloom_intensity : 0.0f;

            SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &fs_params, sizeof(FsParams));

            SDL_GPUTexture* upscale_source = composite_color;
            if (view_mode == 1) upscale_source = normal_id;
            else if (view_mode == 2) upscale_source = scene_color;

            // O Pass Fullscreen agora bebe da id_mask pra calcular a oclusão volumetrica da luz e da cor unificada do bloom kawase
            // M2.5-26-15 DEFEITO CORRIGIDO: a 26-14 declarou o vetor com 7 posicoes
            // e escreveu na posicao 7 (a oitava). Isso e escrita FORA do vetor -
            // comportamento indefinido, que so nao quebrou porque a pilha tinha folga.
            // O oitavo binding agora nasce dentro do inicializador, onde deveria.
            SDL_GPUTextureSamplerBinding fs_samp_binds[9] = {
                { upscale_source, upscale_sampler },
                { normal_id, upscale_sampler },
                { bloom_tex_a ? bloom_tex_a : fallback_tex, upscale_sampler },
                { depth_texture, point_sampler },
                { shadow_map, shadow_cmp },
                { weather_tex, sampler },
                { expo_prev, upscale_sampler }, // M2.5-26-2a: a MESMA exposicao que pre-expos a cena
                { lt_tex[1], upscale_sampler }, // 26-16: t7 = B FINAL (bilateral+gaussiana)
                { grid_tex, upscale_sampler }   // 26-23a: t8 = o atlas da grade
            };
            SDL_BindGPUFragmentSamplers(fs_pass, 0, fs_samp_binds, 9);

            SDL_DrawGPUPrimitives(fs_pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(fs_pass);

            expo_ping = 1 - expo_ping; // M2.5-26-4: alterna o ping-pong da exposicao

            SDL_GPUColorTargetInfo ui_target = {};
            ui_target.texture = swapchain_texture;
            ui_target.load_op = SDL_GPU_LOADOP_LOAD;
            ui_target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* ui_pass = SDL_BeginGPURenderPass(cmd_buffer, &ui_target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd_buffer, ui_pass);
            SDL_EndGPURenderPass(ui_pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd_buffer);

        // ====================================================================
        // M2.5-26-M: MEDIR AGORA. Readback de UM quadro, disparado por clique.
        // A regra da casa proibe readback no caminho de render (latencia); isto
        // e diagnostico manual, roda so quando pedido, e a mesma maquina que a
        // contraprova Sunny 16 vai precisar.
        // Baixa o mapa 32x32 de LOG-luminancia (que o PS_Luma ja gravou com a
        // pre-exposicao DESFEITA, ou seja em cd/m2 absolutos) e devolve:
        //   Lavg (media geometrica) - EV100 - a exposicao resultante -
        //   min/max e um histograma por decada. O histograma e o que separa
        //   "o medidor esta errado" de "a cena esta errada".
        // ====================================================================
        if (g_medir_pedido)
        {
            g_medir_pedido = false;

            // ================================================================
            // M2.5-26-21: O RETRATO DA CONFIGURACAO, JUNTO COM A MEDIDA.
            //
            // PEDIDO DO ARTHUR, e ele esta certo: "uma coisa que eu acho que nos
            // ajudaria e se o log tambem mostrasse as configuracoes atuais para
            // evitar erros".
            //
            // POR QUE, com o historico a vista. Nesta temporada a gente perdeu
            // rodadas inteiras por nao saber o estado dos sliders no momento da
            // medida:
            //   * uma medida das 5h50 saiu com as nuvens LIGADAS e outra com elas
            //     desligadas - e o pico mudou 8x (2.5e4 -> 3.2e3) sem que nenhum
            //     dos dois logs dissesse qual era qual;
            //   * duas medidas foram tiradas com a curva Khronos DESLIGADA e isso
            //     so apareceu depois, por lembranca;
            //   * o tuning.ini do x64-Release foi apagado enquanto o binario lia o
            //     do x64-Debug, e os defaults novos da 26-19 nunca chegaram a rodar
            //     - o painel dizia ev_min 4 e ninguem percebeu por dois dias.
            // Uma medida sem o estado que a produziu nao e um dado, e uma anedota.
            //
            // Isto e SO log. Nenhum valor muda, nenhum pixel muda.
            // ================================================================
            {
                app_log.add("[CONFIG] hora %.2fh (manual %d) | interna %dx%d modo %d | nuvens %d cob %.3f passos %d",
                    sol_hora, sol_hora_manual ? 1 : 0, internal_w, internal_h, pixel_mode,
                    draw_clouds ? 1 : 0, cloud_cobertura, cloud_passos);
                app_log.add("[CONFIG] expo auto %d | manual %.2f EV | chave %.4f | comp %.2f EV | percentis %.2f/%.2f | ev_min %.2f ev_max %.2f",
                    auto_exposure ? 1 : 0, exposure_ev, exposure_key, exposure_bias_ev,
                    expo_perc_lo, expo_perc_hi, ev_min, ev_max);
                app_log.add("[CONFIG] adaptacao claro %.2f escuro %.2f assent %.2f stops/s",
                    expo_vel_up, expo_vel_dn, expo_ev_trans);
                app_log.add("[CONFIG] curva %s (ombro %.3f) | bloom %d forca %.3f limiar %.3f teto %.2f",
                    (tonemap_modo > 0) ? "KHRONOS" : "LEGADO-24I", tonemap_shoulder,
                    draw_bloom ? 1 : 0, bloom_intensity, bloom_threshold, bloom_cap);
                app_log.add("[CONFIG] balanco de branco: o branco vira (%.3f %.3f %.3f)%s", // 26-24
                    wb_alvo[0], wb_alvo[1], wb_alvo[2],
                    (wb_alvo[0] == 1.0f && wb_alvo[1] == 1.0f && wb_alvo[2] == 1.0f) ? "  = NEUTRO" : "");
                app_log.add("[CONFIG] tonemap local %d | c %.3f d %.3f c_lo %.3f mix %.3f sigma %.3f",
                    lt_on ? 1 : 0, lt_c, lt_d, lt_c_lo, lt_mix, lt_sr);
                app_log.add("[CONFIG] sol %.0f lx | lua %.3f lx | piso noturno %.5f | ceu-luz %.2f | aerea %.2f",
                    sun_intensity, moon_scene_bright, night_sky_floor, sky_light_str, aerial_cena_str);
                // M2.5-26-23b: a grade faltava no retrato. Falha da 26-23a - o
                // primeiro [CONFIG] tirado depois dela nao dizia se ela estava
                // sequer ligada, que e a primeira coisa que eu precisava saber.
                app_log.add("[CONFIG] grade %d | sigma_r %.3f ln | Z -%.2f/+%.2f sigma_r | %d taps/eixo (%d por celula)",
                    grid_on ? 1 : 0, grid_sr, grid_lo, grid_hi, grid_taps, grid_taps * grid_taps);

                SDL_Log("[CONFIG] hora %.2fh manual %d | %dx%d | nuvens %d | auto %d EVman %.2f perc %.2f/%.2f evmin %.2f evmax %.2f",
                    sol_hora, sol_hora_manual ? 1 : 0, internal_w, internal_h,
                    draw_clouds ? 1 : 0, auto_exposure ? 1 : 0, exposure_ev,
                    expo_perc_lo, expo_perc_hi, ev_min, ev_max);
                SDL_Log("[CONFIG] curva %s | bloom %d forca %.3f limiar %.3f teto %.2f | local %d c %.3f d %.3f | sol %.0f lx lua %.3f lx",
                    (tonemap_modo > 0) ? "KHRONOS" : "LEGADO-24I",
                    draw_bloom ? 1 : 0, bloom_intensity, bloom_threshold, bloom_cap,
                    lt_on ? 1 : 0, lt_c, lt_d, sun_intensity, moon_scene_bright);
                // M2.5-26-24: a linha do console e a que sai colada num relatorio.
                // Ela nao levava o EC, o estado do B da grade, a mistura nem o
                // balanco - e cada um deles ja custou uma rodada de perguntas.
                SDL_Log("[CONFIG] estrelas: forca %.2f | mag ate %.2f | sigma %.2f px | %.0f/face = %.0f estrelas | inclinacao %.2f", // 25-18
                    stars_str, stars_mag_lim, stars_size, stars_dens, 6.0f * stars_dens * stars_dens, stars_slope);
                SDL_Log("[CONFIG] EC %.2f | B da grade %d mistura %.2f | branco vira (%.3f %.3f %.3f)",
                    exposure_bias_ev, lt_b_grade ? 1 : 0, grid_mix,
                    wb_alvo[0], wb_alvo[1], wb_alvo[2]);
                // ====================================================================
                // M2.5-26-26: A CONFORMIDADE VIRA INSTRUMENTO PERMANENTE.
                //
                // A auditoria de 30/08 achou quatro numeros divergentes da fonte, e
                // DOIS deles estavam la porque o controle nao alcancava o valor
                // certo - o slider de Sigma tinha minimo 9,2x acima do fisico. Eles
                // sobreviveram meses porque nada os cobrava.
                //
                // Esta linha cobra. So entram numeros que a documentacao FIXA; o que
                // a fonte deixa para a arte nao aparece aqui e nunca vai aparecer.
                // Tolerancia de 2% relativa - acima disso e divergencia, nao ruido.
                // ====================================================================
                {
                    const float s_ar = sigma_ar_limpo();
                    struct { const char* nome; float tem; float fonte; } cf[] = {
                        { "fase_g",   shaft_g,     0.80f  },  // Cornette-Shanks, Hillaire T1
                        { "sigma_ar", shaft_sigma, s_ar   },  // Tabela 1, ar limpo
                        { "fog",      fog_density, s_ar   },  // idem
                        { "lt_sigma", lt_sr,       0.921f },  // Durand & Dorsey 2002
                    };
                    char lin[256]; int pos = 0, fora = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        float razao = cf[i].tem / ((cf[i].fonte != 0.0f) ? cf[i].fonte : 1.0f);
                        if (std::fabs(razao - 1.0f) <= 0.02f) { continue; }
                        ++fora;
                        int n = SDL_snprintf(lin + pos, sizeof(lin) - pos, "%s %.6g (fonte %.6g, %+.2f st) ",
                                             cf[i].nome, cf[i].tem, cf[i].fonte,
                                             std::log2(std::max(razao, 1e-9f)));
                        if (n <= 0 || pos + n >= (int)sizeof(lin) - 1) { break; }
                        pos += n;
                    }
                    if (fora == 0)
                        SDL_Log("[CONFIG] conformidade: os 4 valores fixados pela fonte conferem");
                    else
                        SDL_Log("[CONFIG] conformidade: %d de 4 FORA da fonte -> %s", fora, lin);
                }
                SDL_Log("[CONFIG] grade %d | sigma_r %.3f | Z -%.2f/+%.2f | %d taps",  // 26-23b
                    grid_on ? 1 : 0, grid_sr, grid_lo, grid_hi, grid_taps);
            }
            const int LW = 32, LH = 32;
            const uint32_t bytes = (uint32_t)(LW * LH * 8); // RGBA16F
            SDL_GPUTransferBufferCreateInfo dl_info = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes };
            SDL_GPUTransferBuffer* dl_tb = SDL_CreateGPUTransferBuffer(device, &dl_info);
            if (dl_tb)
            {
                SDL_GPUCommandBuffer* dl_cmd = SDL_AcquireGPUCommandBuffer(device);
                SDL_GPUCopyPass* dl_pass = SDL_BeginGPUCopyPass(dl_cmd);
                SDL_GPUTextureRegion dl_reg = { luma_map_tex, 0, 0, 0, 0, 0, (Uint32)LW, (Uint32)LH, 1 };
                SDL_GPUTextureTransferInfo dl_dst = { dl_tb, 0, (Uint32)LW, (Uint32)LH };
                SDL_DownloadFromGPUTexture(dl_pass, &dl_reg, &dl_dst);
                SDL_EndGPUCopyPass(dl_pass);
                SDL_GPUFence* dl_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(dl_cmd);
                if (dl_fence) { SDL_WaitForGPUFences(device, true, &dl_fence, 1); SDL_ReleaseGPUFence(device, dl_fence); }

                uint16_t* px = (uint16_t*)SDL_MapGPUTransferBuffer(device, dl_tb, false);
                if (px)
                {
                    double soma = 0.0; int n = 0;
                    // M2.5-26-19: O MEDIDOR PASSA A MEDIR O QUE O SHADER MEDE.
                    // Ate aqui este bloco calculava a media log de TODOS os 1024
                    // blocos e chamava aquilo de "Lavg" - mas o PS_Adapt aplica a
                    // JANELA DE PERCENTIL desde a 26-12. Com 5/95 a diferenca era
                    // pequena (a janela cobria 90% do quadro) e passou batida. Com
                    // 75/85 ela vale ~2 stops: o painel reportaria um alvo que o
                    // shader nunca usou. E a mesma classe de defeito que custou
                    // quatro estacoes - um instrumento que mente com confianca.
                    // Abaixo, as DUAS leituras, lado a lado e rotuladas.
                    static float lgs[1024]; int nlg = 0;
                    float lo = 1e30f, hi = -1e30f;
                    float pico_real = -1e30f, vale_real = 1e30f; // 26-17: pixel, nao bloco
                    int hist[10] = { 0,0,0,0,0,0,0,0,0,0 }; // decadas de log10(cd/m2): <-1 .. >=7
                    for (int i = 0; i < LW * LH; ++i)
                    {
                        float lg = h2f(px[i * 4 + 0]); // R = MEDIA do log no bloco
                        float pk = h2f(px[i * 4 + 1]); // G = PICO do bloco  (26-17)
                        float vl = h2f(px[i * 4 + 2]); // B = VALE do bloco  (26-17)
                        if (!(lg > -1e30f && lg < 1e30f)) { hist[9] += 1000; continue; } // inf/NaN
                        soma += lg; ++n;
                        if (nlg < 1024) lgs[nlg++] = lg; // 26-19
                        if (lg < lo) lo = lg;
                        if (lg > hi) hi = lg;
                        if (pk > -1e30f && pk < 1e30f && pk > pico_real) pico_real = pk;
                        if (vl > -1e30f && vl < 1e30f && vl < vale_real) vale_real = vl;
                        float d = std::log10(std::exp(lg));
                        int b = (int)std::floor(d) + 1; b = std::max(0, std::min(9, b));
                        hist[b] += 1;
                    }
                    SDL_UnmapGPUTransferBuffer(device, dl_tb);
                    if (n > 0)
                    {
                        float l_todos = (float)std::exp(soma / (double)n);
                        // M2.5-26-19: replica EXATA do PS_Adapt - mesmas 64 caixas,
                        // mesmos LN_MIN/LN_MAX, mesma busca de limiar por borda de
                        // caixa, mesma segunda passagem exata. Se este bloco e o
                        // shader divergirem, o painel volta a mentir.
                        float l_avg = l_todos;
                        {
                            const int NB = 64;
                            const float LN_MIN = -9.21034f, LN_MAX = 13.81551f;
                            const float LN_ESC = (float)NB / (LN_MAX - LN_MIN);
                            float hb[NB]; for (int b = 0; b < NB; ++b) hb[b] = 0.0f;
                            for (int i = 0; i < nlg; ++i) {
                                int b = (int)std::clamp((lgs[i] - LN_MIN) * LN_ESC, 0.0f, (float)(NB - 1));
                                hb[b] += 1.0f;
                            }
                            float tot = (float)std::max(nlg, 1);
                            float n_lo = std::clamp(expo_perc_lo, 0.0f, 0.98f) * tot;
                            float n_hi = std::clamp(expo_perc_hi, 0.02f, 1.0f) * tot;
                            if (n_hi <= n_lo) { n_lo = 0.0f; n_hi = tot; }
                            float ln_lo = LN_MIN, ln_hi = LN_MAX, acc = 0.0f;
                            for (int b = 0; b < NB; ++b) {
                                float ini = acc; acc += hb[b];
                                if (ini <= n_lo && acc > n_lo) ln_lo = LN_MIN + (float)b / LN_ESC;
                                if (ini <  n_hi && acc >= n_hi) ln_hi = LN_MIN + (float)(b + 1) / LN_ESC;
                            }
                            double sj = 0.0; int nj = 0;
                            for (int i = 0; i < nlg; ++i)
                                if (lgs[i] >= ln_lo && lgs[i] <= ln_hi) { sj += lgs[i]; ++nj; }
                            if (nj > 0) l_avg = (float)std::exp(sj / (double)nj);
                            app_log.add("[MEDIDA] janela %.0f-%.0f pct: %d de %d blocos | Lavg JANELA=%.4g  (Lavg de TUDO=%.4g, %.2f stops de diferenca)",
                                expo_perc_lo * 100.0f, expo_perc_hi * 100.0f, nj, nlg,
                                l_avg, l_todos, std::log2(std::max(l_avg, 1e-9f) / std::max(l_todos, 1e-9f)));
                        }
                        float ev100 = std::log2(l_avg * 100.0f / 12.5f);
                        float alvo = exposure_key / std::max(l_avg, 1e-5f) * std::pow(2.0f, exposure_bias_ev);
                        float e_hi = 1.0f / (1.2f * std::pow(2.0f, ev_max));
                        float e_lo = 1.0f / (1.2f * std::pow(2.0f, ev_min));
                        float clamped = std::clamp(alvo, std::min(e_hi, e_lo), std::max(e_hi, e_lo));
                        app_log.add("[MEDIDA] Lavg=%.4g cd/m2 | EV100=%.2f | alvo=%.4g | aplicada=%.4g%s",
                            l_avg, ev100, alvo, clamped, (clamped != alvo) ? " (TETO/PISO MORDEU)" : "");
                        // M2.5-26-17: as DUAS leituras, lado a lado. A de cima e a media
                        // de bloco (o que existia); a de baixo e o pixel. A razao entre os
                        // picos e quanto o medidor antigo estava escondendo.
                        {
                            float pk = std::exp(pico_real), vl = std::exp(vale_real);
                            app_log.add("[MEDIDA] PIXEL  min=%.4g  max=%.4g cd/m2  (faixa %.1f stops)",
                                        vl, pk, std::log2(std::max(pk, 1e-9f) / std::max(vl, 1e-9f)));
                            app_log.add("[MEDIDA] o bloco 32x32 escondia %.1fx do pico (%.2f stops)",
                                        pk / std::max(std::exp(hi), 1e-9f),
                                        std::log2(pk / std::max(std::exp(hi), 1e-9f)));
                            SDL_Log("[MEDIDA] PIXEL min=%.4g max=%.4g | bloco escondia %.1fx",
                                    vl, pk, pk / std::max(std::exp(hi), 1e-9f));
                            g_med_expo = clamped; g_med_ev = std::log2(1.0 / (1.2 * clamped));
                            g_med_tick = sim_tick;
                            g_med_lavg = l_avg; // 26-20: para o confronto GPU x CPU
                        }
                        app_log.add("[MEDIDA] BLOCO  min=%.4g  max=%.4g cd/m2  (razao %.0fx)",
                            std::exp(lo), std::exp(hi), std::exp(hi - lo));
                        app_log.add("[MEDIDA] hist log10(cd/m2) <0:%d 0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7:%d inf:%d",
                            hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9]);
                        SDL_Log("[MEDIDA] Lavg=%.4g EV100=%.2f alvo=%.4g aplicada=%.4g min=%.4g max=%.4g",
                            l_avg, ev100, alvo, clamped, std::exp(lo), std::exp(hi));
                        SDL_Log("[MEDIDA] hist <0:%d 0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7:%d inf:%d",
                            hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9]);
                    }
                    else { SDL_Log("[MEDIDA] mapa inteiro invalido (inf/NaN) - o medidor esta envenenado."); }
                }
                SDL_ReleaseGPUTransferBuffer(device, dl_tb);
            }
            else { SDL_Log("[MEDIDA] falhou ao criar o transfer buffer: %s", SDL_GetError()); }

            // ================================================================
            // M2.5-26-20: O MEDIDOR PASSA A VER A EXPOSICAO QUE A GPU USOU.
            //
            // O QUE FALTAVA, e custou uma discussao inteira. O bloco [MEDIDA]
            // imprime 'alvo' e 'aplicada' - MAS OS DOIS SAO CALCULADOS NA CPU:
            //     alvo    = exposure_key / l_avg * 2^EC
            //     aplicada = clamp(alvo, ...)
            // Nenhum dos dois e o valor ADAPTADO que o PS_Adapt escreveu. Durante
            // um transiente o painel mostra para onde a exposicao esta indo e
            // nunca onde ela esta. E a mesma classe do exposure_auto_cur que a
            // 26-17 matou, ressuscitada em outro lugar: um numero no painel que
            // nao e o que renderiza.
            //
            // O QUE ISTO RESPONDE, com um numero so - O SUBNORMAL EM HALF.
            // Frostbite 5.2, Tabela 14, VERBATIM: o menor valor do 16-bit float e
            // 6.10 x 10^-5. A nossa textura 1x1 da exposicao e R16G16B16A16_FLOAT.
            // Medido nesta engine ao meio-dia: exposicao 6.881e-06 olhando para o
            // chao e 2.829e-05 olhando para o ceu - AS DUAS abaixo do piso. A
            // fronteira cai em EV100 13,74, e toda cena de dia vive acima dela.
            // Se a GPU faz flush-to-zero de subnormais, 'prev' volta ZERO, cai no
            //     if (!(prev > 0.0f)) prev = alvo;
            // e a adaptacao salta direto para o alvo, todo frame, o dia inteiro.
            // Imprimindo o PADRAO DE BITS a duvida acaba:
            //     0x0000            -> a GPU descartou. Hipotese confirmada.
            //     expoente zero, mantissa != 0 -> guardou o subnormal. Descartada.
            //
            // E DE QUEBRA confronta o l_avg da GPU (canal G) com a replica de CPU
            // da 26-19. Se os dois divergirem, a replica esta mentindo e eu quero
            // saber disso antes de tirar qualquer conclusao de um [MEDIDA].
            //
            // CUSTO: 8 bytes, num botao que ja faz download de 8 KB. A regra da
            // casa contra readback continua valendo no caminho de render - isto
            // roda so no clique, igual ao resto do [MEDIDA].
            // ================================================================
            {
                SDL_GPUTransferBufferCreateInfo ex_info = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, 8u };
                SDL_GPUTransferBuffer* ex_tb = SDL_CreateGPUTransferBuffer(device, &ex_info);
                if (ex_tb)
                {
                    // Depois do flip da linha do expo_ping, a textura escrita NESTE
                    // frame e a expo_tex[1 - expo_ping]. E a mesma que o proximo
                    // frame vai pre-expor a cena, ou seja: a exposicao de verdade.
                    SDL_GPUCommandBuffer* ex_cmd = SDL_AcquireGPUCommandBuffer(device);
                    SDL_GPUCopyPass* ex_pass = SDL_BeginGPUCopyPass(ex_cmd);
                    SDL_GPUTextureRegion ex_reg = { expo_tex[1 - expo_ping], 0, 0, 0, 0, 0, 1u, 1u, 1u };
                    SDL_GPUTextureTransferInfo ex_dst = { ex_tb, 0, 1u, 1u };
                    SDL_DownloadFromGPUTexture(ex_pass, &ex_reg, &ex_dst);
                    SDL_EndGPUCopyPass(ex_pass);
                    SDL_GPUFence* ex_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(ex_cmd);
                    if (ex_fence) { SDL_WaitForGPUFences(device, true, &ex_fence, 1); SDL_ReleaseGPUFence(device, ex_fence); }

                    uint16_t* ex_px = (uint16_t*)SDL_MapGPUTransferBuffer(device, ex_tb, false);
                    if (ex_px)
                    {
                        uint16_t bits_r = ex_px[0];
                        uint16_t bits_g = ex_px[1];
                        SDL_UnmapGPUTransferBuffer(device, ex_tb);
                        float e_real = h2f(bits_r);
                        float lavg_gpu = h2f(bits_g);
                        const char* classe =
                            (bits_r == 0u)                  ? "ZERO - a GPU DESCARTOU o subnormal" :
                            ((bits_r & 0x7C00u) == 0u)      ? "SUBNORMAL - a GPU guardou, sem flush" :
                                                              "normal (acima de 6.10e-5)";
                        float ev_real = (e_real > 0.0f) ? std::log2(1.0f / (1.2f * e_real)) : 0.0f;
                        app_log.add("[26-20] GPU exposicao REAL = %.4g  (EV100 %.2f)  bits 0x%04X  %s",
                            e_real, ev_real, (unsigned)bits_r, classe);
                        SDL_Log("[26-20] GPU exposicao REAL = %.4g (EV100 %.2f) bits 0x%04X %s",
                            e_real, ev_real, (unsigned)bits_r, classe);
                        app_log.add("[26-20] GPU l_avg (canal G) = %.4g  bits 0x%04X",
                            lavg_gpu, (unsigned)bits_g);
                        if (g_med_lavg > 0.0 && lavg_gpu > 0.0f)
                            app_log.add("[26-20] l_avg  GPU %.4g  x  replica CPU %.4g  =  %+.3f stops de divergencia",
                                lavg_gpu, (float)g_med_lavg,
                                std::log2(lavg_gpu / (float)std::max(g_med_lavg, 1e-9)));
                        if (g_med_expo > 0.0 && e_real > 0.0f)
                            app_log.add("[26-20] adaptacao: REAL %.4g  x  alvo %.4g  =  %+.2f stops de atraso",
                                e_real, (float)g_med_expo,
                                std::log2((float)g_med_expo / e_real));
                        SDL_Log("[26-20] l_avg GPU %.4g x CPU %.4g | adaptacao atrasada %+.2f stops",
                            lavg_gpu, (float)g_med_lavg,
                            (g_med_expo > 0.0 && e_real > 0.0f) ? std::log2((float)g_med_expo / e_real) : 0.0f);

                        // ============================================================
                        // M2.5-26-23b: A GRADE, LIDA EM NUMERO EM VEZ DE OLHADA.
                        //
                        // A 26-23a entregou um modo de Vista para inspecionar o atlas
                        // com o olho. Sete pedidos de captura de tela depois, a licao
                        // desta temporada inteira se aplica a ela mesma: quando o dado
                        // nao chega, o certo e CONSTRUIR O INSTRUMENTO, nao pedir de
                        // novo. Isto e mais preciso que a imagem, e cabe num log.
                        //
                        // Baixa o atlas 256x96 (192 KB, num botao que ja baixa 8 KB) e
                        // devolve, por fatia Z:
                        //   * quantas celulas de 2048 tem peso (ocupacao)
                        //   * o B medio da fatia, em stops relativos a M
                        // A PERGUNTA QUE ISSO RESPONDE: a grade separa por luminancia?
                        //   ocupacao concentrada em UMA fatia -> nao separa nada, e
                        //     ligar o B nela na 26-23c seria inutil
                        //   ocupacao espalhada, com poucas celulas nas fatias CLARAS ->
                        //     a faixa do horizonte esta isolada numa caixa propria, que
                        //     e exatamente o que o B do PS_Down nao consegue fazer
                        // E o numero de celulas VAZIAS diz se o eixo Z esta largo demais.
                        // ============================================================
                        if (grid_on && lavg_gpu > 0.0f && e_real > 0.0f)
                        {
                            const int GW = 256, GH = 96, GNZ = 12;
                            SDL_GPUTransferBufferCreateInfo gr_info = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                                        (uint32_t)(GW * GH * 8) };
                            SDL_GPUTransferBuffer* gr_tb = SDL_CreateGPUTransferBuffer(device, &gr_info);
                            if (gr_tb)
                            {
                                SDL_GPUCommandBuffer* gr_cmd = SDL_AcquireGPUCommandBuffer(device);
                                SDL_GPUCopyPass* gr_pass = SDL_BeginGPUCopyPass(gr_cmd);
                                SDL_GPUTextureRegion gr_reg = { grid_tex, 0, 0, 0, 0, 0, (Uint32)GW, (Uint32)GH, 1 };
                                SDL_GPUTextureTransferInfo gr_dst = { gr_tb, 0, (Uint32)GW, (Uint32)GH };
                                SDL_DownloadFromGPUTexture(gr_pass, &gr_reg, &gr_dst);
                                SDL_EndGPUCopyPass(gr_pass);
                                SDL_GPUFence* gr_f = SDL_SubmitGPUCommandBufferAndAcquireFence(gr_cmd);
                                if (gr_f) { SDL_WaitForGPUFences(device, true, &gr_f, 1); SDL_ReleaseGPUFence(device, gr_f); }
                                uint16_t* gp = (uint16_t*)SDL_MapGPUTransferBuffer(device, gr_tb, false);
                                if (gp)
                                {
                                    int    ocup[12] = { 0,0,0,0,0,0,0,0,0,0,0,0 };
                                    double bsum[12] = { 0,0,0,0,0,0,0,0,0,0,0,0 };
                                    for (int py = 0; py < GH; ++py)
                                        for (int px = 0; px < GW; ++px)
                                        {
                                            int idx = (py * GW + px) * 4;
                                            float wv = h2f(gp[idx + 0]);
                                            float w  = h2f(gp[idx + 1]);
                                            int z = (py / 32) * 4 + (px / 64);
                                            if (z < 0 || z >= GNZ) continue;
                                            if (w > 1e-4f && wv > -1e30f && wv < 1e30f)
                                            { ocup[z] += 1; bsum[z] += (double)(wv / w); }
                                        }
                                    // M2.5-26-23d: o unmap desceu para o FIM deste bloco.
                                    // O medidor do operador, la embaixo, le os mesmos
                                    // 256x96 texels - baixar o atlas duas vezes para ler
                                    // duas coisas dele seria desperdicio e, pior, as duas
                                    // leituras poderiam cair em quadros diferentes.
                                    // M e o mesmo que o shader usa: log(l_avg * exposicao)
                                    float Mg = std::log(std::max(lavg_gpu * e_real, 1e-9f));
                                    float dz = (grid_lo + grid_hi) * grid_sr / (float)GNZ;
                                    int tot = 0; for (int z = 0; z < GNZ; ++z) tot += ocup[z];
                                    app_log.add("[GRADE] Z de %+.2f a %+.2f ln em torno de M=%.2f | dz %.3f ln = %.2f stops | %d de 24576 celulas com peso (%.1f%%)",
                                        Mg - grid_lo * grid_sr, Mg + grid_hi * grid_sr, Mg, dz, dz / 0.6931472f,
                                        tot, 100.0f * (float)tot / 24576.0f);
                                    app_log.add("[GRADE] ocupacao 0(escura)->11(clara): %d %d %d %d %d %d %d %d %d %d %d %d",
                                        ocup[0],ocup[1],ocup[2],ocup[3],ocup[4],ocup[5],
                                        ocup[6],ocup[7],ocup[8],ocup[9],ocup[10],ocup[11]);
                                    SDL_Log("[GRADE] M=%.2f dz=%.3f ln | ocup: %d %d %d %d %d %d %d %d %d %d %d %d",
                                        Mg, dz, ocup[0],ocup[1],ocup[2],ocup[3],ocup[4],ocup[5],
                                        ocup[6],ocup[7],ocup[8],ocup[9],ocup[10],ocup[11]);
                                    // B medio de cada fatia ocupada, em stops relativos a M
                                    char lin[256]; int pos = 0;
                                    for (int z = 0; z < GNZ; ++z)
                                    {
                                        float bs = (ocup[z] > 0) ? (float)((bsum[z] / ocup[z] - Mg) / 0.6931472) : 0.0f;
                                        int n = SDL_snprintf(lin + pos, sizeof(lin) - pos,
                                                             (ocup[z] > 0) ? "%+.1f " : "  .  ", bs);
                                        if (n <= 0 || pos + n >= (int)sizeof(lin) - 1) break;
                                        pos += n;
                                    }
                                    app_log.add("[GRADE] B medio por fatia, em stops relativos a M: %s", lin);
                                    SDL_Log("[GRADE] B/fatia (stops vs M): %s", lin);

                                    // ================================================
                                    // M2.5-26-23d: O OPERADOR LOCAL, MEDIDO.
                                    //
                                    // O PROBLEMA, e ele e ESTRUTURAL, nao um bug: o
                                    // [MEDIDA] le o composite_color, que fica ANTES do
                                    // operador local. As medidas com 'B da grade'
                                    // ligado e desligado saem IDENTICAS AO DIGITO -
                                    // PIXEL max, BLOCO max, histograma, tudo. Isso e
                                    // CORRETO por construcao, e significa que nenhum
                                    // instrumento deste motor conseguia dizer se a
                                    // 26-23c mudou um pixel. So o olho podia.
                                    // A licao desta temporada, aplicada mais uma vez:
                                    // quando o dado nao chega, constroi-se o
                                    // instrumento; nao se pede a captura de novo.
                                    //
                                    // O QUE ELE FAZ. Baixa tambem o lt_tex[1] (64x32,
                                    // 16 KB - o B ANTIGO, o mesmo texel que o
                                    // amostra_bspline le no canal R) e, para as 2048
                                    // celulas, refaz na CPU o caminho inteiro que falta
                                    // ate a tela, com CADA UM dos dois B:
                                    //   Io = c_sel*(B-M) + d*(Ii-B) + M   (slide 137)
                                    //   -> Khronos PBR Neutral -> sRGB -> 0..255
                                    //
                                    // DE ONDE VEM CADA INGREDIENTE - todos MEDIDOS,
                                    // nenhum estimado:
                                    //   M   = ln(l_avg * exposicao), os dois canais da
                                    //         textura 1x1 que a 26-20 ja baixou. E a
                                    //         MESMA linha do shader, nao uma replica.
                                    //   Ii  = soma_z(wv) / soma_z(w) da celula no atlas.
                                    //         O PS_Grid escreve V = log da luminancia de
                                    //         (composite + bloom*intensidade), que e
                                    //         exatamente o que o fullscreen chama de Ii,
                                    //         e os pesos de Z de cada amostra somam ~1
                                    //         (slide 136: "each sample contributes to
                                    //         two nearest Z slices"). Logo a razao e a
                                    //         media de Ii sobre os taps da celula.
                                    //   B_ant = canal R do lt_tex[1].
                                    //   B_gr  = o mesmo fatiamento em Z do fullscreen.
                                    //   c_sel = (B >= M) ? lt_c : lt_c_lo  (slide 145).
                                    //         Com lt_c_lo = 1.00 o ramo escuro tem ganho
                                    //         exp(0) = 1: o operador so age no lado
                                    //         CLARO. Por isso a conta abaixo mede o que
                                    //         importa - a faixa do horizonte.
                                    //
                                    // TRES APROXIMACOES, DECLARADAS:
                                    //  1. o shader le B com bicubic B-spline sobre 64x32
                                    //     (slide 140); aqui e o texel da celula. A
                                    //     diferenca e de subtexel.
                                    //  2. a celula tem ~10x11 pixels e o Ii dela e a
                                    //     media dos taps, nao o pixel de pico. O numero
                                    //     e um PISO do estouro real, nunca um teto.
                                    //  3. a curva Khronos e avaliada como se a celula
                                    //     fosse CINZA NEUTRO. Para neutro a conta e
                                    //     EXATA (o lerp de dessaturacao vira no-op);
                                    //     para uma faixa alaranjada ela superestima o
                                    //     byte. Declarado, nao escondido.
                                    // ================================================
                                    if (tonemap_modo > 0)
                                    {
                                        const int BW = 64, BH = 32;
                                        SDL_GPUTransferBufferCreateInfo bo_info = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                                                    (uint32_t)(BW * BH * 8) };
                                        SDL_GPUTransferBuffer* bo_tb = SDL_CreateGPUTransferBuffer(device, &bo_info);
                                        if (bo_tb)
                                        {
                                            SDL_GPUCommandBuffer* bo_cmd = SDL_AcquireGPUCommandBuffer(device);
                                            SDL_GPUCopyPass* bo_pass = SDL_BeginGPUCopyPass(bo_cmd);
                                            SDL_GPUTextureRegion bo_reg = { lt_tex[1], 0, 0, 0, 0, 0, (Uint32)BW, (Uint32)BH, 1 };
                                            SDL_GPUTextureTransferInfo bo_dst = { bo_tb, 0, (Uint32)BW, (Uint32)BH };
                                            SDL_DownloadFromGPUTexture(bo_pass, &bo_reg, &bo_dst);
                                            SDL_EndGPUCopyPass(bo_pass);
                                            SDL_GPUFence* bo_f = SDL_SubmitGPUCommandBufferAndAcquireFence(bo_cmd);
                                            if (bo_f) { SDL_WaitForGPUFences(device, true, &bo_f, 1); SDL_ReleaseGPUFence(device, bo_f); }
                                            uint16_t* bp = (uint16_t*)SDL_MapGPUTransferBuffer(device, bo_tb, false);
                                            if (bp)
                                            {
                                                // Khronos PBR Neutral + sRGB, em cinza neutro.
                                                // Constantes verbatim da especificacao, as mesmas
                                                // do fullscreen.hlsl: F90 = 0.04, Ks = 0.76.
                                                auto tela = [](float v) -> int {
                                                    float c = (v > 0.0f) ? v : 0.0f;
                                                    float off = (c < 0.08f) ? (c - 6.25f * c * c) : 0.04f;
                                                    c -= off;
                                                    const float sc = 0.76f;
                                                    if (c >= sc) { const float dd = 1.0f - sc; c = 1.0f - dd * dd / (c + dd - sc); }
                                                    c = std::clamp(c, 0.0f, 1.0f);
                                                    float s = (c <= 0.0031308f) ? (c * 12.92f)
                                                                                : (std::pow(c, 1.0f / 2.4f) * 1.055f - 0.055f);
                                                    return (int)(s * 255.0f + 0.5f);
                                                };
                                                const float g_sr = std::max(grid_sr, 1e-3f);
                                                const float g_lo = Mg - std::max(grid_lo, 0.1f) * g_sr;
                                                const float g_hi = Mg + std::max(grid_hi, 0.1f) * g_sr;
                                                const float g_dz = std::max((g_hi - g_lo) / (float)GNZ, 1e-4f);
                                                const float mix  = std::clamp(grid_mix, 0.0f, 1.0f);
                                                const float L2   = 0.6931472f;
                                                // O PONTO DE SATURACAO DA CURVA, achado por bissecao
                                                // na propria lambda acima - nao e constante escrita a
                                                // mao, e uma PROPRIEDADE da curva que esta no shader.
                                                // Se alguem trocar as constantes da Khronos, este
                                                // numero se corrige sozinho. A 26-11 mediu 13,45 por
                                                // outro caminho; a bissecao devolve 13,49 (0,3%).
                                                // POR QUE ELE PRECISA EXISTIR: acima dele o byte da
                                                // tela para em 255 e deixa de informar. Sem esta
                                                // regua, "255 -> 255" pareceria "a grade nao fez
                                                // nada" mesmo quando ela move o sinal 5 stops.
                                                float sat_lo = 1.0f, sat_hi = 1.0e6f;
                                                for (int it = 0; it < 40; ++it)
                                                {
                                                    float md = std::sqrt(sat_lo * sat_hi);
                                                    if (tela(md) >= 255) sat_hi = md; else sat_lo = md;
                                                }
                                                const float SAT = sat_hi;
                                                int   nval = 0, chap_a = 0, chap_g = 0, joe_a = 0, joe_g = 0;
                                                double gsoma = 0.0; int gn = 0;
                                                double xsoma_a = 0.0, xsoma_g = 0.0;
                                                float pIi = -1e30f, pBa = 0.0f, pBg = 0.0f, pBm = 0.0f;
                                                float pIoa = 0.0f, pIog = 0.0f; int pcx = -1, pcy = -1;
                                                for (int cy = 0; cy < BH; ++cy)
                                                    for (int cx = 0; cx < BW; ++cx)
                                                    {
                                                        double sv = 0.0, sw = 0.0;
                                                        for (int z = 0; z < GNZ; ++z)
                                                        {
                                                            int gi = (((z / 4) * 32 + cy) * GW + (z % 4) * 64 + cx) * 4;
                                                            float wv = h2f(gp[gi + 0]), w = h2f(gp[gi + 1]);
                                                            if (w > 1e-6f && wv > -1e30f && wv < 1e30f) { sv += wv; sw += w; }
                                                        }
                                                        if (!(sw > 1e-6)) continue;
                                                        float Ii = (float)(sv / sw);
                                                        float B_ant = h2f(bp[(cy * BW + cx) * 4 + 0]);
                                                        if (!(B_ant > -1e30f && B_ant < 1e30f)) continue;
                                                        float fz = std::clamp((Ii - g_lo) / g_dz - 0.5f, 0.0f, (float)(GNZ - 1));
                                                        int   z0 = (int)fz; float tz = fz - (float)z0;
                                                        int   z1 = std::min(z0 + 1, GNZ - 1);
                                                        int   i0 = (((z0 / 4) * 32 + cy) * GW + (z0 % 4) * 64 + cx) * 4;
                                                        int   i1 = (((z1 / 4) * 32 + cy) * GW + (z1 % 4) * 64 + cx) * 4;
                                                        float rr = h2f(gp[i0 + 0]) * (1.0f - tz) + h2f(gp[i1 + 0]) * tz;
                                                        float ww = h2f(gp[i0 + 1]) * (1.0f - tz) + h2f(gp[i1 + 1]) * tz;
                                                        float B_gr = (ww > 1e-6f) ? (rr / ww) : B_ant;
                                                        float B_mx = B_ant + (B_gr - B_ant) * mix;
                                                        float io_a = ((B_ant >= Mg) ? lt_c : lt_c_lo) * (B_ant - Mg) + lt_d * (Ii - B_ant) + Mg;
                                                        float io_g = ((B_mx  >= Mg) ? lt_c : lt_c_lo) * (B_mx  - Mg) + lt_d * (Ii - B_mx ) + Mg;
                                                        ++nval;
                                                        int ta = tela(std::exp(io_a)), tg = tela(std::exp(io_g));
                                                        if (ta >= 255) ++chap_a;
                                                        if (tg >= 255) ++chap_g;
                                                        // o joelho da curva em linear: c - 0.04 >= 0.76
                                                        if (ta >= 255) { xsoma_a += (io_a - std::log(SAT)) / L2; xsoma_g += (io_g - std::log(SAT)) / L2; }
                                                        if (std::exp(io_a) >= 0.80f) { ++joe_a; gsoma += (io_g - io_a) / L2; ++gn; }
                                                        if (std::exp(io_g) >= 0.80f) ++joe_g;
                                                        if (Ii > pIi) { pIi = Ii; pcx = cx; pcy = cy; pBa = B_ant; pBg = B_gr; pBm = B_mx; pIoa = io_a; pIog = io_g; }
                                                    }
                                                SDL_UnmapGPUTransferBuffer(device, bo_tb);
                                                if (pcx >= 0)
                                                {
                                                    int ta = tela(std::exp(pIoa)), tg = tela(std::exp(pIog));
                                                    app_log.add("[OPER] celula mais clara (%d,%d) de %d validas | Ii %+.2f st de M | B antigo %+.2f | B grade %+.2f | B usado %+.2f (mistura %.2f)",
                                                        pcx, pcy, nval, (pIi - Mg) / L2, (pBa - Mg) / L2, (pBg - Mg) / L2, (pBm - Mg) / L2, mix);
                                                    app_log.add("[OPER] saida no pico: B ANTIGO %+.2f st de M -> tela %d/255  |  com a GRADE %+.2f st -> tela %d/255  |  GANHO %+.2f stops, %d niveis",
                                                        (pIoa - Mg) / L2, ta, (pIog - Mg) / L2, tg, (pIog - pIoa) / L2, tg - ta);
                                                    // A REGUA QUE NAO SATURA. O byte para em 255; esta linha nao.
                                                    app_log.add("[OPER] ESTOURO no pico (a curva satura em %.2f): antigo %+.2f stops da saturacao -> com a grade %+.2f  |  veredito: %s",
                                                        SAT, (pIoa - std::log(SAT)) / L2, (pIog - std::log(SAT)) / L2,
                                                        (pIog > std::log(SAT)) ? "AINDA CHAPA" : "NAO CHAPA MAIS");
                                                    app_log.add("[OPER] celulas CHAPADAS (255/255): antigo %d de %d -> grade %d  |  estouro medio delas %+.2f -> %+.2f stops  |  acima do joelho: %d -> %d  |  ganho medio onde importa %+.2f stops",
                                                        chap_a, nval, chap_g,
                                                        (chap_a > 0) ? (float)(xsoma_a / chap_a) : 0.0f,
                                                        (chap_a > 0) ? (float)(xsoma_g / chap_a) : 0.0f,
                                                        joe_a, joe_g, (gn > 0) ? (float)(gsoma / gn) : 0.0f);
                                                    SDL_Log("[OPER] pico (%d,%d) Ii %+.2f | Bant %+.2f Bgr %+.2f | tela %d -> %d | estouro %+.2f -> %+.2f st | chapadas %d -> %d",
                                                        pcx, pcy, (pIi - Mg) / L2, (pBa - Mg) / L2, (pBg - Mg) / L2, ta, tg,
                                                        (pIoa - std::log(SAT)) / L2, (pIog - std::log(SAT)) / L2, chap_a, chap_g);
                                                }
                                                else { app_log.add("[OPER] nenhuma celula com peso - a grade saiu vazia neste quadro"); }
                                            }
                                            SDL_ReleaseGPUTransferBuffer(device, bo_tb);
                                        }
                                        else { SDL_Log("[OPER] falhou ao criar o transfer buffer do B antigo: %s", SDL_GetError()); }
                                    }
                                    else { app_log.add("[OPER] curva LEGADO selecionada - este medidor so sabe a Khronos. Troque a curva para medir."); }
                                    SDL_UnmapGPUTransferBuffer(device, gr_tb);
                                }
                                SDL_ReleaseGPUTransferBuffer(device, gr_tb);
                            }
                            else { SDL_Log("[GRADE] falhou ao criar o transfer buffer da grade: %s", SDL_GetError()); }
                        }
                    }
                    SDL_ReleaseGPUTransferBuffer(device, ex_tb);
                }
                else { SDL_Log("[26-20] falhou ao criar o transfer buffer da exposicao: %s", SDL_GetError()); }
            }
        }
    }

    SDL_WaitForGPUIdle(device);

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_ReleaseWindowFromGPUDevice(device, window);

    SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
    if (vertex_buffer) SDL_ReleaseGPUBuffer(device, vertex_buffer);
    if (index_buffer) SDL_ReleaseGPUBuffer(device, index_buffer);
    SDL_ReleaseGPUBuffer(device, neutral_instance_buffer);
    SDL_ReleaseGPUBuffer(device, crowd_instance_buffer);
    SDL_ReleaseGPUBuffer(device, char_instance_buffer);
    SDL_ReleaseGPUBuffer(device, npc_instance_buffer);

    for (auto& a : level_assets) {
        if (a.tex) SDL_ReleaseGPUTexture(device, a.tex);
    }

    for (int i = 0; i < 5; ++i) {
        if (npc_visuals[i].tex) SDL_ReleaseGPUTexture(device, npc_visuals[i].tex);
    }

    if (fallback_tex) SDL_ReleaseGPUTexture(device, fallback_tex);
    if (sampler) SDL_ReleaseGPUSampler(device, sampler);

    SDL_ReleaseGPUGraphicsPipeline(device, sky_pipeline);
    SDL_ReleaseGPUShader(device, sky_vs);
    SDL_ReleaseGPUShader(device, sky_ps);

    SDL_ReleaseGPUBuffer(device, dome_vertex_buffer);
    SDL_ReleaseGPUBuffer(device, dome_index_buffer);

    SDL_ReleaseGPUGraphicsPipeline(device, out_pipeline);
    SDL_ReleaseGPUShader(device, out_vs);
    SDL_ReleaseGPUShader(device, out_ps);
    SDL_ReleaseGPUTexture(device, composite_color);
    SDL_ReleaseGPUSampler(device, point_sampler);

    if (bloom_tex_a) SDL_ReleaseGPUTexture(device, bloom_tex_a);
    if (bloom_tex_b) SDL_ReleaseGPUTexture(device, bloom_tex_b);
    if (bloom_tex_c) SDL_ReleaseGPUTexture(device, bloom_tex_c);
    if (bloom_tex_d) SDL_ReleaseGPUTexture(device, bloom_tex_d);

    SDL_ReleaseGPUGraphicsPipeline(device, bloom_thresh_pipeline);
    SDL_ReleaseGPUGraphicsPipeline(device, bloom_down_pipeline);
    SDL_ReleaseGPUGraphicsPipeline(device, bloom_up_pipeline);
    SDL_ReleaseGPUShader(device, bloom_vs);
    SDL_ReleaseGPUShader(device, bloom_thresh_ps);
    SDL_ReleaseGPUShader(device, bloom_down_ps);
    SDL_ReleaseGPUShader(device, bloom_up_ps);

    SDL_ReleaseGPUTexture(device, shadow_map);
    SDL_ReleaseGPUSampler(device, shadow_diag);
    SDL_ReleaseGPUSampler(device, shadow_cmp);
    SDL_ReleaseGPUGraphicsPipeline(device, shadow_pipeline);
    SDL_ReleaseGPUShader(device, shadow_vs);
    SDL_ReleaseGPUShader(device, shadow_ps);

    SDL_ReleaseGPUGraphicsPipeline(device, fs_pipeline);
    SDL_ReleaseGPUGraphicsPipeline(device, shaft_pipeline); // 26-25
    SDL_ReleaseGPUShader(device, fs_vs);
    SDL_ReleaseGPUShader(device, fs_ps);
    SDL_ReleaseGPUSampler(device, upscale_sampler);

    SDL_ReleaseGPUTexture(device, scene_color);
    SDL_ReleaseGPUTexture(device, normal_id);
    SDL_ReleaseGPUTexture(device, cloud_tex);
    SDL_ReleaseGPUTexture(device, cloud_detail_tex);
    SDL_ReleaseGPUTexture(device, weather_tex);
    SDL_ReleaseGPUTexture(device, trans_tex);
    SDL_ReleaseGPUTexture(device, ms_tex);
    SDL_ReleaseGPUSampler(device, lut_sampler);

    SDL_ReleaseGPUGraphicsPipeline(device, grass_pipeline);
    SDL_ReleaseGPUShader(device, grass_vs);
    SDL_ReleaseGPUShader(device, grass_ps);
    SDL_ReleaseGPUBuffer(device, grass_vbuf);
    SDL_ReleaseGPUBuffer(device, grass_ibuf);
    if (grass_instbuf) SDL_ReleaseGPUBuffer(device, grass_instbuf);

    SDL_ReleaseGPUTransferBuffer(device, dyn_transfer_buf);
    SDL_ReleaseGPUTransferBuffer(device, char_transfer_buf);
    SDL_ReleaseGPUTransferBuffer(device, npc_transfer_buf);

    SDL_ReleaseGPUTexture(device, depth_texture);
    SDL_ReleaseGPUShader(device, vs);
    SDL_ReleaseGPUShader(device, ps);

    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);

    phys::phys_shutdown();

    SDL_Quit();

    return 0;
}