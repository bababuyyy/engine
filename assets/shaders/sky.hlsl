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
    float3 sky_ambient_color;
    float detail_str; // M2.5-24C: Forca do detalhe de borda
    float3 cloud_shadow_color;
    float curl_str; // M2.5-24C: Forca da distorcao do curl noise
};

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
    float2 pr = float2(pp.x * 0.8f - pp.y * 0.6f, pp.x * 0.6f + pp.y * 0.8f);
    return vnoise2(pp) * 0.66f + vnoise2(pr * 2.7f) * 0.34f;
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

    float phase_val = lerp(hg_phase(cos_sun, hg_g_frontal), hg_phase(cos_sun, -0.2f), 0.5f);
    float sun_angular_gate = saturate(-cos_sun);

    float T = 1.0f;
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float cloud_dist = 9999.0f;

    float4 grad_stratus = float4(0.0f, 0.07f, 0.08f, 0.15f);
    float4 grad_cumulus = float4(0.0f, 0.08f, 0.75f, 0.98f);
    float4 grad_nimbus = float4(0.0f, 0.06f, 0.88f, 1.0f);

    float4 active_grad;
    if (cloud_type < 0.5f)
    {
        active_grad = lerp(grad_stratus, grad_cumulus, cloud_type * 2.0f);
    }
    else
    {
        active_grad = lerp(grad_cumulus, grad_nimbus, (cloud_type - 0.5f) * 2.0f);
    }

    if (dir.y > 0.01f || (cam_pos.y >= h_min && cam_pos.y <= h_max))
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

            [unroll(48)]
            for (int i = 0; i < (int) passos; ++i)
            {
                float3 p = cam_pos + dir * t;

                if (p.y < h_min || p.y > h_max)
                {
                    t += step_sz;
                    continue;
                }

                float3 uvw = frac((p + float3(wdisp.x, 0.0f, wdisp.y)) * escala);
                float noise_val = cloud_noise.SampleLevel(cloud_samp, uvw, 0).r;
                float fade = 1.0f - smoothstep(cap * 0.8f, cap, t - t_min);

                float wcov = saturate((wfbm((p.xz + wdisp) * 0.0008f) - 0.32f) / 0.36f);
                float hf = saturate((p.y - h_min) / max(h_max - h_min, 1.0f));

                float prof = get_height_fraction(active_grad, hf);

                float d_base = noise_val * prof - (1.0f - cobertura * wcov);
                float d_erosao = erosao_str * (1.0f - noise_val) * smoothstep(0.35f, 1.0f, hf) * (1.0f - 0.75f * saturate(t / cap));
                float d_raw = saturate(d_base - d_erosao);

                d_raw *= lerp(0.6f, 1.0f, smoothstep(0.0f, 0.25f, hf));

                // M2.5-24C: Anti-algodao (Curl warp & Swiss Cheese Erosion da borda)
                if (d_raw > 0.01f)
                {
                    float2 cw = float2(
                        vnoise2(p.xz * 0.004f + 17.3f) - 0.5f,
                        vnoise2(p.xz * 0.004f - 31.7f) - 0.5f) * curl_str * 30.0f;

                    float det = cloud_detail.SampleLevel(cloud_samp, frac((p + float3(cw.x, 0.0f, cw.y)) * escala * 6.1f), 0).r;

                    float borda = 1.0f - saturate(d_raw / 0.35f);
                    float det_str_h = detail_str * lerp(1.25f, 0.7f, pow(hf, 0.25f));

                    det_str_h *= (1.0f - 0.65f * saturate(t / cap)); // HZD Nubis

                    d_raw = saturate(d_raw - det * borda * det_str_h);
                }

                d_raw = smoothstep(0.0f, 0.18f, d_raw);
                float d = d_raw * densidade * fade;

                if (d > 0.0f)
                {
                    float dens_sol = 0.0f;
                    float step_sol = 12.0f;
                    for (int j = 1; j <= 4; ++j)
                    {
                        float3 p_sol = p + to_sun * (j * step_sol);
                        float3 uvw_sol = frac((p_sol + float3(wdisp.x, 0.0f, wdisp.y)) * escala);
                        float n_sol = cloud_noise.SampleLevel(cloud_samp, uvw_sol, 0).r;
                        float hf_s = saturate((p_sol.y - h_min) / max(h_max - h_min, 1.0f));
                        float prof_s = get_height_fraction(active_grad, hf_s);
                        float wcov_s = saturate((wfbm((p_sol.xz + wdisp) * 0.0008f) - 0.32f) / 0.36f);

                        float d_s_base = n_sol * prof_s - (1.0f - cobertura * wcov_s);
                        float d_s_erosao = erosao_str * (1.0f - n_sol) * smoothstep(0.35f, 1.0f, hf_s);
                        float d_sol_raw = saturate(d_s_base - d_s_erosao) * lerp(0.6f, 1.0f, smoothstep(0.0f, 0.25f, hf_s));

                        dens_sol += smoothstep(0.0f, 0.18f, d_sol_raw) * densidade * step_sol;
                    }

                    float luz = exp(-dens_sol * absorcao);
                    float powder = 1.0f - exp(-2.0f * dens_sol);
                    luz *= lerp(1.0f, powder, sun_angular_gate * powder_str);

                    if (cloud_bands >= 2.0f)
                    {
                        luz = (floor(luz * cloud_bands) + smoothstep(0.35f, 0.65f, frac(luz * cloud_bands))) / cloud_bands;
                    }

                    float3 base_ambient = lerp(cloud_shadow_color, sky_ambient_color, 0.4f);
                    float3 ambient_term = base_ambient * sun_ambient * lerp(0.72f, 1.0f, hf);

                    float3 direct_term = (luz * phase_val + 0.12f) * sun_color * sun_intensity;

                    radiance += T * (1.0f - exp(-d * step_sz * absorcao)) * (cloud_color * (ambient_term + direct_term));
                    T *= exp(-d * step_sz * absorcao);
                }

                if (T < 0.05f)
                {
                    T = 0.0f;
                    break;
                }
                t += step_sz;
            }
        }
    }

    // M2.5-24D-b: corte de silhueta com rescale premultiplicado.
    // O smoothstep corta a OPACIDADE das bordas finas; a radiancia acumulada
    // precisa encolher na MESMA proporcao, senao a luz da nuvem cortada soma
    // por cima do ceu restaurado -> silhueta fantasma translucida.
    float alpha = 1.0f - T;
    float alpha_cut = smoothstep(cloud_silhouette, cloud_silhouette + 0.12f, alpha);
    radiance *= (alpha > 0.001f) ? (alpha_cut / alpha) : 0.0f;
    T = 1.0f - alpha_cut;

    float fog_air = saturate(cloud_dist / 900.0f) * 0.65f;

    float3 final_color = lerp(radiance, horizon_color * (1.0f - T), fog_air) + T * (ceu + astro);
    final_color = min(max(final_color, 0.0f), 8.0f);

    PSOutput output;
    output.color = float4(final_color, 1.0f);
    output.normal_id = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return output;
}
