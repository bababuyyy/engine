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
    float3 zenith_color;
    float pad1;
    float3 horizon_color;
    float pad2;
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
    float pad_h2;
};

Texture2D weather_map : register(t0, space2);   // M2.5-24G-1: weather map 512^2 (R=coverage, G=tipo)
SamplerState weather_samp : register(s0, space2); // sampler REPEAT: wrap perfeito, imune a precisao do tempo acelerado
Texture3D<float> cloud_noise : register(t1, space2);
Texture3D<float> cloud_detail : register(t2, space2); // M2.5-24C: Swiss Cheese Detail 32^3
SamplerState cloud_samp : register(s1, space2);

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
    return smoothstep(cloud_silhouette, cloud_silhouette + 0.18f, d_raw);
}

PSOutput PSMain(PSInput input)
{
    float3 dir = normalize(input.dir);

    float h = saturate(dir.y);
    float3 ceu = lerp(horizon_color, zenith_color, pow(h, 0.6f));

    float cos_sun = dot(dir, to_sun);

    float sun_core = smoothstep(0.9985f, 0.9995f, cos_sun);
    sun_core = pow(sun_core, 0.6f);
    float3 sun_disk = sun_color * sun_core * 0.75f;

    float angle = acos(saturate(cos_sun));
    float halo_str = lerp(1.0f, 2.5f, 1.0f - saturate(dir.y * 4.0f));
    float3 sun_halo = sun_color * exp(-angle / 0.04f) * 0.5f +
                      sun_color * exp(-angle / 0.15f) * 0.15f * halo_str;

    float3 astro = sun_disk + sun_halo;

    // M2.5-24E-4: soma convexa normalizada + silver lining artistico via max (Nubis 2017); k=0 = fisica pura
    float phase_base = lerp(hg_phase(cos_sun, hg_g_frontal), hg_phase(cos_sun, -0.2f), 0.5f);
    float phase_val = max(phase_base, cloud_silver_k * hg_phase(cos_sun, 0.99f - cloud_silver_spread));
    float sun_angular_gate = saturate(-cos_sun);

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
            float t = t_min + step_sz * dither;

            float2 wdisp = wind_dir * time * cloud_speed * -200.0f;

            // M2.5-24J-2 (A17, Nutshell): marcha BARATA (contorno sem noise, passo 2x)
            // ate tocar nuvem; recua 1 passo e liga o modo CARO. Amostras caras zeradas
            // seguidas = "no longer in the cloud" -> volta ao barato (contador 6 =
            // concretizacao da casa; a fonte nao fixa numero).
            bool modo_caro = false;
            int zeros_seguidos = 0;

            [loop] // M2.5-24J-2: marcha com ESTADO (barato/caro) - loop dinamico
            for (int i = 0; i < (int) passos; ++i)
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
                        t = max(t - sz * 2.0f, t_min); // "we take one step back" (Nutshell)
                        continue;
                    }
                    t += sz * 2.0f; // passo largo: "reduces the number of samples by half"
                    continue;
                }

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
                        float3 p_sol = p_c + to_sun * t_s;
                        float dim_unused;
                        float d_sol = SampleCloudDensity(p_sol, wdisp, 0.0f, false, dim_unused);
                        dens_sol += d_sol * densidade * (t_s - prev_ts);
                        prev_ts = t_s;
                    }
                    {
                        float3 p_far = p_c + to_sun * 150.0f;
                        float dim_unused2;
                        float d_far = SampleCloudDensity(p_far, wdisp, 0.0f, false, dim_unused2);
                        dens_sol += d_far * densidade * 24.0f;
                    }

                    // M2.5-24E-2: multi-scatter dual-exp (Schneider, Nubis 2017)
                    float luz = max(exp(-dens_sol * absorcao), 0.7f * exp(-0.25f * dens_sol * absorcao));
                    float powder = 1.0f - exp(-2.0f * dens_sol);
                    luz *= lerp(1.0f, powder, sun_angular_gate * powder_str);

                    if (cloud_bands >= 2.0f)
                    {
                        luz = (floor(luz * cloud_bands) + smoothstep(0.35f, 0.65f, frac(luz * cloud_bands))) / cloud_bands;
                    }

                    // M2.5-24E-3: Nubis ambient scattering (Schneider 2022) - maximo na casca, cai no miolo
                    float amb_scat = pow(saturate(1.0f - dim_profile), 0.5f);
                    float3 ambient_term = sky_ambient_color * (amb_scat * cloud_ambient_str);

                    float3 direct_term = (luz * phase_val + 0.12f) * sun_color * sun_intensity;

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
                    T = 0.0f;
                    break;
                }
                t += sz;
            }
        }
    }

    // M2.5-24F-3: corte pos-integral REMOVIDO (V7 do tribunal: nenhuma referencia
    // de producao remapeia o alpha final; a borda pertence ao dominio da densidade).

    float fog_air = saturate(cloud_dist / (cap * 1.2f)) * 0.85f;

    float3 final_color = lerp(radiance, horizon_color * (1.0f - T), fog_air) + T * (ceu + astro);
    final_color = min(max(final_color, 0.0f), 8.0f);

    PSOutput output;
    // M2.5-24G-7: alpha carrega a transmitancia - nuvem grossa OCLUI god ray no fullscreen
    output.color = float4(final_color, T);
    output.normal_id = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return output;
}
