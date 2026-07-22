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

#include "core/jobs.h"
#include "sim/sim.h"
#include "phys/physics.h"
#include "level/level.h"
#include "gen/gen.h"

typedef unsigned char BYTE;
#include "basic_vs.h"
#include "basic_ps.h"

// ============================================================================
// SISTEMA DE FEEL / MÁQUINA DE ESTADO DO JOGADOR
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

struct Vertex { float x, y, z; float r, g, b; };
struct InstanceData { float x, y, z; float r, g, b; };

struct MeshRange {
    uint32_t first_index;
    uint32_t index_count;
};

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window* window = SDL_CreateWindow("Minha Engine", 1280, 720, 0);
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
    SDL_GPUShader* ps = SDL_CreateGPUShader(device, &ps_info);
    if (!ps) return 1;

    core::JobSystem js;

    sim::World world;
    sim::init_world(world, 42, 100000);

    phys::phys_init();

    phys::phys_create_static_box(300.0f, -0.5f, 0.0f, 15.0f, 0.5f, 15.0f);
    phys::phys_create_static_box(295.0f, 1.0f, 5.0f, 5.0f, 1.0f, 0.5f, 0.785398f);
    phys::phys_create_static_box(305.0f, 1.0f, 5.0f, 5.0f, 1.0f, 0.5f, -0.785398f);
    phys::phys_create_static_box(300.0f, 2.5f, 12.0f, 5.0f, 2.5f, 0.5f);
    phys::phys_create_static_box(300.0f, 0.0f, -10.0f, 2.0f, 0.1f, 3.0f, 0.0f, 0.785398f);
    // M1-6b: Rampa invisível abraçando perfeitamente os 8 degraus (pitch calculado via atan)
    phys::phys_create_static_box(305.0f, 1.0f, -13.75f, 2.0f, 0.125f, 2.236f, 0.0f, 0.4636f);

    uint32_t plat_id = phys::phys_create_kinematic_box(0.0f, 0.0f, 0.0f, 2.0f, 0.25f, 2.0f);
    phys::phys_create_character(0.0f, 2.0f, 0.0f);

    AppLog app_log;

    MeshRange nivel_range, tortura_range, cubo_multidao, cubo_jogador, cubo_jogador_agachado, cubo_plataforma;
    size_t level_box_count = 0;
    std::vector<uint32_t> level_body_ids;

    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUBuffer* index_buffer = nullptr;

    glm::vec3 current_player_pos(0.0f);
    glm::vec3 prev_player_pos(0.0f);
    phys::PhysState active_phys_state = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false };

    auto build_level = [&](const level::LevelData& lvl) {
        for (uint32_t id : level_body_ids) phys::phys_destroy_body(id);
        level_body_ids.clear();

        if (vertex_buffer) SDL_ReleaseGPUBuffer(device, vertex_buffer);
        if (index_buffer) SDL_ReleaseGPUBuffer(device, index_buffer);

        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;

        auto add_box = [&](float cx, float cy, float cz, float hx, float hy, float hz, float r, float g, float b, float rot_yaw = 0.0f, float rot_pitch = 0.0f) {
            uint32_t base = (uint32_t)vertices.size();
            uint32_t start_idx = (uint32_t)indices.size();

            glm::mat4 mat(1.0f);
            mat = glm::translate(mat, glm::vec3(cx, cy, cz));
            mat = glm::rotate(mat, rot_yaw, glm::vec3(0, 1, 0));
            mat = glm::rotate(mat, rot_pitch, glm::vec3(1, 0, 0));
            mat = glm::scale(mat, glm::vec3(hx, hy, hz));

            glm::vec3 corners[8] = {
                {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
                {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1}
            };
            for (int i = 0; i < 8; ++i) {
                glm::vec4 p = mat * glm::vec4(corners[i], 1.0f);
                vertices.push_back({ p.x, p.y, p.z, r, g, b });
            }

            uint16_t local_inds[36] = {
                0,1,2, 0,2,3,  1,5,6, 1,6,2,  5,4,7, 5,7,6,
                4,0,3, 4,3,7,  3,2,6, 3,6,7,  4,5,1, 4,1,0
            };
            for (int i = 0; i < 36; ++i) indices.push_back(base + local_inds[i]);

            return MeshRange{ start_idx, 36 };
            };

        uint32_t lvl_start = indices.size();
        for (const auto& b : lvl.boxes) {
            add_box(b.cx, b.cy, b.cz, b.hx, b.hy, b.hz, b.r, b.g, b.b);
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
            // Visual preservado, física tratada inteiramente pela nova rampa invisível externa
            add_box(305.0f, 0.125f + i * 0.25f, -12.0f - i * 0.5f, 2.0f, 0.125f, 0.25f, 0.5f, 0.5f, 0.5f);
        }
        tortura_range = { tortura_start, (uint32_t)indices.size() - tortura_start };

        cubo_multidao = add_box(0.0f, 0.15f, 0.0f, 0.15f, 0.15f, 0.15f, 1.0f, 1.0f, 1.0f);
        cubo_jogador = add_box(0.0f, 0.9f, 0.0f, 0.4f, 0.9f, 0.4f, 1.0f, 1.0f, 1.0f);
        cubo_jogador_agachado = add_box(0.0f, 0.45f, 0.0f, 0.4f, 0.45f, 0.4f, 1.0f, 1.0f, 1.0f);
        cubo_plataforma = add_box(0.0f, 0.0f, 0.0f, 2.0f, 0.25f, 2.0f, 1.0f, 1.0f, 1.0f);

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

        SDL_EndGPUCopyPass(copy_pass);
        SDL_SubmitGPUCommandBuffer(copy_cmd);
        SDL_ReleaseGPUTransferBuffer(device, temp_transfer);

        phys::phys_teleport_character(lvl.spawn.x, lvl.spawn.y, lvl.spawn.z);
        phys::phys_refresh_contacts();

        active_phys_state = { lvl.spawn.x, lvl.spawn.y, lvl.spawn.z, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false };
        current_player_pos = glm::vec3(lvl.spawn.x, lvl.spawn.y, lvl.spawn.z);
        prev_player_pos = current_player_pos;

        level_box_count = lvl.boxes.size();
        };

    level::LevelData initial_lvl = level::load_level(LEVELS_DIR "arena.txt");
    if (initial_lvl.ok) {
        build_level(initial_lvl);
        app_log.add("Nivel recarregado: arena.txt (%zu caixas)", level_box_count);
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

    SDL_GPUTransferBufferCreateInfo dyn_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (uint32_t)(world.count * sizeof(InstanceData)) };
    SDL_GPUTransferBuffer* dyn_transfer_buf = SDL_CreateGPUTransferBuffer(device, &dyn_tb_info);

    SDL_GPUTransferBufferCreateInfo char_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 2 * sizeof(InstanceData) };
    SDL_GPUTransferBuffer* char_transfer_buf = SDL_CreateGPUTransferBuffer(device, &char_tb_info);

    SDL_GPUTextureCreateInfo depth_info = {};
    depth_info.type = SDL_GPU_TEXTURETYPE_2D;
    depth_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    depth_info.width = 1280;
    depth_info.height = 720;
    depth_info.layer_count_or_depth = 1;
    depth_info.num_levels = 1;
    depth_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    depth_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    SDL_GPUTexture* depth_texture = SDL_CreateGPUTexture(device, &depth_info);

    SDL_GPUGraphicsPipelineCreateInfo pipe_info = {};
    pipe_info.vertex_shader = vs;
    pipe_info.fragment_shader = ps;
    pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;

    pipe_info.depth_stencil_state.enable_depth_test = true;
    pipe_info.depth_stencil_state.enable_depth_write = true;
    pipe_info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

    SDL_GPUVertexAttribute attrs[4] = {};
    attrs[0].location = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[0].offset = 0;                 attrs[0].buffer_slot = 0;
    attrs[1].location = 1; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[1].offset = 3 * sizeof(float); attrs[1].buffer_slot = 0;
    attrs[2].location = 2; attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[2].offset = 0;                 attrs[2].buffer_slot = 1;
    attrs[3].location = 3; attrs[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[3].offset = 3 * sizeof(float); attrs[3].buffer_slot = 1;

    SDL_GPUVertexBufferDescription vb_desc[2] = {};
    vb_desc[0].slot = 0; vb_desc[0].pitch = sizeof(Vertex);       vb_desc[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vb_desc[1].slot = 1; vb_desc[1].pitch = sizeof(InstanceData); vb_desc[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;

    pipe_info.vertex_input_state.num_vertex_attributes = 4;
    pipe_info.vertex_input_state.vertex_attributes = attrs;
    pipe_info.vertex_input_state.num_vertex_buffers = 2;
    pipe_info.vertex_input_state.vertex_buffer_descriptions = vb_desc;

    SDL_GPUColorTargetDescription color_desc = {};
    color_desc.format = SDL_GetGPUSwapchainTextureFormat(device, window);
    pipe_info.target_info.num_color_targets = 1;
    pipe_info.target_info.color_target_descriptions = &color_desc;
    pipe_info.target_info.has_depth_stencil_target = true;
    pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipe_info);
    if (!pipeline) return 1;

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

    app_log.add("JobSystem ativado em pool com %d workers físicos disponíveis.", max_hardware_threads);
    app_log.add("Mundo seed=%llu", (unsigned long long)world.seed);
    app_log.add("AVISO: Pause e un-pause realocados para a tecla [P]. Espaco configurado p/ pulos.");

    glm::vec3 cam_pos(0.0f, 35.0f, -70.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = -0.4f;

    // M1-6b: Zoom Desacoplado da Histerese 
    float wheel_target = 9.0f;
    float zoom_arm = 9.0f;
    float actual_arm_length = 9.0f;

    bool exp_walk_stairs = true;
    float exp_stick_step_down = 0.5f;

    bool rmb_down = false;
    bool cam_follow = false;
    bool in_torture = false;
    bool is_crouched = false;

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

    // Instrumentação de Stalls Lateral (Pesquisa Validada)
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
                        SDL_WaitForGPUIdle(device);
                        level::LevelData new_lvl = level::load_level(LEVELS_DIR "arena.txt");
                        if (new_lvl.ok) {
                            build_level(new_lvl);
                            app_log.add("Nivel recarregado: arena.txt (%zu caixas)", level_box_count);
                        }
                        else app_log.add("Erro: Falha ao recarregar arena.txt");
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

        accumulator += frame_time * time_scale;

        uint64_t sim_start_timer = SDL_GetPerformanceCounter();
        int ticks_this_frame = 0;

        while (accumulator >= DT) {
            sim::step(world, sim_tick, js, sim_threads);

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

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Tempo");
        if (ImGui::SliderInt("Threads", &sim_threads, 1, max_hardware_threads)) {
            app_log.add("Poder de processamento ajustado para %d threads.", sim_threads);
        }

        if (ImGui::Button("Pausa [P]")) set_scale(0, "Botão"); ImGui::SameLine();
        if (ImGui::Button("1x"))        set_scale(1, "Botão"); ImGui::SameLine();
        if (ImGui::Button("10x"))       set_scale(10, "Botão"); ImGui::SameLine();
        if (ImGui::Button("100x"))      set_scale(100, "Botão"); ImGui::SameLine();
        if (ImGui::Button("1000x"))     set_scale(1000, "Botão");

        uint64_t game_seconds = sim_tick / SIM_HZ;
        ImGui::Text("Tick: %llu", (unsigned long long)sim_tick);
        ImGui::Text("Mundo: Dia %llu %02llu:%02llu", (unsigned long long)(game_seconds / 86400), (unsigned long long)((game_seconds / 3600) % 24), (unsigned long long)((game_seconds / 60) % 60));
        ImGui::Text("Level Info: N/A (%zu caixas)", level_box_count);
        ImGui::Text("Camera Pos: %.2f, %.2f, %.2f", cam_pos.x, cam_pos.y, cam_pos.z);
        ImGui::Text("FPS: %.1f", last_frame_ms > 0.0 ? 1000.0 / last_frame_ms : 0.0);

        // CORREÇÃO: Métricas analíticas do pipeline devolvidas
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
            if (ImGui::Button("Rodar até e pausar")) {
                arm_stop = true;
                audit_hash = true;
                app_log.add("Auditoria armada para o tick %llu.", (unsigned long long)stop_at_tick);
            }
        }
        else {
            ImGui::BeginDisabled();
            ImGui::Button("Rodar até e pausar");
            ImGui::EndDisabled();
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
        ImGui::End();

        ImGui::Begin("Log");
        for (const std::string& line : app_log.lines) ImGui::TextUnformatted(line.c_str());
        if (app_log.auto_scroll) { ImGui::SetScrollHereY(1.0f); app_log.auto_scroll = false; }
        ImGui::End();

        game_seconds = sim_tick / SIM_HZ;
        if (game_seconds != last_displayed_second || time_scale != last_displayed_scale) {
            last_displayed_second = game_seconds;
            last_displayed_scale = time_scale;

            uint64_t days = game_seconds / 86400;
            uint64_t hours = (game_seconds / 3600) % 24;
            uint64_t minutes = (game_seconds / 60) % 60;

            char title_buffer[256];
            if (time_scale == 0) {
                snprintf(title_buffer, sizeof(title_buffer), "Minha Engine | dia %llu %02llu:%02llu | tick %llu | pausado",
                    (unsigned long long)days, (unsigned long long)hours, (unsigned long long)minutes, (unsigned long long)sim_tick);
            }
            else {
                snprintf(title_buffer, sizeof(title_buffer), "Minha Engine | dia %llu %02llu:%02llu | tick %llu | %lldx",
                    (unsigned long long)days, (unsigned long long)hours, (unsigned long long)minutes, (unsigned long long)sim_tick, (long long)time_scale);
            }
            SDL_SetWindowTitle(window, title_buffer);
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

            SDL_GPUCopyPass* char_copy_pass = SDL_BeginGPUCopyPass(cmd_buffer);

            SDL_GPUTransferBufferLocation loc = { dyn_transfer_buf, 0 };
            SDL_GPUBufferRegion reg = { crowd_instance_buffer, 0, (uint32_t)(world.count * sizeof(InstanceData)) };
            SDL_UploadToGPUBuffer(char_copy_pass, &loc, &reg, true);

            SDL_GPUTransferBufferLocation char_loc = { char_transfer_buf, 0 };
            SDL_GPUBufferRegion char_reg = { char_instance_buffer, 0, 2 * sizeof(InstanceData) };
            SDL_UploadToGPUBuffer(char_copy_pass, &char_loc, &char_reg, true);

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

        if (swapchain_texture) {
            glm::mat4 proj = glm::perspectiveLH_ZO(glm::radians(70.0f), 1280.0f / 720.0f, 0.1f, 500.0f);
            glm::mat4 view = glm::lookAtLH(cam_pos, cam_pos + front, glm::vec3(0, 1, 0));
            glm::mat4 mvp = proj * view;

            SDL_PushGPUVertexUniformData(cmd_buffer, 0, glm::value_ptr(mvp), sizeof(mvp));
            ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd_buffer);

            SDL_GPUColorTargetInfo color_target = {};
            color_target.texture = swapchain_texture;
            color_target.clear_color = { 0.05f, 0.07f, 0.12f, 1.0f };
            color_target.load_op = SDL_GPU_LOADOP_CLEAR;
            color_target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPUDepthStencilTargetInfo depth_target = {};
            depth_target.texture = depth_texture;
            depth_target.clear_depth = 1.0f;
            depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
            depth_target.store_op = SDL_GPU_STOREOP_STORE;
            depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            depth_target.cycle = true;

            SDL_GPURenderPass* scene_pass = SDL_BeginGPURenderPass(cmd_buffer, &color_target, 1, &depth_target);
            SDL_BindGPUGraphicsPipeline(scene_pass, pipeline);

            SDL_GPUBufferBinding vb_bind = { vertex_buffer, 0 };
            SDL_BindGPUVertexBuffers(scene_pass, 0, &vb_bind, 1);

            SDL_GPUBufferBinding ib_bind = { index_buffer, 0 };
            SDL_BindGPUIndexBuffer(scene_pass, &ib_bind, SDL_GPU_INDEXELEMENTSIZE_16BIT);

            SDL_GPUBufferBinding inst_bind_floor = { neutral_instance_buffer, 0 };
            SDL_BindGPUVertexBuffers(scene_pass, 1, &inst_bind_floor, 1);

            SDL_DrawGPUIndexedPrimitives(scene_pass, nivel_range.index_count, 1, nivel_range.first_index, 0, 0);

            SDL_DrawGPUIndexedPrimitives(scene_pass, tortura_range.index_count, 1, tortura_range.first_index, 0, 0);

            SDL_GPUBufferBinding inst_bind_crowd = { crowd_instance_buffer, 0 };
            SDL_BindGPUVertexBuffers(scene_pass, 1, &inst_bind_crowd, 1);
            SDL_DrawGPUIndexedPrimitives(scene_pass, cubo_multidao.index_count, 100000, cubo_multidao.first_index, 0, 0);

            SDL_GPUBufferBinding inst_bind_char = { char_instance_buffer, 0 };
            SDL_BindGPUVertexBuffers(scene_pass, 1, &inst_bind_char, 1);

            MeshRange range_active_player = is_crouched ? cubo_jogador_agachado : cubo_jogador;
            SDL_DrawGPUIndexedPrimitives(scene_pass, range_active_player.index_count, 1, range_active_player.first_index, 0, 0);

            SDL_DrawGPUIndexedPrimitives(scene_pass, cubo_plataforma.index_count, 1, cubo_plataforma.first_index, 0, 1);

            SDL_EndGPURenderPass(scene_pass);

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

    SDL_ReleaseGPUTransferBuffer(device, dyn_transfer_buf);
    SDL_ReleaseGPUTransferBuffer(device, char_transfer_buf);

    SDL_ReleaseGPUTexture(device, depth_texture);
    SDL_ReleaseGPUShader(device, vs);
    SDL_ReleaseGPUShader(device, ps);

    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);

    phys::phys_shutdown();

    SDL_Quit();

    return 0;
}