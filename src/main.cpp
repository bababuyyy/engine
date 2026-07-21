#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int main(int argc, char* argv[]) {
    // Evita warnings do compilador (/W4) sobre parâmetros não utilizados,
    // mantendo a assinatura clássica do int main.
    (void)argc;
    (void)argv;

    // Inicializa o subsistema de vídeo do SDL3.
    // É a base obrigatória para criar janelas e interagir com o ambiente gráfico do SO.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Erro no SDL_Init: %s", SDL_GetError());
        return 1;
    }

    // Cria a janela de 1280x720 sem flags gráficas específicas (como OPENGL ou VULKAN).
    // O gerenciamento da superfície é delegado à API de GPU do SDL posteriormente.
    SDL_Window* window = SDL_CreateWindow("Minha Engine", 1280, 720, 0);
    if (!window) {
        SDL_Log("Erro no SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Cria o dispositivo lógico da GPU habilitando os principais formatos de shader e o modo debug.
    // O uso de `nullptr` no final delega a escolha do melhor backend gráfico disponível (DX12/Vulkan/Metal) ao SDL.
    SDL_GPUShaderFormat format_flags = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    SDL_GPUDevice* device = SDL_CreateGPUDevice(format_flags, true, nullptr);
    if (!device) {
        SDL_Log("Erro no SDL_CreateGPUDevice: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Reivindica o controle da janela para o nosso device de GPU.
    // Isso vincula internamente uma Swapchain à janela para podermos apresentar os frames.
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        SDL_Log("Erro no SDL_ClaimWindowForGPUDevice: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    bool running = true;
    while (running) {
        SDL_Event event;
        // Processa a fila de mensagens do sistema a cada iteração do frame.
        // Mantém a janela viva no SO e permite detectar comandos de fechamento.
        while (SDL_PollEvent(&event)) {
            // Interrompe o loop principal se o usuário fechar a janela pelo botão (X).
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            // Interrompe o loop se a tecla ESC for pressionada, conforme o requisito do teste.
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }

        // Adquire um buffer de comando para enfileirar as instruções do frame atual.
        // Toda operação de renderização na GPU precisa passar por esse buffer antes da execução.
        SDL_GPUCommandBuffer* cmd_buffer = SDL_AcquireGPUCommandBuffer(device);
        if (!cmd_buffer) {
            SDL_Log("Erro ao adquirir command buffer: %s", SDL_GetError());
            continue;
        }

        SDL_GPUTexture* swapchain_texture = nullptr;
        // Pede uma textura da swapchain associada à janela para funcionar como nosso quadro de desenho.
        // O método já lida com o Vsync padrão e bloqueia aguardando a disponibilidade da tela.
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_buffer, window, &swapchain_texture, nullptr, nullptr)) {
            SDL_Log("Erro ao adquirir textura da swapchain: %s", SDL_GetError());
            // Libera o buffer vazio caso haja uma falha momentânea de redimensionamento/perda de contexto.
            SDL_SubmitGPUCommandBuffer(cmd_buffer);
            continue;
        }

        if (swapchain_texture) {
            // Configura os metadados do alvo de renderização.
            // Instruímos a GPU a limpar o buffer antigo e preencher com o nosso azul-escuro específico.
            SDL_GPUColorTargetInfo color_target_info = {};
            color_target_info.texture = swapchain_texture;
            color_target_info.clear_color = { 0.05f, 0.07f, 0.12f, 1.0f };
            color_target_info.load_op = SDL_GPU_LOADOP_CLEAR;
            color_target_info.store_op = SDL_GPU_STOREOP_STORE;

            // Abre e fecha imediatamente um passe de renderização na GPU.
            // Como passamos LOADOP_CLEAR, a própria abertura do passe já executa a limpeza da tela.
            SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(cmd_buffer, &color_target_info, 1, nullptr);
            SDL_EndGPURenderPass(render_pass);
        }

        // Envia as instruções gravadas neste frame para processamento na placa de vídeo.
        // Ao concluir as instruções do buffer, o SDL apresenta magicamente a textura na janela.
        SDL_SubmitGPUCommandBuffer(cmd_buffer);
    }

    // Libera a janela do domínio da GPU de forma explícita.
    // É o primeiro passo do desmonte e garante que a Swapchain seja desmontada sem travamentos.
    SDL_ReleaseWindowFromGPUDevice(device, window);

    // Destrói o contexto geral da GPU, apagando buffers de memória internos remanescentes.
    SDL_DestroyGPUDevice(device);

    // Fecha e limpa os recursos nativos de janela alocados pelo Sistema Operacional.
    SDL_DestroyWindow(window);

    // Encerra globalmente o SDL, resetando o subsistema de vídeo à estaca zero.
    SDL_Quit();

    return 0;
}