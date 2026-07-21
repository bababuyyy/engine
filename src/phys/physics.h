#pragma once

// Header agnóstico: PROIBIDA a inclusão de SDL, ImGui, GLM, sim.h ou até mesmo headers sujos do Jolt.
// A interface pública expõe apenas plain-old-data (POD) seguros.

namespace phys {

    struct PhysInput {
        float move_x;
        float move_z;
        bool jump;
    };

    struct PhysState {
        float x;
        float y;
        float z;
        bool on_ground;
    };

    void phys_init();
    void phys_shutdown();

    // Cria colisores estáticos genéricos na cena
    void phys_create_static_box(float px, float py, float pz, float hx, float hy, float hz);

    // Injeta a cápsula do CharacterVirtual no ecossistema físico
    void phys_create_character(float px, float py, float pz);

    // Chamada isolada por tick: Consome input, injeta gravidade, processa a atualização global e retorna estado atual
    PhysState phys_update_character(const PhysInput& input);
}