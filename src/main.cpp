#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cstdint>
#include <cstdio>

// Função reservada para a lógica de simulação do jogo.
// Entrarão aqui os sistemas nas próximas etapas; roda sempre a passos fixos.
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

    // --- SETUP DO RELÓGIO E TIMESTEP ---
    // A simulação rodará estritamente a 60 Hz independentemente do framerate visual.
    constexpr uint64_t SIM_HZ = 60;
    constexpr double DT = 1.0 / 60.0;

    // O tempo canônico é inteiro para evitar perda de precisão flutuante em jogos longos e garantir determinismo.
    // O acumulador é double apenas para medir rebarbas de tempo fracionário entre frames de render.
    uint64_t sim_tick = 0;
    double accumulator = 0.0;

    // Define a escala de tempo atual e a última usada (para poder restaurar a velocidade ao sair do pause).
    int64_t time_scale = 1;
    int64_t previous_time_scale = 1;

    // Variáveis de controle para saber quando atualizar o título da janela (evitando chamadas lentas todo frame).
    uint64_t last_displayed_second = ~0ULL;
    int64_t last_displayed_scale = -1;

    // Captura o estado inicial dos contadores de alta performance do sistema.
    uint64_t current_time = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE) {
                    running = false;
                }
                // Alterna entre estado pausado (0) e a escala anterior de velocidade.
                else if (event.key.key == SDLK_SPACE) {
                    if (time_scale == 0) {
                        time_scale = previous_time_scale;
                    }
                    else {
                        previous_time_scale = time_scale;
                        time_scale = 0;
                    }
                } 
                // Aplica a escala de tempo imediatamente e a salva como escala ativa para o toggle do Pause.
                else if (event.key.key == SDLK_1) { time_scale = 1; previous_time_scale = 1; }
                else if (event.key.key == SDLK_2) { time_scale = 10; previous_time_scale = 10; }
                else if (event.key.key == SDLK_3) { time_scale = 100; previous_time_scale = 100; }
                else if (event.key.key == SDLK_4) { time_scale = 1000; previous_time_scale = 1000; }
            }
        }

        // --- ATUALIZAÇÃO DO TIMESTEP ---
        uint64_t new_time = SDL_GetPerformanceCounter();
        double frame_time = (double)(new_time - current_time) / frequency;
        current_time = new_time;

        // Limita o gap de tempo máximo para evitar "espirais da morte" se a janela for arrastada.
        if (frame_time > 0.25) {
            frame_time = 0.25;
        }

        // Incrementa o acumulador multiplicando pela escala (não mexe no tamanho interno do passo DT).
        accumulator += frame_time * time_scale;

        int ticks_this_frame = 0;
        // Consome tempo em fatias perfeitamente iguais de DT para avançar a simulação.
        while (accumulator >= DT) {
            simulation_step();
            sim_tick++;
            accumulator -= DT;
            ticks_this_frame++;

            // Proteção contra engasgos severos em escalas de 1000x se a CPU não der conta de limpar o buffer.
            if (ticks_this_frame >= 10000) {
                accumulator = 0.0;
                break;
            }
        }

        // Deriva o tempo de mundo através de divisões inteiras (seguros e imutáveis por float rot).
        uint64_t game_seconds = sim_tick / SIM_HZ;

        // Atualiza a barra da janela apenas se 1 segundo in-game passar ou a escala mudar.
        if (game_seconds != last_displayed_second || time_scale != last_displayed_scale) {
            last_displayed_second = game_seconds;
            last_displayed_scale = time_scale;

            uint64_t days = game_seconds / 86400;
            uint64_t hours = (game_seconds / 3600) % 24;
            uint64_t minutes = (game_seconds / 60) % 60;

            char title_buffer[256];
            // Formatamos forçando o cast para (unsigned long long) garantindo compatibilidade multiplataforma do %llu.
            if (time_scale == 0) {
                snprintf(title_buffer, sizeof(title_buffer),
                    "Minha Engine | dia %llu %02llu:%02llu | tick %llu | pausado",
                    (unsigned long long)days, (unsigned long long)hours, (unsigned long long)minutes, (unsigned long long)sim_tick);
            }
            else {
                snprintf(title_buffer, sizeof(title_buffer),
                    "Minha Engine | dia %llu %02llu:%02llu | tick %llu | %lldx",
                    (unsigned long long)days, (unsigned long long)hours, (unsigned long long)minutes, (unsigned long long)sim_tick, (long long)time_scale);
            }
            SDL_SetWindowTitle(window, title_buffer);
        }

        // --- RENDERIZAÇÃO DA GPU ---
        SDL_GPUCommandBuffer* cmd_buffer = SDL_AcquireGPUCommandBuffer(device);
        if (!cmd_buffer) {
            SDL_Log("Erro ao adquirir command buffer: %s", SDL_GetError());
            continue;
        }

        SDL_GPUTexture* swapchain_texture = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_buffer, window, &swapchain_texture, nullptr, nullptr)) {
            SDL_Log("Erro ao adquirir textura da swapchain: %s", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(cmd_buffer);
            continue;
        }

        if (swapchain_texture) {
            SDL_GPUColorTargetInfo color_target_info = {};
            color_target_info.texture = swapchain_texture;
            color_target_info.clear_color = { 0.05f, 0.07f, 0.12f, 1.0f };
            color_target_info.load_op = SDL_GPU_LOADOP_CLEAR;
            color_target_info.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(cmd_buffer, &color_target_info, 1, nullptr);
            SDL_EndGPURenderPass(render_pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd_buffer);
    }

    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}