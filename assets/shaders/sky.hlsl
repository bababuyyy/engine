struct VSInput
{
    float3 pos : TEXCOORD0;
};

struct PSInput
{
    float4 pos : SV_Position;
    float3 dir : TEXCOORD0;
};

cbuffer UniformBlock : register(b0, space1)
{
    float4x4 mvp;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.pos = mul(mvp, float4(input.pos, 1.0f));
    output.pos.z = output.pos.w * 0.9999f;
    output.dir = input.pos;
    return output;
}

cbuffer SkyBlock : register(b0, space3)
{
    float3 to_sun;
    float pad0;
    float3 sun_color;
    float pad3;
    float3 cam_pos;
    float pad4;
    float2 wind_dir;
    float time;
    float cloud_speed;
    float3 cloud_color;
    float cobertura;
    float densidade;
    float escala;
    float h_min;
    float h_max;
    float cap;
    float passos;
    float absorcao;
    float hg_g_frontal;
    float powder_str;
    float sun_intensity;
    float sun_ambient;
    float cloud_type;
    float erosao_str;
    float cloud_silhouette;
    float cloud_bands;
    float cloud_ambient_str; // M2.5-24E-3: ocupa o padding implicito
    float3 sky_ambient_color;
    float detail_str; // M2.5-24C: Forca do detalhe de borda
    float3 cloud_shadow_color;
    float curl_str; // M2.5-24C: Forca da distorcao do curl noise
    float cloud_silver_k;      // M2.5-24E-4
    float cloud_silver_spread;
    float clima_cov_teto;  // M2.5-24G-3: teto de cobertura (embriao da maquina de clima)
    float clima_type_teto; // M2.5-24G-3: teto de tipo
    float cloud_curvatura; // M2.5-24G-6: horizonte honesto (camada desce ao longe)
    float fade_ini;        // M2.5-24G-6: inicio do fade (fracao do cap)
    float passo_far; // M2.5-24J-2 (A18): ocupa o antigo pad_h1
    float cloud_ms_str; // M2.5-24J-3: ocupa o antigo pad_h2
    float cloud_ms_g2;  // M2.5-24J-3
    float cloud_iso_str; // M2.5-24J-7: ocupa o pad_k[0]
    float sky_gain;      // M2.5-25-2: conversao de unidades
    float atmo_debug;    // M2.5-25-2: 0=off, 1=transmitancia, 2=multi-scattering
    float3 sun_illum;    // M2.5-25-2: iluminancia CRUA (offset 256 - float3 alinhado)
    float night_floor;   // M2.5-25-5a: piso do ceu noturno (ocupa o pad_l)
    float sun_disk_deg;    // M2.5-25-7: raio angular do disco (graus); 0.2725 = real
    float sun_disk_bright; // M2.5-25-7: brilho do disco (arte)
    float pad_m0;
    float pad_m1;
    float3 night_tint;     // M2.5-25-8: cor do piso noturno (ARTE)
    float pad_n;
    float3 to_moon;        // M2.5-25-9a
    float moon_disk_deg;
    float moon_disk_bright;
    float pad_o0;
    float pad_o1;
    float pad_o2;
    float3 to_astro;       // M2.5-25-12: astro dominante (sol de dia, lua de noite)
    float pad_p;
    float3 astro_sky_col;  // M2.5-25-12: cor JA multiplicada pela intensidade
    float aerial_str;      // M2.5-25-13: 1.0 = extincao fisica; acima = bruma de arte
    float3 moon_illum;     // M2.5-25-15: iluminancia da lua (E_i da Eq. 11)
    float pad_r;
};

Texture2D weather_map : register(t0, space2);   // M2.5-24G-1: weather map 512^2 (R=coverage, G=tipo)
SamplerState weather_samp : register(s0, space2); // sampler REPEAT: wrap perfeito, imune a precisao do tempo acelerado
Texture3D<float> cloud_noise : register(t1, space2);
Texture3D<float> cloud_detail : register(t2, space2); // M2.5-24C: Swiss Cheese Detail 32^3
SamplerState cloud_samp : register(s1, space2);

Texture2D trans_lut : register(t3, space2); // M2.5-25-2: Transmittance LUT 256x64 (Hillaire Â§5.2)
Texture2D ms_lut : register(t4, space2);    // M2.5-25-2: Multiple-scattering LUT 32x32 (Â§5.5)
SamplerState lut_samp : register(s2, space2);

struct PSOutput
{
    float4 color : SV_Target0;
    float4 normal_id : SV_Target1;
};

float hash12w(float2 pp)
{
    return frac(sin(dot(pp, float2(127.1f, 311.7f))) * 43758.5453f);
}

float vnoise2(float2 pp)
{
    float2 i = floor(pp);
    float2 f = frac(pp);
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float a = hash12w(i);
    float b = hash12w(i + float2(1, 0));
    float c = hash12w(i + float2(0, 1));
    float dd = hash12w(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, dd, u.x), u.y);
}

float wfbm(float2 pp)
{
    // M2.5-24F-4: 4 oitavas rotacionadas (Nubis: "several octaves") - as frequencias
    // medias que faltavam para o caos; pesos somam 1.0 (mesma faixa de antes).
    float2 pr = float2(pp.x * 0.8f - pp.y * 0.6f, pp.x * 0.6f + pp.y * 0.8f);
    float2 pr2 = float2(pr.x * 0.8f - pr.y * 0.6f, pr.x * 0.6f + pr.y * 0.8f);
    float v = vnoise2(pp) * 0.5f;
    v += vnoise2(pr * 2.3f) * 0.25f;
    v += vnoise2(pr2 * 5.1f) * 0.15f;
    v += vnoise2(pp * 11.7f + 31.4f) * 0.10f;
    return v;
}

static const float bayer[16] =
{
    0.0f / 16.0f, 8.0f / 16.0f, 2.0f / 16.0f, 10.0f / 16.0f,
    12.0f / 16.0f, 4.0f / 16.0f, 14.0f / 16.0f, 6.0f / 16.0f,
    3.0f / 16.0f, 11.0f / 16.0f, 1.0f / 16.0f, 9.0f / 16.0f,
    15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f, 5.0f / 16.0f
};

float hg_phase(float cos_theta, float g)
{
    float g2 = g * g;
    return (1.0f - g2) / (pow(1.0f + g2 - 2.0f * g * cos_theta, 1.5f) * 12.5663706f);
}

float get_height_fraction(float4 grad, float hf)
{
    return smoothstep(grad.x, grad.y, hf) * (1.0f - smoothstep(grad.z, grad.w, hf));
}

// M2.5-24G-2: gradiente vertical por tipo (stratus -> cumulus -> nimbus)
float4 grad_for_type(float ct)
{
    float4 grad_stratus = float4(0.0f, 0.07f, 0.08f, 0.15f);
    float4 grad_cumulus = float4(0.0f, 0.08f, 0.75f, 0.98f);
    float4 grad_nimbus = float4(0.0f, 0.06f, 0.88f, 1.0f);
    if (ct < 0.5f)
    {
        return lerp(grad_stratus, grad_cumulus, ct * 2.0f);
    }
    return lerp(grad_cumulus, grad_nimbus, (ct - 0.5f) * 2.0f);
}

// M2.5-24F-1: avaliador UNICO de densidade — marcha de camera, marcha de luz e
// futuros consumidores (sombras) leem o MESMO campo. Variantes divergentes da
// densidade ficam impossiveis por construcao (invariante estrutural do tribunal).
// dist_frac  = saturate(t/cap) na camera; 0.0 na marcha de luz (comportamento atual preservado)
// apply_detail = erosao de detalhe so na camera (padrao Nubis: luz usa versao barata)
// out_dim_profile = perfil dimensional (prof * coverage), consumido pelo ambient 24E-3
// M2.5-24J-2 (A17): o contorno NAO-detalhado da nuvem (Nutshell: "we skip all the
// noise" - so coverage + gradiente de altura, NENHUMA textura 3D). E o probe da
// marcha barata E a fonte do perfil dentro do avaliador: um campo so (24F-1).
// M2.5-24G-1: coverage do weather map assado (tile = 5000u de mundo).
// M2.5-24G-2: canal G = TIPO POR REGIAO; o slider "Tipo de Nuvem" vira vies global.
// M2.5-24G-3/4: tetos do clima ceifam os extremos; janela com respiro.
float CloudDimProfile(float3 p, float2 wdisp)
{
    float4 weather = weather_map.SampleLevel(weather_samp, (p.xz + wdisp) * 0.0002f, 0);
    float wcov = min(saturate((weather.r - 0.22f) / 0.55f), clima_cov_teto);
    float wtype = min(saturate(weather.g + (cloud_type - 0.5f)), clima_type_teto);
    float4 active_grad = grad_for_type(wtype);
    float hf = saturate((p.y - h_min) / max(h_max - h_min, 1.0f));
    float prof = get_height_fraction(active_grad, hf);
    return prof * saturate(cobertura * wcov);
}

float SampleCloudDensity(float3 p, float2 wdisp, float dist_frac, bool apply_detail, out float out_dim_profile)
{
    float3 uvw = frac((p + float3(wdisp.x, 0.0f, wdisp.y)) * escala);
    float noise_val = cloud_noise.SampleLevel(cloud_samp, uvw, 0).r;

    // M2.5-24J-2 (A17): o perfil dimensional vem da funcao unica CloudDimProfile -
    // o probe barato da marcha le o MESMO campo (invariante 24F-1 preservado).
    out_dim_profile = CloudDimProfile(p, wdisp);
    float hf = saturate((p.y - h_min) / max(h_max - h_min, 1.0f));



    // M2.5-24F-2: formula do Nubis 2022 (Schneider): saturate(noise - (1 - dimensional_profile)).
    // O perfil dimensional INTEIRO entra no limiar; o noise nao e mais multiplicado pelo perfil.
    float d_base = saturate(noise_val - (1.0f - out_dim_profile));
    // Erosao de topo mantida como tempero, atenuada no miolo (protege o nucleo - emenda do tribunal)
    float d_erosao = erosao_str * (1.0f - noise_val) * smoothstep(0.35f, 1.0f, hf) * (1.0f - 0.75f * dist_frac) * (1.0f - 0.5f * d_base);
    float d_raw = saturate(d_base - d_erosao);

    d_raw *= lerp(0.6f, 1.0f, smoothstep(0.0f, 0.25f, hf));

    // M2.5-24C: Anti-algodao (Curl warp & Swiss Cheese Erosion da borda)
    if (apply_detail && d_raw > 0.01f)
    {
        float2 cw = float2(
            vnoise2(p.xz * 0.004f + 17.3f) - 0.5f,
            vnoise2(p.xz * 0.004f - 31.7f) - 0.5f) * curl_str * 30.0f;

        float det = cloud_detail.SampleLevel(cloud_samp, frac((p + float3(cw.x, 0.0f, cw.y)) * escala * 6.1f), 0).r;

        float borda = 1.0f - saturate(d_raw / 0.35f);
        float det_str_h = detail_str * lerp(1.25f, 0.7f, pow(hf, 0.25f));

        det_str_h *= (1.0f - 0.65f * dist_frac); // HZD Nubis

        d_raw = saturate(d_raw - det * borda * det_str_h);
    }

    // M2.5-24F-3: recorte no DOMINIO DA DENSIDADE (V7): a borda nasce aqui, antes
    // da integracao - luz e alpha leem o mesmo campo, franja fantasma impossivel.
    // O slider "Silhueta" agora e o piso de recorte da densidade.
    // M2.5-24J-4 (V5, GPU Pro 7 verbatim): "remapping prevents a loss of too much
    // density at the core" - remap linear preserva a gradacao do miolo; a janela
    // estreita de 0.18 esmagava tudo acima dela em 1.0 (plato = silhueta dura).
    // O piso "Silhueta" permanece: zero abaixo dele (fantasma 24F-3 segue morto).
    return saturate((d_raw - cloud_silhouette) / max(1.0f - cloud_silhouette, 1e-4f));
}

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

float3 atmo_sky(float3 dir)
{
    // mundo: 1 unidade ~ 1 metro -> km
    float3 p0 = float3(0.0f, kRg + max(cam_pos.y, 0.0f) * 0.001f, 0.0f);

    float t_top = atmo_ray_sphere(p0, dir, kRt);
    if (t_top <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

    float t_max = t_top;
    bool hit_ground = false; // M2.5-25-3: o termo Lo precisa saber
    {
        float b = dot(p0, dir);
        float c = dot(p0, p0) - kRg * kRg;
        float disc = b * b - c;
        if (disc >= 0.0f)
        {
            float tg = -b - sqrt(disc);
            if (tg > 0.0f) { t_max = tg; hit_ground = true; } // o chao corta a marcha
        }
    }

    // M2.5-25-15: uma fase por LUZ (Eq. 11 soma sobre Nlight)
    float pr_s, pm_s, pr_m, pm_m;
    atmo_phases(dot(dir, to_sun),  pr_s, pm_s);
    atmo_phases(dot(dir, to_moon), pr_m, pm_m);

    const int N = 20; // Tabela 2
    float dt = t_max / (float)N;
    float3 L = float3(0.0f, 0.0f, 0.0f);
    float3 T = float3(1.0f, 1.0f, 1.0f);

    [loop]
    for (int i = 0; i < N; ++i)
    {
        float3 x = p0 + dir * ((i + 0.5f) * dt);
        float r = length(x);
        float h = r - kRg;

        float dr_ = exp(-h / 8.0f);
        float dm_ = exp(-h / 1.2f);
        float3 ss_r = kRayS * dr_;
        float3 ss_m = kMieS * dm_;
        float3 st = atmo_sigma_t(h);

        // M2.5-25-15: Eq. 11 verbatim - SOMA sobre as luzes direcionais, cada uma
        // com sua fase, sua transmitancia, seu Psi_ms e sua iluminancia E_i.
        float3 up_local = x / r;
        float3 S = atmo_light(up_local, h, ss_r, ss_m, to_sun,  pr_s, pm_s) * sun_illum
                 + atmo_light(up_local, h, ss_r, ss_m, to_moon, pr_m, pm_m) * moon_illum;
        float3 e = exp(-st * dt);
        float3 Sint = (S - S * e) / max(st, 1e-7f); // Hillaire 2015 sl.28
        L += T * Sint;
        T *= e;
    }

    // M2.5-25-3: o termo T(c,p)*Lo(p,v) da Eq. 1 do Hillaire - resposta difusa do
    // chao do planeta virtual (albedo 0.3, Â§4). Sem ele, tudo abaixo da linha do
    // horizonte fica PRETO (a marcha para no chao e nao sobra in-scattering).
    if (hit_ground)
    {
        float3 pg = p0 + dir * t_max;
        float3 ng = pg / length(pg);
        // M2.5-25-15: o chao tambem e iluminado pelas DUAS luzes (Eq. 1 por fonte)
        float mu_gs = dot(ng, to_sun);
        float mu_gm = dot(ng, to_moon);
        float3 Tg_s = trans_lut.SampleLevel(lut_samp, atmo_trans_uv(0.0f, mu_gs), 0).rgb;
        float3 Tg_m = trans_lut.SampleLevel(lut_samp, atmo_trans_uv(0.0f, mu_gm), 0).rgb;
        float3 Lo = Tg_s * max(0.0f, mu_gs) * sun_illum + Tg_m * max(0.0f, mu_gm) * moon_illum;
        L += T * Lo * (0.3f / 3.14159265f); // albedo 0.3 (Â§4)
    }

    // 25-15: a iluminancia E_i ja foi aplicada POR LUZ dentro da soma
    return L;
}
PSOutput PSMain(PSInput input)
{
    float3 dir = normalize(input.dir);

    float h = saturate(dir.y);
    // M2.5-25-2: o domo procedural MORREU (lerp horizonte->zenite + paleta do
    // crepusculo). O ceu agora e a atmosfera fisica de Hillaire 2020. O ganho e
    // conversao de UNIDADES (o paper e fotometrico; nosso sun_intensity e
    // arbitrario) - unico numero desta estacao que nao esta na fonte.
    // M2.5-25-5a / 25-8: PISO DO CEU NOTURNO. Sem ele, o modelo fisico devolve
    // preto absoluto a noite (transmitancia ao sol = 0).
    // COM FONTE (ING sky brightness, 3 fontes convergentes): o ceu sem lua NAO e
    // preto (~2e-4 cd/m2), e sua composicao e airglow + luz zodiacal + luz estelar.
    // SEM FONTE (correcao da 25-8): a COR. A divisao 145/60/15 do documento esta em
    // unidades S10(vis) - magnitudes fotometricas numa banda, nao pesos RGB. O verde
    // que estava aqui foi extrapolacao. Alem disso, o airglow verde e fenomeno de
    // fotografia de longa exposicao; o olho adaptado ao escuro ve cinza-azulado
    // (Purkinje - lacuna declarada na fornada). Logo: a cor e ARTE, no painel.
    // Somado sem condicional: de dia ele e desprezivel, como na natureza.
    float3 ceu = (atmo_sky(dir) + night_floor * night_tint) * sky_gain;

    float cos_sun = dot(dir, to_sun);
    // M2.5-25-12: as NUVENS seguem o astro dominante; o disco do sol e da lua
    // continuam usando to_sun (o sol de verdade), como a geometria exige.
    float cos_astro = dot(dir, to_astro);

    // Â§5.3 / S6: o disco do sol e composto SEPARADO. O halo fake ao redor dele
    // foi REMOVIDO - o halo agora nasce do espalhamento de Mie na marcha.
    // M2.5-25-7: DISCO SOLAR FISICO.
    // Raio angular real = 0.2725 graus (diametro 0.545) - fornada N1.1. O disco
    // anterior tinha ~1.8-3.1 graus e nenhum perfil de brilho: lia como esfera chapada.
    // Escurecimento de borda (Hestroffer & Magnan 1998, verbatim):
    //   L(phi) = L(0) * [1 - u*(1 - sqrt((phi_sol^2 - phi^2)/phi_sol^2))],  u = 0.6
    // A transmitancia atmosferica ja esta embutida: sun_color carrega t_sol (25-1).
    float phi_sun = acos(clamp(cos_sun, -1.0f, 1.0f));
    float phi_disk = radians(max(sun_disk_deg, 1e-4f));
    float x_disk = saturate(phi_sun / phi_disk);
    float limb = 1.0f - 0.6f * (1.0f - sqrt(saturate(1.0f - x_disk * x_disk)));
    float edge = 1.0f - smoothstep(0.88f, 1.0f, x_disk); // 1 pixel de suavidade no fat-pixel
    float3 astro = sun_color * (limb * edge * sun_disk_bright);

    // M2.5-25-9a: DISCO DA LUA COM FASE REAL.
    // Reconstruimos a normal da esfera lunar por pixel do disco e iluminamos com
    // Lambert (rho = 0.11, bond albedo da tese CosmoScout VR, secao 3.3.2 -
    // "An Ideal Diffuse Moon"). A fase (crescente/gibosa) EMERGE da geometria:
    // nao ha mascara nem textura de fase, so N.L contra a direcao do sol.
    {
        float cos_moon = dot(dir, to_moon);
        float phi_m = acos(clamp(cos_moon, -1.0f, 1.0f));
        float phi_disk_m = radians(max(moon_disk_deg, 1e-4f));
        if (phi_m < phi_disk_m)
        {
            // base ortonormal no plano do disco
            float3 aux = (abs(to_moon.y) < 0.99f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            float3 rgt = normalize(cross(aux, to_moon));
            float3 upv = cross(to_moon, rgt);
            // deslocamento angular normalizado para o disco unitario
            float2 duv = float2(dot(dir, rgt), dot(dir, upv)) / sin(phi_disk_m);
            float zz = sqrt(saturate(1.0f - dot(duv, duv)));
            // normal da superficie lunar (no centro do disco aponta para nos)
            float3 Nm = rgt * duv.x + upv * duv.y - to_moon * zz;
            float lam = saturate(dot(Nm, to_sun));           // a fase, por geometria
            float edge_m = 1.0f - smoothstep(0.88f, 1.0f, length(duv));
            // M2.5-26-5 (N1.4): o disco da lua tambem atravessa a atmosfera.
            // A checklist da noite exige, verbatim: "Disco multiplicado pela
            // Transmittance LUT na direcao dele (avermelha no poente) - etapa
            // EXPLICITA, nao embutida". O disco do SOL ja fazia isso desde a 25-7
            // (sun_color carrega t_sol); a LUA ficou de fora. Sintoma: ela descia
            // branca ate encostar no horizonte e sumia de supetao no teste de
            // oclusao do planeta, enquanto o halo em volta - esse sim atenuado
            // pela marcha - ja estava vermelho. Branco, depois vermelho, depois
            // nada: exatamente o que a retina relatou as 5.3h.
            // A LUT vale 0 para direcoes bloqueadas pelo chao, entao o corte do
            // planeta passa a ser invisivel: o disco chega ao horizonte ja preto.
            float3 t_moon_disk = trans_lut.SampleLevel(lut_samp, atmo_trans_uv(0.0f, to_moon.y), 0).rgb;
            astro += t_moon_disk * ((0.11f / 3.14159265f) * lam * edge_m * moon_disk_bright);
        }
    }

    // M2.5-25-9b: O PLANETA OCLUI OS ASTROS. Sem isto, sol e lua sao desenhados
    // abaixo da linha do horizonte - visiveis na metade de baixo do domo, alem da
    // borda do terreno ("a lua no chao"). E o MESMO teste raio-esfera que a marcha
    // ja usa para cortar em atmo_sky(); aqui aplicado a composicao dos discos.
    // Sendo por pixel, o nascer e o poente saem certos de graca: o astro e cortado
    // pela linha do horizonte, sem mascara nem fade artificial.
    {
        float3 p0_a = float3(0.0f, kRg + max(cam_pos.y, 0.0f) * 0.001f, 0.0f);
        float b_a = dot(p0_a, dir);
        float c_a = dot(p0_a, p0_a) - kRg * kRg;
        float disc_a = b_a * b_a - c_a;
        if (disc_a >= 0.0f && (-b_a - sqrt(disc_a)) > 0.0f)
        {
            astro = float3(0.0f, 0.0f, 0.0f); // atras do mundo
        }
    }

    // Vista de depuracao das LUTs (salvaguarda de atribuicao desta estacao)
    if (atmo_debug > 0.5f)
    {
        float2 duv = float2(frac(atan2(dir.z, dir.x) * 0.15915494f + 0.5f), saturate(dir.y));
        ceu = (atmo_debug > 1.5f) ? ms_lut.SampleLevel(lut_samp, duv, 0).rgb * 20.0f
                                  : trans_lut.SampleLevel(lut_samp, duv, 0).rgb;
        astro = float3(0.0f, 0.0f, 0.0f);
    }

    // M2.5-24E-4: soma convexa normalizada + silver lining artistico via max (Nubis 2017); k=0 = fisica pura
    float phase_base = lerp(hg_phase(cos_astro, hg_g_frontal), hg_phase(cos_astro, -0.2f), 0.5f);
    float phase_val = max(phase_base, cloud_silver_k * hg_phase(cos_astro, 0.99f - cloud_silver_spread));
    float sun_angular_gate = saturate(-cos_astro);

    float T = 1.0f;
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float cloud_dist = 9999.0f;

    // M2.5-24G-2: o gradiente por tipo agora e POR AMOSTRA (regional), dentro do avaliador.

    // M2.5-24G-3: abs() = camera ACIMA da camada olhando para baixo tambem marcha (fix voo alto)
    if (abs(dir.y) > 0.01f || (cam_pos.y >= h_min && cam_pos.y <= h_max))
    {
        float t_min = (h_min - cam_pos.y) / dir.y;
        float t_max = (h_max - cam_pos.y) / dir.y;

        if (t_min > t_max)
        {
            float tmp = t_min;
            t_min = t_max;
            t_max = tmp;
        }

        t_min = max(t_min, 0.0f);
        t_max = min(t_max, t_min + cap);

        if (t_max > t_min)
        {
            cloud_dist = t_min;
            float step_sz = (t_max - t_min) / passos;
            int2 px = int2(input.pos.xy) % 4;
            float dither = bayer[px.y * 4 + px.x];
            float t = t_min + step_sz * 2.0f * dither; // 24J-2c: jitter cobre o quantum BARATO (2 passos) - fases decorrelacionadas

            float2 wdisp = wind_dir * time * cloud_speed * -200.0f;

            // M2.5-24J-2 (A17, Nutshell): marcha BARATA (contorno sem noise, passo 2x)
            // ate tocar nuvem; recua 1 passo e liga o modo CARO. Amostras caras zeradas
            // seguidas = "no longer in the cloud" -> volta ao barato (contador 6 =
            // concretizacao da casa; a fonte nao fixa numero).
            bool modo_caro = false;
            int zeros_seguidos = 0;
            bool deu_passo_barato = true; // 24J-2b: recuo so e permitido depois de ANDAR barato
            int amostras_caras = 0; // 24J-5: orcamento de amostras CARAS (= Passos); o 3x e so p/ sondas

            [loop] // M2.5-24J-2: marcha com ESTADO (barato/caro) - loop dinamico
            for (int i = 0; i < (int) (passos * 3.0f); ++i) // 24J-2b: orcamento de ITERACOES (3x Passos); amostras caras continuam ~Passos
            {
                float3 p = cam_pos + dir * t;

                // M2.5-24G-6: curvatura fake - aproximacao barata da casca esferica do Nubis
                float2 dxz = p.xz - cam_pos.xz;
                float3 p_c = p;
                p_c.y += dot(dxz, dxz) * cloud_curvatura;

                if (t >= t_max) break; // 24J-2: o passo adaptativo pode cruzar o teto do slab

                // M2.5-24J-2 (A18, Nubis 2022: step = near + far_offset * dist): o passo
                // cresce LINEAR com a distancia; "Passo no longe" 0 = passo fixo de antes.
                float sz = step_sz * (1.0f + passo_far * saturate(t / cap));

                if (p_c.y < h_min || p_c.y > h_max)
                {
                    deu_passo_barato = true; // 24J-2b: movimento real - recuo seguro
                    t += sz * 2.0f; // fora da camada: passo largo de graca
                    continue;
                }

                if (!modo_caro)
                {
                    // A17: amostra BARATA - "we skip all the noise": so coverage+perfil.
                    // Teste SEM PERDA: d_raw <= perfil e o recorte zera abaixo de "Silhueta",
                    // logo perfil <= cloud_silhouette garante amostra cara = 0.
                    if (CloudDimProfile(p_c, wdisp) > cloud_silhouette)
                    {
                        modo_caro = true;
                        zeros_seguidos = 0;
                        if (deu_passo_barato) // 24J-2b: so recua se a marcha barata andou ate aqui
                        {
                            t = max(t - sz * (2.0f - dither), t_min); // 24J-2c: "one step back" com jitter - pixels vizinhos nao realinham
                        }
                        deu_passo_barato = false;
                        continue;
                    }
                    deu_passo_barato = true;
                    t += sz * 2.0f; // passo largo: "reduces the number of samples by half"
                    continue;
                }

                amostras_caras++;
                if (amostras_caras > (int) passos) break; // 24J-5: teto = Passos (devolve o FPS com densidade baixa)
                float dim_profile;
                float d_raw = SampleCloudDensity(p_c, wdisp, saturate(t / cap), true, dim_profile);

                float fade = 1.0f - smoothstep(cap * fade_ini, cap, t - t_min);
                float d = d_raw * densidade * fade;

                if (d > 0.0f)
                {
                    zeros_seguidos = 0;
                    // M2.5-24F-5: passos exponenciais (Frostbite) + amostra longa (Schneider 2015:
                    // "the last sample is placed far away to capture shadows cast by distant clouds")
                    float dens_sol = 0.0f;
                    float prev_ts = 0.0f;
                    [unroll]
                    for (int j = 0; j < 4; ++j)
                    {
                        float t_s = (j == 0) ? 6.0f : ((j == 1) ? 15.0f : ((j == 2) ? 30.0f : 60.0f));
                        float3 p_sol = p_c + to_astro * t_s; // 25-12
                        float dim_unused;
                        float d_sol = SampleCloudDensity(p_sol, wdisp, 0.0f, false, dim_unused);
                        dens_sol += d_sol * densidade * (t_s - prev_ts);
                        prev_ts = t_s;
                    }
                    {
                        float3 p_far = p_c + to_astro * 150.0f; // 25-12
                        float dim_unused2;
                        float d_far = SampleCloudDensity(p_far, wdisp, 0.0f, false, dim_unused2);
                        dens_sol += d_far * densidade * 24.0f;
                    }

                    // M2.5-24J-3: Direct Scattering na forma Nubis3 (Schneider 2023, pt3, verbatim):
                    //   Direct = (Transmittance x Primary Phase) + (MS x Secondary Phase)
                    //   ms_volume = dimensional_profile * exp(-dens_sol * Remap(sun_dot, 0, 0.9, 0.25, ValueRemap(depth, .., 0.05, 0.25)))
                    // Adaptacao da casa (sem SDF de voxel): profundidade ~ perfil dimensional
                    // (a MESMA variavel que o Nubis3 usa como base do ms_volume). Constantes de
                    // extincao 0.25 / 0.05-0.25 e janela sun_dot 0->0.9 = fonte. ms_str/g2 = retina.
                    float t_sun = exp(-dens_sol * absorcao);
                    float k_ms = lerp(0.25f, lerp(0.25f, 0.05f, saturate(dim_profile)), saturate(cos_astro / 0.9f));
                    float ms_vol = saturate(dim_profile) * exp(-dens_sol * absorcao * k_ms);
                    // M2.5-25-14: o terceiro termo isotropico entra SOMADO A FASE, dentro da
                    // integral - e assim que uma funcao de fase multi-lobo se compoe. Como fase,
                    // ele multiplica a atenuacao t_sun, que varia ao longo da marcha de luz:
                    // o miolo escurece e a casca acende mesmo longe do sol.
                    // A forma anterior (24J-7) somava 0.12 DEPOIS da atenuacao e, a 90 graus do
                    // sol, essa constante era 2.5x a fase (0.048) - tudo chapava.
                    float luz = t_sun * (phase_val + cloud_iso_str) + ms_vol * cloud_ms_str * hg_phase(cos_astro, cloud_ms_g2);
                    float powder = 1.0f - exp(-2.0f * dens_sol);
                    luz *= lerp(1.0f, powder, sun_angular_gate * powder_str);

                    if (cloud_bands >= 2.0f)
                    {
                        luz = (floor(luz * cloud_bands) + smoothstep(0.35f, 0.65f, frac(luz * cloud_bands))) / cloud_bands;
                    }

                    // M2.5-24E-3: Nubis ambient scattering (Schneider 2022) - maximo na casca, cai no miolo
                    float amb_scat = pow(saturate(1.0f - dim_profile), 0.5f);
                    float3 ambient_term = sky_ambient_color * (amb_scat * cloud_ambient_str);

                    // 25-12: cor do astro dominante (ja pre-multiplicada) Â· 25-14: o isotropico
                    // saiu daqui e virou piso de fase, acima.
                    float3 direct_term = luz * astro_sky_col;

                    radiance += T * (1.0f - exp(-d * sz * absorcao)) * (cloud_color * (ambient_term + direct_term));
                    T *= exp(-d * sz * absorcao);
                }
                else
                {
                    zeros_seguidos++; // A17: "when we detect that we are no longer in the cloud"
                    if (zeros_seguidos >= 6) { modo_caro = false; }
                }

                if (T < 0.05f)
                {
                    break; // 24J-6: para SEM forcar T=0 - o snap criava a iso-linha dura do miolo
                }
                t += sz;
            }
        }
    }

    // M2.5-24F-3: corte pos-integral REMOVIDO (V7 do tribunal: nenhuma referencia
    // de producao remapeia o alpha final; a borda pertence ao dominio da densidade).

    // M2.5-24J-6: remap continuo do threshold do early-out - chega a 0 exatamente onde
    // o snap chegava, sem degrau e sem ~5% de ceu HDR vazando no miolo. Ceu limpo: 1.0 exato.
    T = saturate((T - 0.05f) / 0.95f);

    // M2.5-25-13: PERSPECTIVA AEREA FISICA.
    // Antes: fog_air = saturate(cloud_dist / (cap*1.2)) * 0.85 - uma rampa linear
    // normalizada pelo 'cap', que e o ORCAMENTO DE MARCHA, nao distancia de mundo.
    // Da altura t_min explode (1000u de altitude, 3 graus abaixo da horizontal ->
    // ~12.000u ate a camada), a rampa satura e 85% da nuvem virava ceu.
    // Agora: extincao REAL do ar com o MESMO sigma_t da Tabela 1 do Hillaire que a
    // marcha do ceu usa, na forma canonica "cor atenuada + in-scatter". O in-scatter
    // e o 'ceu' - a cor fisica da atmosfera naquela direcao (25-10).
    // ESCOPO: perspectiva aerea avaliada NUM PONTO, nao a LUT 32^3 por froxel (5.4).
    // "Nevoa aerea" (aerial_str) = ARTE: 1.0 = fisico puro; acima disso e bruma
    // por direcao de arte.
    float h_aer_km = max((cam_pos.y + (h_min + h_max) * 0.5f) * 0.5f, 0.0f) * 0.001f;
    float3 T_aer = exp(-atmo_sigma_t(h_aer_km) * (min(cloud_dist, 200000.0f) * 0.001f * aerial_str));

    float3 final_color = radiance * T_aer + ceu * (1.0f - T) * (1.0f - T_aer) + T * (ceu + astro);
    final_color = min(max(final_color, 0.0f), 8.0f);

    PSOutput output;
    // M2.5-24G-7: alpha carrega a transmitancia - nuvem grossa OCLUI god ray no fullscreen
    output.color = float4(final_color, T);
    output.normal_id = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return output;
}
