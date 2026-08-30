// ============================================================================
//  atmo_common.hlsli - M2.5-26-8: A ATMOSFERA, UMA VEZ SO.
// ----------------------------------------------------------------------------
//  Doutrina da casa 24F-1, verbatim: "avaliador UNICO ... variantes divergentes
//  ficam impossiveis por construcao". Ate a 26-7 o ceu tinha a marcha completa e
//  a cena tinha uma constante fazendo as vezes dela - duas aproximacoes da mesma
//  fisica, com uma costura dura no horizonte. Este arquivo acaba com isso: ceu,
//  terreno e grama leem O MESMO codigo.
//
//  USO - o shader escolhe os registradores das LUTs ANTES de incluir:
//      #define ATMO_T_TRANS t3
//      #define ATMO_T_MS    t4
//      #define ATMO_S_LUT   s2
//      #include "atmo_common.hlsli"
//
//  Fonte: Sebastien Hillaire, "A Scalable and Production-Ready Sky and
//  Atmosphere Rendering Technique", EGSR 2020. Tabela 1 (coeficientes), sec. 3
//  (equacao de espalhamento), 5.2 (Transmittance LUT), 5.4 (perspectiva aerea),
//  5.5 (Psi_ms). Integral conservadora de energia: Hillaire 2015, slide 28.
// ============================================================================
#ifndef ATMO_COMMON_HLSLI
#define ATMO_COMMON_HLSLI

Texture2D trans_lut : register(ATMO_T_TRANS, space2); // M2.5-25-2: Transmittance LUT 256x64 (Hillaire Â§5.2)
Texture2D ms_lut : register(ATMO_T_MS, space2);    // M2.5-25-2: Multiple-scattering LUT 32x32 (Â§5.5)
SamplerState lut_samp : register(ATMO_S_LUT, space2);

// ============================================================================
// M2.5-25-2: A ATMOSFERA FISICA (Hillaire, EGSR 2020)
// Tabela 1 (coeficientes) + Â§3 (equacao de espalhamento) + Â§5.2 (Transmittance
// LUT) + Â§5.5 (Psi_ms). Integral conservadora de energia do Hillaire 2015 sl.28.
// Unidades internas: km. FISICA COM FONTE = CODIGO BLINDADO.
// ============================================================================
static const float3 kRayS = float3(5.802e-3f, 13.558e-3f, 33.1e-3f);
static const float  kMieS = 3.996e-3f;
static const float  kMieA = 4.40e-3f;
static const float3 kOzoA = float3(0.650e-3f, 1.881e-3f, 0.085e-3f);
static const float  kRg = 6360.0f;
static const float  kRt = 6460.0f;

float3 atmo_sigma_t(float h)
{
    float dr = exp(-h / 8.0f);
    float dm = exp(-h / 1.2f);
    float dz = max(0.0f, 1.0f - abs(h - 25.0f) / 15.0f);
    return kRayS * dr + (kMieS + kMieA) * dm + kOzoA * dz;
}

// MESMA parametrizacao do assador (src/sky/atmosphere.cpp) - contrato de casa
float2 atmo_trans_uv(float h, float mu)
{
    float s = (mu < 0.0f ? -1.0f : 1.0f) * sqrt(abs(mu));
    return float2(saturate(0.5f + 0.5f * s), saturate(h / (kRt - kRg)));
}

float atmo_ray_sphere(float3 p, float3 d, float r)
{
    float b = dot(p, d);
    float c = dot(p, p) - r * r;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    disc = sqrt(disc);
    float t0 = -b - disc;
    float t1 = -b + disc;
    if (t1 < 0.0f) return -1.0f;
    return (t0 < 0.0f) ? t1 : t0;
}

// M2.5-25-15: contribuicao de UMA luz direcional num ponto da marcha.
// Extraida para funcao porque a Eq. 11 do Hillaire soma sobre Nlight luzes e a
// forma e identica para todas - so mudam a direcao, as fases e a iluminancia.
// A Transmittance LUT e a MS LUT sao parametrizadas pelo cosseno de zenite DA LUZ,
// entao servem para qualquer direcional (sol, lua, o que vier).
float3 atmo_light(float3 up_local, float h, float3 ss_r, float3 ss_m,
                  float3 L_dir, float pr, float pm)
{
    float mu = dot(up_local, L_dir);
    float3 Tl = trans_lut.SampleLevel(lut_samp, atmo_trans_uv(h, mu), 0).rgb;
    float3 psi = ms_lut.SampleLevel(lut_samp, float2(saturate(0.5f + 0.5f * mu), saturate(h / (kRt - kRg))), 0).rgb;
    return (ss_r * pr + ss_m * pm) * Tl + (ss_r + ss_m) * psi;
}

// Fases de Rayleigh e de Mie para um cosseno de angulo de espalhamento (Hillaire Â§4).
void atmo_phases(float cos_t, out float pr, out float pm)
{
    pr = 3.0f * (1.0f + cos_t * cos_t) / (16.0f * 3.14159265f);
    const float g = 0.8f; // Cornette-Shanks, padrao do paper
    pm = (3.0f / (8.0f * 3.14159265f)) * ((1.0f - g * g) * (1.0f + cos_t * cos_t)) /
         ((2.0f + g * g) * pow(max(1.0f + g * g - 2.0f * g * cos_t, 1e-4f), 1.5f));
}

// ============================================================================
// M2.5-26-8: PERSPECTIVA AEREA DA CENA.
// Hillaire (EGSR 2020) Eq. 1:  L(c,v) = T(c,p)*Lo(p,v) + integral de in-scatter.
// A secao 5.4 resolve por froxel numa LUT 32^3; aqui a MESMA integral e avaliada
// por pixel ao longo do raio camera->fragmento, com poucos passos: nos poucos km
// da cena o meio e quase homogeneo. A LUT vira otimizacao depois - a fisica e a
// mesma, e agora ela e a MESMA do ceu, do arquivo, por construcao (doutrina 24F-1).
//
// POR QUE ISTO EXISTE (medido, 26-8): a cena usava  lerp(lit, fog_color, fog_f)
// com fog_color = UMA constante (media de 8 azimutes a 4 graus de elevacao) e
// fog_density = 0,006 por METRO. O sigma_t real do verde ao nivel do mar e
// 1,36e-5 por metro (Tabela 1): a nevoa era ~440x mais grossa que o ar. Resultado
// medido: todo terreno alem de ~500 unidades virava uma LAJE CHAPADA da cor do
// horizonte - 1.023 cd/m2 ao nascer do sol, exatamente o 'max' de 1.022 que o
// medidor devolveu. Contra um ceu que ali TEM gradiente, isso e uma costura dura.
// Duas aproximacoes da mesma fisica se encontrando. Agora e uma so.
//
// APROXIMACOES DECLARADAS: (1) o 'up' local e (0,1,0) - a curvatura do planeta e
// desprezivel nos poucos km da cena; (2) sem froxel/LUT, entao sem reuso entre
// pixels; (3) sem sombra volumetrica ao longo do raio (isso e a 24H, separada).
// 'str' multiplica o sigma_t: 1.0 = ar fisico, acima = bruma por direcao de arte.
// Como a integral de in-scatter e (S - S*e)/sigma_t, ela se auto-normaliza - mais
// extincao satura na cor do ceu em vez de inventar energia.
// ============================================================================
void atmo_aerial(float3 cam_w, float3 frag_w,
                 float3 to_sun, float3 sun_illum,
                 float3 to_moon, float3 moon_illum,
                 float steps, float str,
                 out float3 T, out float3 S)
{
    T = float3(1.0f, 1.0f, 1.0f);
    S = float3(0.0f, 0.0f, 0.0f);

    float3 d_w = frag_w - cam_w;
    float dist_m = length(d_w);
    if (dist_m <= 1e-4f) return;
    float3 dir = d_w / dist_m;
    float dist_km = dist_m * 0.001f;   // mundo: 1 unidade ~ 1 metro

    float pr_s, pm_s, pr_m, pm_m;
    atmo_phases(dot(dir, to_sun),  pr_s, pm_s);
    atmo_phases(dot(dir, to_moon), pr_m, pm_m);

    int N = (int)clamp(steps, 1.0f, 16.0f);
    float dt = dist_km / (float)N;
    float h0 = max(cam_w.y, 0.0f) * 0.001f;
    const float3 up_local = float3(0.0f, 1.0f, 0.0f);

    [loop]
    for (int i = 0; i < N; ++i)
    {
        float h = max(h0 + dir.y * ((i + 0.5f) * dt), 0.0f);
        float3 ss_r = kRayS * exp(-h / 8.0f);
        float3 ss_m = kMieS * exp(-h / 1.2f);
        float3 st = atmo_sigma_t(h) * max(str, 1e-4f);

        float3 Sc = atmo_light(up_local, h, ss_r, ss_m, to_sun,  pr_s, pm_s) * sun_illum
                  + atmo_light(up_local, h, ss_r, ss_m, to_moon, pr_m, pm_m) * moon_illum;

        float3 e = exp(-st * dt);
        float3 Sint = (Sc - Sc * e) / max(st, 1e-7f); // Hillaire 2015 sl.28
        S += T * Sint;
        T *= e;
    }
}

#endif // ATMO_COMMON_HLSLI
