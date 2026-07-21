#include "level.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>

namespace level {
    LevelData load_level(const char* path) {
        LevelData data;
        data.ok = false;
        data.spawn = { 0.0f, 0.0f, 0.0f };

        std::ifstream file(path);
        if (!file.is_open()) return data;

        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            line_num++;
            // Ignora linhas vazias e comentários
            if (line.empty() || line[0] == '#') continue;

            std::istringstream iss(line);
            std::string type;
            if (!(iss >> type)) continue;

            if (type == "box") {
                LevelBox b;
                if (iss >> b.cx >> b.cy >> b.cz >> b.hx >> b.hy >> b.hz >> b.r >> b.g >> b.b) {
                    data.boxes.push_back(b);
                }
                else {
                    printf("Erro parsing 'box': sintaxe invalida na linha %d\n", line_num);
                }
            }
            else if (type == "spawn") {
                if (!(iss >> data.spawn.x >> data.spawn.y >> data.spawn.z)) {
                    printf("Erro parsing 'spawn': sintaxe invalida na linha %d\n", line_num);
                }
            }
            else {
                printf("Comando level ignorado: tipo '%s' desconhecido na linha %d\n", type.c_str(), line_num);
            }
        }

        data.ok = true;
        return data;
    }
}