#include "level.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace level {
    LevelData load_level(const char* filepath) {
        LevelData data;
        data.ok = false;
        data.spawn = { 0.0f, 2.0f, 0.0f };

        std::ifstream file(filepath);
        if (!file.is_open()) {
            return data;
        }

        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            line_num++;
            if (line.empty() || line[0] == '#') continue;

            std::istringstream ss(line);
            std::string type;
            ss >> type;

            if (type == "box") {
                BoxDef b;
                if (ss >> b.cx >> b.cy >> b.cz >> b.hx >> b.hy >> b.hz >> b.r >> b.g >> b.b) {
                    data.boxes.push_back(b);
                }
                else {
                    std::cerr << "Linha " << line_num << ": Formato de box invalido\n";
                }
            }
            else if (type == "spawn") {
                if (!(ss >> data.spawn.x >> data.spawn.y >> data.spawn.z)) {
                    std::cerr << "Linha " << line_num << ": Formato de spawn invalido\n";
                }
            }
            else if (type == "mesh") {
                LevelMesh m;
                if (ss >> m.path >> m.x >> m.y >> m.z) {
                    data.meshes.push_back(m);
                }
                else {
                    std::cerr << "Linha " << line_num << ": Formato de mesh invalido\n";
                }
            }
            else {
                std::cerr << "Linha " << line_num << ": Tipo desconhecido '" << type << "'\n";
            }
        }

        data.ok = true;
        return data;
    }
}