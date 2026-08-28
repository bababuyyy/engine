#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdint>
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
#include "shadow_vs.h"
#include "shadow_ps.h"
#include "clouds/cloud_noise.h"

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
    float light_vp[16];
};

struct SkyParams {
    float to_sun[3];        float pad0;
    float zenith_color[3];  float pad1;
    float horizon_color[3]; float pad2;
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
    float cloud_ms_g2;      float pad_k[3]; // 24J-3: direct scattering Nubis3 (256B)
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
    float tonemap_shoulder; float exposure; float shaft_cloud_occ; float cloud_mid; // 24I-3: altura media da camada
    float shaft_debug;      float shaft_height_falloff; float pad_j[2]; // 24I-4: vista de depuracao (240B)
};
static_assert(sizeof(FsParams) % 16 == 0, "FsParams: alinhamento de 16 bytes quebrado");

struct BloomParams {
    float texel[2];
    float threshold;
    float knee;
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
float cloud_hg_g_frontal = 0.8f;
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
float cloud_silver_spread = 0.3f;
float clima_cov_teto = 0.85f;  // M2.5-24G-3: embriao da maquina de clima
float clima_type_teto = 0.80f;
float cloud_curvatura = 0.000025f; // M2.5-24G-6: horizonte honesto
float cloud_fade_ini = 0.45f;
float cloud_passo_far = 0.0f; // M2.5-24J-2b (A18): crescimento do passo no longe (0 = passo fixo; LIGAR e decisao de retina)

bool sol_automatico = true;
bool sol_hora_manual = false;  // M2.5-24G-5: override de apresentacao (nao persiste de proposito)
float sol_hora = 12.0f;
float arco_inclinacao = 25.0f;
float sun_yaw = 45.0f;
float sun_pitch = -45.0f;
float sun_ambient = 0.55f;
float sun_intensity = 1.2f;

float ray_strength = 0.6f;
float ray_decay = 0.97f;
int ray_samples = 48;
float shaft_g = 0.55f;      // M2.5-24I-5: 0.5-0.7 = faixa da pesquisa para shafts
float shaft_height_falloff = 0.015f; // M2.5-24J (B6): queda exp. da densidade do ar com a altura (1/u); 0 = ar uniforme
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
float tonemap_shoulder = 0.75f; // M2.5-24I: inicio do ombro (abaixo = cores intactas)
float exposure = 1.0f;          // M2.5-24I-2: a chave do HDR (Lagarde 2014 via Hillaire)
float shaft_cloud_occ = 0.95f;  // M2.5-24I-5: quase-binario de verdade (T ~ 0.05 sob nuvem)

bool draw_grass = true;
float grass_scale = 1.0f;
float grass_backlight = 0.35f;
float grass_specular = 0.25f;

bool draw_shadows = true;
bool debug_shadows = false;
float obj_shadow_str = 0.8f;
float obj_shadow_bias = 0.005f;

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
    f << "sun_intensity=" << sun_intensity << "\n";
    f << "ray_strength=" << ray_strength << "\n";
    f << "ray_decay=" << ray_decay << "\n";
    f << "ray_samples=" << ray_samples << "\n";
    f << "shaft_g=" << shaft_g << "\n";
    f << "shaft_sigma=" << shaft_sigma << "\n";
    f << "shaft_height_falloff=" << shaft_height_falloff << "\n";
    f << "draw_bloom=" << (draw_bloom ? "1" : "0") << "\n";
    f << "bloom_intensity=" << bloom_intensity << "\n";
    f << "bloom_threshold=" << bloom_threshold << "\n";
    f << "tonemap_shoulder=" << tonemap_shoulder << "\n";
    f << "exposure=" << exposure << "\n";
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
            else if (key == "cloud_silver_spread") cloud_silver_spread = std::stof(val);
            else if (key == "clima_cov_teto") clima_cov_teto = std::stof(val);
            else if (key == "clima_type_teto") clima_type_teto = std::stof(val);
            else if (key == "cloud_curvatura") cloud_curvatura = std::stof(val);
            else if (key == "cloud_fade_ini") cloud_fade_ini = std::stof(val);
            else if (key == "cloud_passo_far") cloud_passo_far = std::stof(val);
            else if (key == "sol_automatico") sol_automatico = (val == "1");
            else if (key == "arco_inclinacao") arco_inclinacao = std::stof(val);
            else if (key == "sun_yaw") sun_yaw = std::stof(val);
            else if (key == "sun_pitch") sun_pitch = std::stof(val);
            else if (key == "sun_ambient") sun_ambient = std::stof(val);
            else if (key == "sun_intensity") sun_intensity = std::stof(val);
            else if (key == "ray_strength") ray_strength = std::stof(val);
            else if (key == "ray_decay") ray_decay = std::stof(val);
            else if (key == "ray_samples") ray_samples = std::stoi(val);
            else if (key == "shaft_g") shaft_g = std::stof(val);
            else if (key == "shaft_sigma") shaft_sigma = std::stof(val);
            else if (key == "shaft_height_falloff") shaft_height_falloff = std::stof(val);
            else if (key == "draw_bloom") draw_bloom = (val == "1");
            else if (key == "bloom_intensity") bloom_intensity = std::stof(val);
            else if (key == "bloom_threshold") bloom_threshold = std::stof(val);
            else if (key == "tonemap_shoulder") tonemap_shoulder = std::stof(val);
            else if (key == "exposure") exposure = std::stof(val);
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
    ps_info.num_samplers = 4; // M2.5-24G-2: + weather map
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
    fs_ps_info.num_samplers = 6; // M2.5-24H: + depth, shadow map, weather
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
    sky_ps_info.num_samplers = 3; // M2.5-24C: O ceu adere a textura de detalhe 32^3
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
    grass_ps_info.num_samplers = 4; // M2.5-24G-2: + weather map
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
    fallback_tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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

    SDL_GPUSamplerCreateInfo cloud_samp_info = {};
    cloud_samp_info.min_filter = SDL_GPU_FILTER_LINEAR;
    cloud_samp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    cloud_samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    cloud_samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    cloud_samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    cloud_samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    SDL_GPUSampler* cloud_sampler = SDL_CreateGPUSampler(device, &cloud_samp_info);
    if (!cloud_sampler) { SDL_Log("FALHA cloud_sampler: %s", SDL_GetError()); return 1; }

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

    SDL_EndGPUCopyPass(init_pass);
    SDL_SubmitGPUCommandBuffer(init_cmd);
    SDL_ReleaseGPUTransferBuffer(device, fallback_tb);
    SDL_ReleaseGPUTransferBuffer(device, cloud_tb);
    SDL_ReleaseGPUTransferBuffer(device, det_tb);
    SDL_ReleaseGPUTransferBuffer(device, weather_tb);
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


    int pixel_mode = 1;
    const int alturas[] = { 180, 240, 270, 360, 450, 540 };
    int altura_idx = 3;
    int pixel_divisor = 4;
    bool resize_needed = false;

    int internal_w, internal_h;
    if (pixel_mode == 0) {
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
                    asset_tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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
                    asset_tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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
    const char* view_modes[] = { "Final", "Normais+ID", "Sem outline", "Shafts (debug)" };

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

        const char* modos_pixel[] = { "Divisor", "Altura fixa" };
        if (ImGui::Combo("Modo pixel", &pixel_mode, modos_pixel, 2)) {
            resize_needed = true;
        }
        if (pixel_mode == 0) {
            if (ImGui::SliderInt("Pixel divisor", &pixel_divisor, 1, 8)) {
                resize_needed = true;
            }
        }
        else {
            const char* alturas_str[] = { "180", "240", "270", "360", "450", "540" };
            if (ImGui::Combo("Altura interna", &altura_idx, alturas_str, 6)) {
                resize_needed = true;
            }
        }

        ImGui::Combo("Vista", &view_mode, view_modes, 4);

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
            app_log.add("Fisica da Pesquisa aplicada (arte intacta). Salvar Config p/ persistir.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(valores com fonte)");
        ImGui::Checkbox("Ceu (Processual)", &draw_sky);
        ImGui::SliderFloat("Densidade do Fog", &fog_density, 0.0f, 0.02f, "%.4f");

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
        if (sol_hora_manual) ImGui::SliderFloat("Hora do dia", &sol_hora, 0.0f, 24.0f, "%.1fh");
        if (sol_automatico) {
            ImGui::SliderFloat("Inclinacao do arco", &arco_inclinacao, 0.0f, 45.0f);
        }
        ImGui::BeginDisabled(sol_automatico);
        ImGui::SliderFloat("Yaw", &sun_yaw, 0.0f, 360.0f);
        ImGui::SliderFloat("Pitch", &sun_pitch, -89.0f, -10.0f);
        ImGui::EndDisabled();
        ImGui::SliderFloat("Ambient (Dia)", &sun_ambient, 0.0f, 1.0f);
        ImGui::SliderFloat3("Color", sun_color_arr, 0.0f, 1.0f);
        ImGui::SliderFloat("Intensity", &sun_intensity, 0.0f, 5.0f);

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
        ImGui::SliderFloat("Sigma (densidade do ar)", &shaft_sigma, 0.0002f, 0.02f, "%.4f");
        ImGui::SliderFloat("Queda por altura", &shaft_height_falloff, 0.0f, 0.05f, "%.4f");

        ImGui::Separator();
        ImGui::Text("Dual-Kawase Bloom");
        ImGui::Checkbox("Bloom", &draw_bloom);
        ImGui::SliderFloat("Bloom: Forca", &bloom_intensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Bloom: Limiar", &bloom_threshold, 0.4f, 2.5f); // 24I: alcance HDR
        ImGui::SliderFloat("Tonemap: ombro", &tonemap_shoulder, 0.5f, 0.95f);
        ImGui::SliderFloat("Exposicao", &exposure, 0.1f, 4.0f);
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
            float effective_intensity = sun_intensity;
            glm::vec3 c_zenith, c_horizon;
            float effective_ambient = sun_ambient;

            if (sol_automatico) {
                float t_frac = (g_hours * 60.0f + g_minutes) / 1440.0f;
                if (sol_hora_manual) t_frac = sol_hora / 24.0f; // M2.5-24G-5: so a LUZ obedece; o tick segue vivo
                float theta = (t_frac - 0.25f) * 2.0f * 3.14159265f;
                float phi = glm::radians(arco_inclinacao);

                to_sun_vec = glm::normalize(glm::vec3(
                    std::cos(theta),
                    std::sin(theta) * std::cos(phi),
                    std::sin(theta) * std::sin(phi)
                ));

                float sol_fator = glm::smoothstep(-0.05f, 0.25f, to_sun_vec.y);
                effective_intensity = sun_intensity * sol_fator;

                auto saturate = [](float x) { return std::max(0.0f, std::min(1.0f, x)); };
                float day_w = saturate(to_sun_vec.y);
                float sunset_w = saturate(1.0f - std::fabs(to_sun_vec.y));
                float night_w = saturate(-to_sun_vec.y);

                glm::vec3 zd(0.25f, 0.55f, 0.95f);
                glm::vec3 hd(0.60f, 0.80f, 0.98f);
                glm::vec3 zp(0.30f, 0.30f, 0.60f);
                glm::vec3 hp(0.98f, 0.55f, 0.25f);
                glm::vec3 zn(0.02f, 0.03f, 0.08f);
                glm::vec3 hn(0.05f, 0.07f, 0.15f);

                c_zenith = day_w * zd + sunset_w * zp + night_w * zn;
                c_horizon = day_w * hd + sunset_w * hp + night_w * hn;

                // M2.5-19C: Base lunar noturna - piso garantido de luz
                float min_ambient_night = 0.04f;
                effective_ambient = sun_ambient * (1.0f - night_w) + min_ambient_night * night_w;
                effective_intensity = sun_intensity * (1.0f - night_w) + 0.1f * night_w; // Lua minima iluminando o campo
            }
            else {
                float sy_rad = glm::radians(sun_yaw);
                float sp_rad = glm::radians(sun_pitch);
                to_sun_vec = glm::normalize(glm::vec3(
                    std::cos(sp_rad) * std::sin(sy_rad),
                    std::sin(sp_rad),
                    std::cos(sp_rad) * std::cos(sy_rad)
                ));

                c_zenith = glm::vec3(0.1f, 0.4f, 0.8f);
                c_horizon = glm::vec3(0.4f, 0.7f, 0.9f);
                effective_ambient = sun_ambient;
            }

            // A luz vem DO sol PARA O CHAO (convencao da iluminacao do cenario mantida)
            glm::vec3 s_dir = -to_sun_vec;

            glm::vec3 c_cloud_day(0.95f, 0.95f, 0.95f);
            glm::vec3 c_cloud_sunset(0.95f, 0.85f, 0.70f);
            glm::vec3 c_cloud_night(0.08f, 0.1f, 0.15f);

            glm::vec3 c_sky_amb_day(0.45f, 0.65f, 0.95f); // Azulada contra a luz solar
            glm::vec3 c_sky_amb_sunset(0.85f, 0.60f, 0.70f);
            glm::vec3 c_sky_amb_night(0.02f, 0.03f, 0.08f);

            glm::vec3 c_cloud_shad_day(0.55f, 0.60f, 0.75f); // Azul lavanda opaco
            glm::vec3 c_cloud_shad_sunset(0.50f, 0.35f, 0.40f); // Roxo quente
            glm::vec3 c_cloud_shad_night(0.01f, 0.01f, 0.04f); // Azul escuro profundo

            glm::vec3 effective_cloud_color = c_cloud_day;
            glm::vec3 effective_sky_amb = c_sky_amb_day;
            glm::vec3 effective_cloud_shad = c_cloud_shad_day;

            // M2.5-24: Computacao da Matriz Orto p/ Sombra
            glm::mat4 light_vp = glm::mat4(1.0f);
            if (draw_shadows && to_sun_vec.y > 0.01f) {
                float ortho_size = 340.0f; // cobre a geometria de teste em x=300
                glm::mat4 light_proj = glm::orthoLH_ZO(-ortho_size, ortho_size, -ortho_size, ortho_size, -300.0f, 300.0f);
                glm::mat4 light_view = glm::lookAtLH(to_sun_vec * 200.0f, glm::vec3(0.0f), glm::vec3(0, 1, 0));
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
                effective_sky_amb = day_w * c_sky_amb_day + sunset_w * c_sky_amb_sunset + night_w * c_sky_amb_night;
                effective_cloud_shad = day_w * c_cloud_shad_day + sunset_w * c_cloud_shad_sunset + night_w * c_cloud_shad_night;
            }

            SkyParams sky_params;
            sky_params.to_sun[0] = to_sun_vec.x;
            sky_params.to_sun[1] = to_sun_vec.y;
            sky_params.to_sun[2] = to_sun_vec.z;
            sky_params.pad0 = 0.0f;
            sky_params.zenith_color[0] = c_zenith.x;
            sky_params.zenith_color[1] = c_zenith.y;
            sky_params.zenith_color[2] = c_zenith.z;
            sky_params.pad1 = 0.0f;
            sky_params.horizon_color[0] = c_horizon.x;
            sky_params.horizon_color[1] = c_horizon.y;
            sky_params.horizon_color[2] = c_horizon.z;
            sky_params.pad2 = 0.0f;
            sky_params.sun_color[0] = sun_color_arr[0] * effective_intensity;
            sky_params.sun_color[1] = sun_color_arr[1] * effective_intensity;
            sky_params.sun_color[2] = sun_color_arr[2] * effective_intensity;
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

            sky_params.sky_ambient_color[0] = effective_sky_amb.r;
            sky_params.sky_ambient_color[1] = effective_sky_amb.g;
            sky_params.sky_ambient_color[2] = effective_sky_amb.b;
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
            sky_params.pad_k[0] = 0.0f; sky_params.pad_k[1] = 0.0f; sky_params.pad_k[2] = 0.0f;

            SunParams sun_params;
            sun_params.dir[0] = s_dir.x;
            sun_params.dir[1] = s_dir.y;
            sun_params.dir[2] = s_dir.z;
            sun_params.ambient = effective_ambient;
            sun_params.color[0] = sun_color_arr[0] * sun_intensity;
            sun_params.color[1] = sun_color_arr[1] * sun_intensity;
            sun_params.color[2] = sun_color_arr[2] * sun_intensity;
            sun_params.cam_pos[0] = cam_pos.x;
            sun_params.cam_pos[1] = cam_pos.y;
            sun_params.cam_pos[2] = cam_pos.z;
            sun_params.toon_cuts = (float)toon_cuts;
            sun_params.toon_soft = toon_soft;
            sun_params.wind_dir[0] = cos(w_rad);
            sun_params.wind_dir[1] = sin(w_rad);
            sun_params.fog_color[0] = c_horizon.x;
            sun_params.fog_color[1] = c_horizon.y;
            sun_params.fog_color[2] = c_horizon.z;
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

                SDL_GPUTextureSamplerBinding sky_samps[3] = {
                    { weather_tex, sampler }, // M2.5-24G-1: weather map no t0 do ceu
                    { cloud_tex, cloud_sampler },
                    { cloud_detail_tex, cloud_sampler }
                };
                SDL_BindGPUFragmentSamplers(scene_pass, 0, sky_samps, 3);

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
                SDL_GPUTextureSamplerBinding samps[4] = {
                    { color_tex ? color_tex : fallback_tex, sampler },
                    { shadow_map, shadow_cmp },
                    { shadow_map, shadow_diag },
                    { weather_tex, sampler }
                };
                SDL_BindGPUFragmentSamplers(scene_pass, 0, samps, 4);
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
            op.texel[0] = 1.0f / (float)internal_w;
            op.texel[1] = 1.0f / (float)internal_h;
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

            // ============================================================================
            // M2.5-23: Dual-Kawase Bloom Ping-Pong Pass
            // ============================================================================
            if (draw_bloom && view_mode == 0) {
                BloomParams bp;
                bp.threshold = bloom_threshold;
                bp.knee = 0.5f;

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
            fs_params.sun_color[0] = sun_color_arr[0] * effective_intensity * shaft_gate;
            fs_params.sun_color[1] = sun_color_arr[1] * effective_intensity * shaft_gate;
            fs_params.sun_color[2] = sun_color_arr[2] * effective_intensity * shaft_gate;
            fs_params.shaft_samples = (float)ray_samples;
            fs_params.tex_size[0] = (float)internal_w;
            fs_params.tex_size[1] = (float)internal_h;
            fs_params.wind_off[0] = cos(w_rad) * sun_params.cloud_time * -200.0f;
            fs_params.wind_off[1] = sin(w_rad) * sun_params.cloud_time * -200.0f;
            fs_params.shaft_sigma = shaft_sigma;
            fs_params.cloud_cover = draw_clouds ? cloud_cobertura : 0.0f;
            fs_params.shadow_str_fs = shadow_str;
            fs_params.tonemap_shoulder = tonemap_shoulder;
            fs_params.exposure = exposure;
            fs_params.shaft_cloud_occ = shaft_cloud_occ;
            fs_params.cloud_mid = (cloud_h_min + cloud_h_max) * 0.5f;
            fs_params.shaft_debug = (view_mode == 3) ? 1.0f : 0.0f;
            fs_params.shaft_height_falloff = shaft_height_falloff; fs_params.pad_j[0] = 0.0f; fs_params.pad_j[1] = 0.0f;
            // Anular shafts nas views de debug para nao sujar a validacao
            fs_params.shaft_intensity = (view_mode == 0 || view_mode == 3) ? ray_strength : 0.0f;
            fs_params.bloom_intensity = (draw_bloom && view_mode == 0) ? bloom_intensity : 0.0f;

            SDL_PushGPUFragmentUniformData(cmd_buffer, 0, &fs_params, sizeof(FsParams));

            SDL_GPUTexture* upscale_source = composite_color;
            if (view_mode == 1) upscale_source = normal_id;
            else if (view_mode == 2) upscale_source = scene_color;

            // O Pass Fullscreen agora bebe da id_mask pra calcular a oclusão volumetrica da luz e da cor unificada do bloom kawase
            SDL_GPUTextureSamplerBinding fs_samp_binds[6] = {
                { upscale_source, upscale_sampler },
                { normal_id, upscale_sampler },
                { bloom_tex_a ? bloom_tex_a : fallback_tex, upscale_sampler },
                { depth_texture, point_sampler },
                { shadow_map, shadow_cmp },
                { weather_tex, sampler }
            };
            SDL_BindGPUFragmentSamplers(fs_pass, 0, fs_samp_binds, 6);

            SDL_DrawGPUPrimitives(fs_pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(fs_pass);

            SDL_GPUColorTargetInfo ui_target = {};
            ui_target.texture = swapchain_texture;
            ui_target.load_op = SDL_GPU_LOADOP_LOAD;
            ui_target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* ui_pass = SDL_BeginGPURenderPass(cmd_buffer, &ui_target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd_buffer, ui_pass);
            SDL_EndGPURenderPass(ui_pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd_buffer);
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
    SDL_ReleaseGPUShader(device, fs_vs);
    SDL_ReleaseGPUShader(device, fs_ps);
    SDL_ReleaseGPUSampler(device, upscale_sampler);

    SDL_ReleaseGPUTexture(device, scene_color);
    SDL_ReleaseGPUTexture(device, normal_id);
    SDL_ReleaseGPUTexture(device, cloud_tex);
    SDL_ReleaseGPUTexture(device, cloud_detail_tex);
    SDL_ReleaseGPUTexture(device, weather_tex);

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