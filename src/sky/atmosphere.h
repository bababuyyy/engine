#pragma once
// ============================================================================
// M2.5-25-2 (S1+S2+S3): LUTs da atmosfera fisica.
// Fonte: Sebastien Hillaire, "A Scalable and Production-Ready Sky and
// Atmosphere Rendering Technique", EGSR 2020 (Epic Games) - o Sky Atmosphere
// da Unreal. PDF nos arquivos do projeto (pt1/pt2), lido na integra.
//   - Tabela 1: coeficientes de Rayleigh / Mie / Ozonio
//   - Secao 5.2: Transmittance LUT 256x64, 40 passos
//   - Secao 5.5: Multiple-scattering LUT 32x32, 20 passos, 64 direcoes,
//                Psi_ms = L2ndorder * 1/(1 - fms)  (Eq. 5-10)
// DOUTRINA DA CASA: fisica com fonte = codigo blindado. Nenhum numero deste
// modulo vira slider. O unico parametro de retina (ganho de unidades) mora no
// painel, porque ele NAO esta na fonte - depende da nossa unidade arbitraria.
// ============================================================================

#include <cstdint>
#include <vector>

namespace atmo {

// Resolucoes da Tabela 2 do paper (perfil PC).
constexpr int kTransW = 256;
constexpr int kTransH = 64;
constexpr int kMsW = 32;
constexpr int kMsH = 32;

// Ambas retornam RGBA16F (4 half-floats por texel), prontas para upload.
// Cache em disco (cache/atmo_luts_v1.bin); assam na primeira execucao.
std::vector<uint8_t> generate_transmittance_lut();
std::vector<uint8_t> generate_multiscatter_lut();

} // namespace atmo
