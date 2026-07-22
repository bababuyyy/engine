#include "social.h"
#include <sstream>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace social {

    Schedule parse_schedule(const char* texto) {
        Schedule sched;
        sched.ok = false;
        sched.food_x = 0.0f;
        sched.food_z = 0.0f;
        if (!texto) return sched;

        std::istringstream ss(texto);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] == '#') continue;

            std::istringstream ls(line);
            std::string word;
            if (ls >> word) {
                if (word == "comida") {
                    ls >> sched.food_x >> sched.food_z;
                }
                else {
                    int hhmm;
                    std::istringstream ws(word);
                    if (ws >> hhmm) {
                        float x, z;
                        std::string ativ;
                        if (ls >> x >> z >> ativ) {
                            SchedEntry e;
                            e.minuto_do_dia = (hhmm / 100) * 60 + (hhmm % 100);
                            e.x = x;
                            e.z = z;
                            std::strncpy(e.atividade, ativ.c_str(), 15);
                            e.atividade[15] = '\0';
                            sched.entries.push_back(e);
                        }
                    }
                }
            }
        }

        if (!sched.entries.empty()) {
            std::sort(sched.entries.begin(), sched.entries.end(), [](const SchedEntry& a, const SchedEntry& b) {
                return a.minuto_do_dia < b.minuto_do_dia;
                });
            sched.ok = true;
        }

        return sched;
    }

    void init_npc(NpcState& npc, const Schedule& sched) {
        npc.action = 0;
        npc.flee_x = 0.0f;
        npc.flee_z = 0.0f;
        npc.entry_atual = 0;
        npc.fome = 1.0f;
        npc.last_minute = -1;

        if (sched.ok && !sched.entries.empty()) {
            int best_idx = -1;
            for (size_t i = 0; i < sched.entries.size(); ++i) {
                if (sched.entries[i].minuto_do_dia <= 0) {
                    best_idx = (int)i;
                }
            }
            if (best_idx == -1) best_idx = (int)sched.entries.size() - 1;

            npc.x = sched.entries[best_idx].x;
            npc.z = sched.entries[best_idx].z;
            npc.tx = npc.x;
            npc.tz = npc.z;
            npc.entry_atual = best_idx;
        }
        else {
            npc.x = npc.z = npc.tx = npc.tz = 0.0f;
        }
    }

    void social_tick(NpcState& npc, const Schedule& sched, int minuto_do_dia) {
        if (!sched.ok || sched.entries.empty()) return;

        // Avaliação Lógica por Minuto (segurança arquitetural)
        if (minuto_do_dia != npc.last_minute) {
            npc.last_minute = minuto_do_dia;

            npc.fome -= 1.6f / 1440.0f;
            if (npc.fome < 0.0f) npc.fome = 0.0f;

            float fdx = sched.food_x - npc.x;
            float fdz = sched.food_z - npc.z;
            float dist_food = std::sqrt(fdx * fdx + fdz * fdz);

            if (dist_food < 0.8f && npc.fome < 0.95f) {
                npc.fome += 0.07f;
                if (npc.fome > 1.0f) npc.fome = 1.0f;
            }

            // Hierarquia: FUGA blinda COMER, COMER blinda AGENDA
            if (npc.action == 0 && npc.fome < 0.3f) {
                npc.action = 2; // Aciona interrupção da fome
            }
            if (npc.action == 2 && npc.fome >= 0.95f) {
                npc.action = 0; // Saciedade -> Retoma a agenda autônoma
            }
        }

        // Determinação do alvo vital
        if (npc.action == 0) {
            int best_idx = -1;
            for (size_t i = 0; i < sched.entries.size(); ++i) {
                if (sched.entries[i].minuto_do_dia <= minuto_do_dia) {
                    best_idx = (int)i;
                }
            }
            if (best_idx == -1) best_idx = (int)sched.entries.size() - 1;

            npc.entry_atual = best_idx;
            npc.tx = sched.entries[best_idx].x;
            npc.tz = sched.entries[best_idx].z;
        }
        else if (npc.action == 1) {
            npc.tx = npc.flee_x;
            npc.tz = npc.flee_z;
        }
        else if (npc.action == 2) {
            npc.tx = sched.food_x;
            npc.tz = sched.food_z;
        }

        // Motor Cinético (2.0 u/s estrito)
        float dx = npc.tx - npc.x;
        float dz = npc.tz - npc.z;
        float dist = std::sqrt(dx * dx + dz * dz);
        float step = 2.0f / 60.0f;

        if (dist <= step) {
            npc.x = npc.tx;
            npc.z = npc.tz;
            if (npc.action == 1 && dist < 0.1f) {
                // Resume miniatura: cumpriu o primitivo, liberta a action para a máquina vital reavaliar
                npc.action = 0;
            }
        }
        else {
            npc.x += (dx / dist) * step;
            npc.z += (dz / dist) * step;
        }
    }

    uint64_t hash_npcs(uint64_t base_hash, const NpcState* npcs, int count) {
        uint64_t h = base_hash;
        for (int i = 0; i < count; ++i) {
            uint32_t bx; std::memcpy(&bx, &npcs[i].x, 4);
            uint32_t bz; std::memcpy(&bz, &npcs[i].z, 4);
            uint32_t bf; std::memcpy(&bf, &npcs[i].fome, 4);

            h ^= bx + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= bz + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= npcs[i].action + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= bf + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
}