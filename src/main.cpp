#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdarg>

// Estrutura simples para o Log sem criar classes ou wrappers complexos.
// Armazena até 256 linhas e controla o autoscroll da UI.
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
        if (lines.size() > 256) {
            lines.erase(lines.begin());
        }
        auto_scroll = true;
    }
};

void simulation_step() {
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Erro no SDL_Init: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Minha Engine", 1280, 720, 0);
    if (!window) {
        SDL_Log("Erro no SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GPUShaderFormat format_flags = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    SDL_GPUDevice* device = SDL_CreateGPUDevice(format_flags, true, nullptr);
    if (!device) {
        SDL_Log("Erro no SDL_CreateGPUDevice: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        SDL_Log("Erro no SDL_ClaimWindowForGPUDevice: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // --- SETUP DO DEAR IMGUI ---
    // Inicializa o contexto global do ImGui logo após a janela estar atrelada à GPU.
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForSDLGPU(window);

    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init_info);

    AppLog app_log;
    app_log.add("Sistemas inicializados com sucesso.");

    constexpr uint64_t SIM_HZ = 60;
    constexpr double DT = 1.0 / 60.0;

    uint64_t sim_tick = 0;
    double accumulator = 0.0;

    int64_t time_scale = 1;
    int64_t previous_time_scale = 1;

    uint64_t last_displayed_second = ~0ULL;
    int64_t last_displayed_scale = -1;

    uint64_t current_time = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();

    // Variáveis para registrar o tempo de simulação do frame anterior para exibir na UI
    double last_frame_ms = 0.0;
    double last_sim_ms = 0.0;
    int last_ticks_this_frame = 0;

    // Função lambda para DRY ao alterar a escala de tempo, registrando a origem no log.
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
            // Entrega o evento ao ImGui para tratar cliques, digitação e foco.
            ImGui_ImplSDL3_ProcessEvent(&event);

            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }

            // Os atalhos nativos da engine só reagem se a UI não estiver capturando teclado.
            // Impede que digitar "1" no console dispare a mudança de timescale do jogo.
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

        // --- IMGUI NEW FRAME ---
        // Prepara as ferramentas de UI antes da simulação, alimentando-a com dados do frame passado.
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Painel: Tempo
        ImGui::Begin("Tempo");
        if (ImGui::Button("Pausa")) set_scale(0, "Botão"); ImGui::SameLine();
        if (ImGui::Button("1x"))    set_scale(1, "Botão"); ImGui::SameLine();
        if (ImGui::Button("10x"))   set_scale(10, "Botão"); ImGui::SameLine();
        if (ImGui::Button("100x"))  set_scale(100, "Botão"); ImGui::SameLine();
        if (ImGui::Button("1000x")) set_scale(1000, "Botão");

        uint64_t game_seconds = sim_tick / SIM_HZ;
        uint64_t days = game_seconds / 86400;
        uint64_t hours = (game_seconds / 3600) % 24;
        uint64_t minutes = (game_seconds / 60) % 60;

        ImGui::Text("Tick: %llu", (unsigned long long)sim_tick);
        ImGui::Text("Mundo: Dia %llu %02llu:%02llu", (unsigned long long)days, (unsigned long long)hours, (unsigned long long)minutes);
        ImGui::Text("Frame ms: %.3f", last_frame_ms);
        ImGui::Text("Sim ms: %.3f", last_sim_ms);
        ImGui::Text("Ticks no frame: %d", last_ticks_this_frame);
        ImGui::End();

        // Painel: Log
        ImGui::Begin("Log");
        for (const std::string& line : app_log.lines) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (app_log.auto_scroll) {
            ImGui::SetScrollHereY(1.0f);
            app_log.auto_scroll = false;
        }
        ImGui::End();

        // --- SIMULAÇÃO ---
        accumulator += frame_time * time_scale;

        // Mede unicamente o tempo real gasto processando a lógica interna do acumulador neste frame.
        uint64_t sim_start_timer = SDL_GetPerformanceCounter();
        int ticks_this_frame = 0;

        while (accumulator >= DT) {
            simulation_step();
            sim_tick++;
            accumulator -= DT;
            ticks_this_frame++;

            if (ticks_this_frame >= 10000) {
                accumulator = 0.0;
                break;
            }
        }

        uint64_t sim_end_timer = SDL_GetPerformanceCounter();
        last_sim_ms = (double)(sim_end_timer - sim_start_timer) * 1000.0 / frequency;
        last_ticks_this_frame = ticks_this_frame;

        // Recalcula o relógio APÓS a simulação, para o título exibir tick e hora do mesmo instante.
        game_seconds = sim_tick / SIM_HZ;
        days = game_seconds / 86400;
        hours = (game_seconds / 3600) % 24;
        minutes = (game_seconds / 60) % 60;

        // Atualização do título da janela conforme especificação existente
        if (game_seconds != last_displayed_second || time_scale != last_displayed_scale) {
            last_displayed_second = game_seconds;
            last_displayed_scale = time_scale;
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

        // --- RENDERIZAÇÃO ---
        // Finaliza a compilação lógica da UI antes de tocar na GPU.
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();

        SDL_GPUCommandBuffer* cmd_buffer = SDL_AcquireGPUCommandBuffer(device);
        if (!cmd_buffer) continue;

        SDL_GPUTexture* swapchain_texture = nullptr;
        SDL_WaitAndAcquireGPUSwapchainTexture(cmd_buffer, window, &swapchain_texture, nullptr, nullptr);

        // Se estiver minimizada, a swapchain retorna nula. Pulamos o setup e render, mas submetemos o cmd_buffer para não travar.
        if (swapchain_texture) {
            // O PrepareDrawData cria e envia os buffers de vértices/índices do frame para a VRAM.
            // Precisa rodar ANTES do pass pois transferências de memória não podem ocorrer durante um render pass ativo.
            ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd_buffer);

            SDL_GPUColorTargetInfo color_target_info = {};
            color_target_info.texture = swapchain_texture;
            color_target_info.clear_color = { 0.05f, 0.07f, 0.12f, 1.0f };
            color_target_info.load_op = SDL_GPU_LOADOP_CLEAR;
            color_target_info.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(cmd_buffer, &color_target_info, 1, nullptr);

            // O RenderDrawData emite os "draw calls" que de fato pintam a UI na textura do swapchain.
            // Precisa rodar DENTRO do pass pois são comandos de desenho que afetam o color target atrelado.
            ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd_buffer, render_pass);

            SDL_EndGPURenderPass(render_pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd_buffer);
    }
    // Espera a GPU terminar todo trabalho em voo antes de destruir os recursos do ImGui.
    SDL_WaitForGPUIdle(device);

    // --- SHUTDOWN ---
    // Encerra os contextos do ImGui e seus recursos na VRAM antes de deletar o render device base.
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}