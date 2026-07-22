#pragma once
#include <cstdint>

// Header agnóstico: PROIBIDA a inclusão de SDL, ImGui, GLM, sim.h ou headers sujos do Jolt.
// A interface pública expõe apenas plain-old-data (POD) seguros.

namespace phys {

    struct PhysInput {
        float vx;
        float vy;
        float vz;
        float gravity_y; // Usado estritamente como vetor direcional do mundo para o ExtendedUpdate
        float stick_step_down = 0.5f;
        bool walk_stairs_on = true;
    };

    struct PhysState {
        float x;
        float y;
        float z;
        float vx;
        float vy;
        float vz;
        float gvx; // Componentes da velocidade relativa do chão (plataformas móveis)
        float gvy;
        float gvz;
        bool on_ground;
    };

    void phys_init();
    void phys_shutdown();

    // Cria colisores estáticos e retorna a ID Jolt embutida num descritor genérico (necessário pro F5 Reload)
    uint32_t phys_create_static_box(float px, float py, float pz, float hx, float hy, float hz, float rot_yaw = 0.0f, float rot_pitch = 0.0f);
    void phys_destroy_body(uint32_t id);

    // Cria um colisor movimentável pela API sem assimilação de forças, retornando a ID atrelada
    uint32_t phys_create_kinematic_box(float px, float py, float pz, float hx, float hy, float hz);
    void phys_move_kinematic(uint32_t id, float x, float y, float z);

    // Injeta a cápsula do CharacterVirtual no ecossistema físico
    void phys_create_character(float px, float py, float pz);

    // Controle e teleporte atrelado às chaves de dev-tool
    void phys_teleport_character(float px, float py, float pz);
    bool phys_resize_character(bool agachado);

    // Atualiza caches espaciais do Kernel para curar dead-stops logo após os teleportes/reloads
    void phys_refresh_contacts();

    // Cast simples do NarrowPhaseQuery para testar visão da câmera de forma determinística e segura
    float phys_camera_ray(float fx, float fy, float fz, float dx, float dy, float dz, float max_dist);

    // Chamada isolada por tick: Consome input, processa a atualização global e retorna estado atual
    PhysState phys_update_character(const PhysInput& input);
}