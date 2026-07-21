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

#include "core/jobs.h"
#include "sim/sim.h"

typedef unsigned char BYTE;
#include "basic_vs.h"
#include "basic_ps.h"

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

    Vertex vertices[] = {
        { -100.0f, 0.0f,  100.0f,   0.35f, 0.35f, 0.35f }, {  100.0f, 0.0f,  100.0f,   0.35f, 0.35f, 0.35f },
        {  100.0f, 0.0f, -100.0f,   0.35f, 0.35f, 0.35f }, { -100.0f, 0.0f, -100.0f,   0.35f, 0.35f, 0.35f },
        { -0.15f, 0.0f,  0.15f,   1.0f, 0.0f, 0.0f }, {  0.15f, 0.0f,  0.15f,   0.0f, 1.0f, 0.0f },
        {  0.15f, 0.0f, -0.15f,   0.0f, 0.0f, 1.0f }, { -0.15f, 0.0f, -0.15f,   1.0f, 1.0f, 0.0f },
        { -0.15f, 0.3f,  0.15f,   1.0f, 0.0f, 1.0f }, {  0.15f, 0.3f,  0.15f,   0.0f, 1.0f, 1.0f },
        {  0.15f, 0.3f, -0.15f,   1.0f, 1.0f, 1.0f }, { -0.15f, 0.3f, -0.15f,   0.0f, 0.0f, 0.0f }
    };

    uint16_t indices[] = {
        0, 1, 2, 0, 2, 3,  4, 5, 6, 4, 6, 7,  8, 10, 9, 8, 11, 10,
        4, 7, 11, 4, 11, 8,  5, 9, 10, 5, 10, 6,  4, 8, 9, 4, 9, 5,  7, 6, 10, 7, 10, 11
    };

    InstanceData neutral_inst = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };

    uint32_t vb_size = sizeof(vertices);
    uint32_t ib_size = sizeof(indices);
    uint32_t neutral_size = sizeof(InstanceData);

    SDL_GPUTransferBufferCreateInfo tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vb_size + ib_size + neutral_size };
    SDL_GPUTransferBuffer* temp_transfer = SDL_CreateGPUTransferBuffer(device, &tb_info);

    void* map = SDL_MapGPUTransferBuffer(device, temp_transfer, false);
    memcpy(map, vertices, vb_size);
    memcpy((uint8_t*)map + vb_size, indices, ib_size);
    memcpy((uint8_t*)map + vb_size + ib_size, &neutral_inst, neutral_size);
    SDL_UnmapGPUTransferBuffer(device, temp_transfer);

    SDL_GPUBufferCreateInfo vb_info = { SDL_GPU_BUFFERUSAGE_VERTEX, vb_size };
    SDL_GPUBuffer* vertex_buffer = SDL_CreateGPUBuffer(device, &vb_info);

    SDL_GPUBufferCreateInfo ib_info = { SDL_GPU_BUFFERUSAGE_INDEX, ib_size };
    SDL_GPUBuffer* index_buffer = SDL_CreateGPUBuffer(device, &ib_info);

    SDL_GPUBufferCreateInfo n_inst_info = { SDL_GPU_BUFFERUSAGE_VERTEX, neutral_size };
    SDL_GPUBuffer* neutral_instance_buffer = SDL_CreateGPUBuffer(device, &n_inst_info);

    SDL_GPUBufferCreateInfo c_inst_info = { SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(world.count * sizeof(InstanceData)) };
    SDL_GPUBuffer* crowd_instance_buffer = SDL_CreateGPUBuffer(device, &c_inst_info);

    SDL_GPUCommandBuffer* copy_cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(copy_cmd);

    SDL_GPUTransferBufferLocation vb_loc = { temp_transfer, 0 };
    SDL_GPUBufferRegion vb_reg = { vertex_buffer, 0, vb_size };
    SDL_UploadToGPUBuffer(copy_pass, &vb_loc, &vb_reg, false);

    SDL_GPUTransferBufferLocation ib_loc = { temp_transfer, vb_size };
    SDL_GPUBufferRegion ib_reg = { index_buffer, 0, ib_size };
    SDL_UploadToGPUBuffer(copy_pass, &ib_loc, &ib_reg, false);

    SDL_GPUTransferBufferLocation n_loc = { temp_transfer, vb_size + ib_size };
    SDL_GPUBufferRegion n_reg = { neutral_instance_buffer, 0, neutral_size };
    SDL_UploadToGPUBuffer(copy_pass, &n_loc, &n_reg, false);

    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(copy_cmd);
    SDL_ReleaseGPUTransferBuffer(device, temp_transfer);

    SDL_GPUTransferBufferCreateInfo dyn_tb_info = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (uint32_t)(world.count * sizeof(InstanceData)) };
    SDL_GPUTransferBuffer* dyn_transfer_buf = SDL_CreateGPUTransferBuffer(device, &dyn_tb_info);

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

    AppLog app_log;
    int max_hardware_threads = (int)std::thread::hardware_concurrency();
    if (max_hardware_threads < 1) max_hardware_threads = 1;
    int sim_threads = max_hardware_threads;

    app_log.add("JobSystem ativado em pool com %d workers físicos disponíveis.", max_hardware_threads);
    app_log.add("Mundo seed=%llu", (unsigned long long)world.seed);

    glm::vec3 cam_pos(0.0f, 35.0f, -70.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = -0.4f;
    bool rmb_down = false;

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
    int last_ticks_this_frame = 0;

    bool first_sync = true;

    bool arm_stop = false;
    uint64_t stop_at_tick = 6000;
    uint64_t last_calculated_hash = 0;
    uint64_t last_hash_tick = 0;

    // CORREÇÃO: Toggle que salva orçamento massivo da CPU mantendo o Hash apenas quando ativado pelo usuário
    bool audit_hash = false;

    uint64_t last_displayed_second = ~0ULL;
    int64_t last_displayed_scale = -1;

    auto set_scale = [&](int64_t new_scale, const char* origin) {
        if (time_scale != new_scale) {
            time_scale = new_scale;
            if (new_scale != 0) previous_time_scale = new_scale;
            app_log.add("Velocidade alterada para %lldx (%s)", (long long)new_scale, origin);
        }
        };

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
                    if (cam_pitch > 1.5f) cam_pitch = 1.5f;
                    if (cam_pitch < -1.5f) cam_pitch = -1.5f;
                }
            }
            else if (rmb_down) {
                rmb_down = false;
                SDL_SetWindowRelativeMouseMode(window, false);
            }

            if (!ImGui::GetIO().WantCaptureKeyboard) {
                if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (event.key.key == SDLK_ESCAPE) running = false;
                    else if (event.key.key == SDLK_SPACE) {
                        if (time_scale == 0) set_scale(previous_time_scale, "Tecla");
                        else set_scale(0, "Tecla");
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

        if (!ImGui::GetIO().WantCaptureKeyboard) {
            int num_keys;
            const bool* keys = SDL_GetKeyboardState(&num_keys);
            float speed = 35.0f * (float)frame_time;
            if (keys[SDL_SCANCODE_LSHIFT]) speed *= 4.0f;

            glm::vec3 front(sin(cam_yaw) * cos(cam_pitch), sin(cam_pitch), cos(cam_yaw) * cos(cam_pitch));
            glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), front));
            glm::vec3 up(0, 1, 0);

            if (keys[SDL_SCANCODE_W]) cam_pos += front * speed;
            if (keys[SDL_SCANCODE_S]) cam_pos -= front * speed;
            if (keys[SDL_SCANCODE_A]) cam_pos -= right * speed;
            if (keys[SDL_SCANCODE_D]) cam_pos += right * speed;
            if (keys[SDL_SCANCODE_E]) cam_pos += up * speed;
            if (keys[SDL_SCANCODE_Q]) cam_pos -= up * speed;
        }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Tempo");
        if (ImGui::SliderInt("Threads", &sim_threads, 1, max_hardware_threads)) {
            app_log.add("Poder de processamento ajustado para %d threads.", sim_threads);
        }

        if (ImGui::Button("Pausa")) set_scale(0, "Botão"); ImGui::SameLine();
        if (ImGui::Button("1x"))    set_scale(1, "Botão"); ImGui::SameLine();
        if (ImGui::Button("10x"))   set_scale(10, "Botão"); ImGui::SameLine();
        if (ImGui::Button("100x"))  set_scale(100, "Botão"); ImGui::SameLine();
        if (ImGui::Button("1000x")) set_scale(1000, "Botão");

        uint64_t game_seconds = sim_tick / SIM_HZ;
        ImGui::Text("Tick: %llu", (unsigned long long)sim_tick);
        ImGui::Text("Mundo: Dia %llu %02llu:%02llu", (unsigned long long)(game_seconds / 86400), (unsigned long long)((game_seconds / 3600) % 24), (unsigned long long)((game_seconds / 60) % 60));
        ImGui::Text("Instancias: 100000");
        ImGui::Text("Camera Pos: %.2f, %.2f, %.2f", cam_pos.x, cam_pos.y, cam_pos.z);
        ImGui::Text("Frame ms: %.3f", last_frame_ms);
        ImGui::Text("FPS: %.1f", last_frame_ms > 0.0 ? 1000.0 / last_frame_ms : 0.0);

        double effective_speed = frame_time > 0.0 ? (last_ticks_this_frame * DT) / frame_time : 0.0;
        ImGui::Text("Velocidade efetiva: %.1fx", effective_speed);

        ImGui::Text("Sim ms/tick: %.3f", last_sim_ms_per_tick);
        ImGui::Text("Sync ms: %.3f", last_sync_ms);
        ImGui::Text("Hash ms: %.3f", last_hash_ms);
        ImGui::Text("Ticks no frame: %d", last_ticks_this_frame);

        ImGui::Separator();
        ImGui::Text("Determinismo:");

        // CORREÇÃO: Renderiza a checkbox no painel linkada ao toggle
        ImGui::Checkbox("Auditoria de hash", &audit_hash);

        ImGui::Text("Hash @ tick %llu: %016llX", (unsigned long long)last_hash_tick, (unsigned long long)last_calculated_hash);

        ImGui::InputScalar("Alvo", ImGuiDataType_U64, &stop_at_tick);
        if (stop_at_tick > sim_tick) {
            if (ImGui::Button("Rodar até e pausar")) {
                arm_stop = true;
                // CORREÇÃO: Disparar a trava liga implícita e compulsoriamente a auditoria do log
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

        ImGui::Begin("Log");
        for (const std::string& line : app_log.lines) ImGui::TextUnformatted(line.c_str());
        if (app_log.auto_scroll) { ImGui::SetScrollHereY(1.0f); app_log.auto_scroll = false; }
        ImGui::End();

        accumulator += frame_time * time_scale;

        uint64_t sim_start_timer = SDL_GetPerformanceCounter();
        int ticks_this_frame = 0;

        while (accumulator >= DT) {
            sim::step(world, sim_tick, js, sim_threads);
            sim_tick++;
            accumulator -= DT;
            ticks_this_frame++;

            // O hash imutável interno de auditoria de limite continua operando sem restrições do Toggle
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

            uint64_t current_timer = SDL_GetPerformanceCounter();
            double elapsed_sim_ms = (double)(current_timer - sim_start_timer) * 1000.0 / frequency;
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

            // CORREÇÃO: Envelopamento do loop de iteração atrelado ao checkbox no painel "audit_hash"
            if (audit_hash) {
                if (last_hash_tick != sim_tick) {
                    uint64_t hash_start = SDL_GetPerformanceCounter();
                    last_calculated_hash = sim::state_hash(world, sim_tick);
                    last_hash_tick = sim_tick;
                    last_hash_ms = (double)(SDL_GetPerformanceCounter() - hash_start) * 1000.0 / frequency;
                }
            }
            else {
                last_hash_ms = 0.0; // Põe o contador explícito de UI pra dormir se o hash for ignorado
            }
        }

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

            SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(cmd_buffer);
            SDL_GPUTransferBufferLocation loc = { dyn_transfer_buf, 0 };
            SDL_GPUBufferRegion reg = { crowd_instance_buffer, 0, (uint32_t)(world.count * sizeof(InstanceData)) };

            SDL_UploadToGPUBuffer(copy_pass, &loc, &reg, true);
            SDL_EndGPUCopyPass(copy_pass);

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
            glm::vec3 front(sin(cam_yaw) * cos(cam_pitch), sin(cam_pitch), cos(cam_yaw) * cos(cam_pitch));
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
            SDL_DrawGPUIndexedPrimitives(scene_pass, 6, 1, 0, 0, 0);

            SDL_GPUBufferBinding inst_bind_crowd = { crowd_instance_buffer, 0 };
            SDL_BindGPUVertexBuffers(scene_pass, 1, &inst_bind_crowd, 1);
            SDL_DrawGPUIndexedPrimitives(scene_pass, 36, 100000, 6, 0, 0);

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
    SDL_ReleaseGPUBuffer(device, vertex_buffer);
    SDL_ReleaseGPUBuffer(device, index_buffer);
    SDL_ReleaseGPUBuffer(device, neutral_instance_buffer);
    SDL_ReleaseGPUBuffer(device, crowd_instance_buffer);
    SDL_ReleaseGPUTransferBuffer(device, dyn_transfer_buf);
    SDL_ReleaseGPUTexture(device, depth_texture);
    SDL_ReleaseGPUShader(device, vs);
    SDL_ReleaseGPUShader(device, ps);

    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}