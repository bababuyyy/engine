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
        std::strncpy(sched.categoria, "plebeu", 15);
        sched.categoria[15] = '\0';

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
                else if (word == "categoria") {
                    std::string cat;
                    if (ls >> cat) {
                        std::strncpy(sched.categoria, cat.c_str(), 15);
                        sched.categoria[15] = '\0';
                    }
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

    Relations parse_relations(const char* texto) {
        Relations r;
        if (!texto) return r;
        std::istringstream ss(texto);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            std::string catA, catB; float val;
            if (ls >> catA >> catB >> val) {
                Relations::Entry e;
                std::strncpy(e.catA, catA.c_str(), 15); e.catA[15] = '\0';
                std::strncpy(e.catB, catB.c_str(), 15); e.catB[15] = '\0';
                e.val = val;
                r.entries.push_back(e);
            }
        }
        return r;
    }

    float get_relation(const Relations& rel, const char* catA, const char* catB) {
        for (const auto& e : rel.entries) {
            if ((std::strcmp(e.catA, catA) == 0 && std::strcmp(e.catB, catB) == 0) ||
                (std::strcmp(e.catA, catB) == 0 && std::strcmp(e.catB, catA) == 0)) {
                return e.val;
            }
        }
        return 0.0f;
    }

    Barks parse_barks(const char* texto) {
        Barks b;
        if (!texto) return b;
        std::istringstream ss(texto);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            std::string tier;
            if (ls >> tier) {
                std::string rest;
                std::getline(ls, rest);
                if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);

                if (tier == "amistoso") b.amistoso.push_back(rest);
                else if (tier == "neutro") b.neutro.push_back(rest);
                else if (tier == "frio") b.frio.push_back(rest);
            }
        }
        return b;
    }

    void init_npc(NpcState& npc, const Schedule& sched) {
        npc.action = 0;
        npc.flee_x = 0.0f;
        npc.flee_z = 0.0f;
        npc.entry_atual = 0;
        npc.fome = 1.0f;
        npc.last_minute = -1;
        npc.conv_ticks = 0;
        npc.partner = 255;
        npc.last_conv_abs_min = -100000;

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

        // Máquina de Estado Temporária
        if (npc.action == 3) {
            npc.conv_ticks--;
            if (npc.conv_ticks <= 0) {
                npc.action = 0; // Resume agenda após o papo
            }
        }

        // Avaliação Lógica Vitais por Minuto
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

            // Hierarquia
            if (npc.action == 0 && npc.fome < 0.3f) {
                npc.action = 2;
            }
            if (npc.action == 2 && npc.fome >= 0.95f) {
                npc.action = 0;
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
        else if (npc.action == 3) {
            npc.tx = npc.x;
            npc.tz = npc.z;
        }

        // Motor Cinético
        if (npc.action != 3) {
            float dx = npc.tx - npc.x;
            float dz = npc.tz - npc.z;
            float dist = std::sqrt(dx * dx + dz * dz);
            float step = 2.0f / 60.0f;

            if (dist <= step) {
                npc.x = npc.tx;
                npc.z = npc.tz;
                if (npc.action == 1 && dist < 0.1f) {
                    npc.action = 0;
                }
            }
            else {
                npc.x += (dx / dist) * step;
                npc.z += (dz / dist) * step;
            }
        }
    }

    std::vector<ConvEvent> social_encounters(NpcState* npcs, const Schedule* scheds, int count, const Relations& rel, int64_t abs_min) {
        std::vector<ConvEvent> events;

        for (int i = 0; i < count; ++i) {
            if (npcs[i].action != 0) continue;
            if (abs_min - npcs[i].last_conv_abs_min < 60) continue;

            for (int j = i + 1; j < count; ++j) {
                if (npcs[j].action != 0) continue;
                if (abs_min - npcs[j].last_conv_abs_min < 60) continue;

                float dx = npcs[i].x - npcs[j].x;
                float dz = npcs[i].z - npcs[j].z;
                float dist = std::sqrt(dx * dx + dz * dz);

                if (dist < 1.5f) {
                    npcs[i].action = 3;
                    npcs[i].conv_ticks = 240;
                    npcs[i].partner = (uint8_t)j;
                    npcs[i].last_conv_abs_min = abs_min;

                    npcs[j].action = 3;
                    npcs[j].conv_ticks = 240;
                    npcs[j].partner = (uint8_t)i;
                    npcs[j].last_conv_abs_min = abs_min;

                    float v = get_relation(rel, scheds[i].categoria, scheds[j].categoria);
                    int tier = 0;
                    if (v > 0.3f) tier = 1;
                    else if (v < -0.3f) tier = -1;

                    events.push_back({ i, j, tier });
                    break;
                }
            }
        }
        return events;
    }

    uint64_t hash_npcs(uint64_t base_hash, const NpcState* npcs, int count) {
        uint64_t h = base_hash;
        for (int i = 0; i < count; ++i) {
            uint32_t bx; std::memcpy(&bx, &npcs[i].x, 4);
            uint32_t bz; std::memcpy(&bz, &npcs[i].z, 4);
            uint32_t bf; std::memcpy(&bf, &npcs[i].fome, 4);
            uint32_t bconv; std::memcpy(&bconv, &npcs[i].conv_ticks, 4);
            uint64_t bcm; std::memcpy(&bcm, &npcs[i].last_conv_abs_min, 8);

            h ^= bx + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= bz + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= npcs[i].action + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= bf + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= bconv + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= (uint32_t)(bcm & 0xFFFFFFFF) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= (uint32_t)(bcm >> 32) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
}