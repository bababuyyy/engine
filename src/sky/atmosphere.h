#pragma once
// ============================================================================
// M2.5-25-2/25-3 (S1+S2+S3+S10): LUTs da atmosfera fisica.
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

// M2.5-25-3 (S10): luminancia MEDIA do hemisferio do ceu, para o termo ambiente
// das nuvens (Nubis: ambient_scattering * cor do ceu). Valor UNITLESS - quem
// chama multiplica pela iluminancia do sol e pelo ganho de unidades.
// Integral de 32 direcoes x 20 passos: custo desprezivel, uma vez por frame.
// A altitude do observador e fixada no chao: nosso mundo inteiro cabe em < 1 km
// de uma atmosfera de 100 km, entao a variacao por altura e irrelevante aqui.
void sky_ambient(float sun_cos_zenith, float out_rgb[3]);

// M2.5-25-10: cor FISICA da atmosfera junto ao horizonte, para a nevoa da cena.
// Media de 8 azimutes a ~4 graus de elevacao - onde a nevoa efetivamente vive.
// Substitui a paleta hardcoded do crepusculo, cujos pesos davam 50% de laranja
// de poente com o sol 30 graus ABAIXO do horizonte. Valor UNITLESS, como o
// sky_ambient: quem chama multiplica pela iluminancia do sol e pelo ganho.
// A perspectiva aerea completa (Hillaire 5.4, LUT 32^3) continua na fila; isto
// corrige a COR, nao a integracao por distancia.
void sky_horizon(float sun_cos_zenith, float out_rgb[3]);

} // namespace atmo
