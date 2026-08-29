struct VSInput
{
    float3 pos : TEXCOORD0;
    float3 color : TEXCOORD1;
    float3 inst_pos : TEXCOORD2;
    float3 inst_color : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float3 normal : TEXCOORD5;
};

struct PSInput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 world_pos : TEXCOORD2;
    float3 inst_color : TEXCOORD3;
    float h01 : TEXCOORD4;
    float3 local_normal : TEXCOORD5;
};

cbuffer GrassVSBlock : register(b0, space1)
{
    float4x4 mvp;
    float3 cam_pos;
    float scale;
    float2 wind_dir;
    float wind_strength;
    float time;
};

float hash12(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

float vnoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float a = hash12(i + float2(0.0f, 0.0f));
    float b = hash12(i + float2(1.0f, 0.0f));
    float c = hash12(i + float2(0.0f, 1.0f));
    float d = hash12(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float gust(float2 pos_xz, float t)
{
    float2 p = pos_xz * 0.08f - wind_dir * t * 2.0f;
    return vnoise(p) * 0.66f + vnoise(p * 2.0f) * 0.34f;
}

PSInput VSMain(VSInput input)
{
    PSInput output;

    float tuft_hash = frac(sin(dot(input.inst_pos.xz, float2(12.9898f, 78.233f))) * 43758.5453f);
    float tuft_h = lerp(0.75f, 1.25f, frac(tuft_hash * 13.37f));
    float clump_hash = frac(sin(dot(floor(input.inst_pos.xz / 2.5f), float2(12.9898f, 78.233f))) * 43758.5453f);
    float clump_h = lerp(0.8f, 1.2f, clump_hash);
    float total_h = tuft_h * clump_h;

    float3 vpos = input.pos * scale;
    vpos.y *= total_h;

    float angle = tuft_hash * 6.283185f;
    float s = sin(angle);
    float c = cos(angle);
    float x = vpos.x * c - vpos.z * s;
    float z = vpos.x * s + vpos.z * c;
    vpos.x = x;
    vpos.z = z;

    float h01 = input.uv.y;

    float wind_weight = h01 * h01;
    float phase = tuft_hash * 6.283185f;
    float g = gust(input.inst_pos.xz, time + phase * 0.1f);
    float w_mag = wind_strength * g;

    vpos.x += wind_dir.x * w_mag * wind_weight;
    vpos.z += wind_dir.y * w_mag * wind_weight;

    float3 world_pos = vpos + input.inst_pos;
    output.pos = mul(mvp, float4(world_pos, 1.0f));
    output.uv = input.uv;
    output.world_pos = world_pos;

    float3 sec_green = input.inst_color * float3(0.85f, 1.15f, 0.7f);
    float color_shift = frac(tuft_hash * 42.0f) * 0.15f;
    output.inst_color = lerp(input.inst_color, sec_green, color_shift);

    float3 rotated_n;
    rotated_n.x = input.normal.x * c - input.normal.z * s;
    rotated_n.y = input.normal.y;
    rotated_n.z = input.normal.x * s + input.normal.z * c;
    output.local_normal = rotated_n;

    output.h01 = h01;

    return output;
}

Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);

Texture2D shadow_map : register(t1, space2);
SamplerComparisonState shadow_cmp : register(s1, space2);
SamplerState shadow_diag : register(s2, space2);

Texture2D weather_map : register(t3, space2);   // M2.5-24G-2: o MESMO weather do ceu
SamplerState weather_samp : register(s3, space2);

// M2.5-26-2a: PRE-EXPOSICAO. Lagarde & de Rousiers, "Moving Frostbite to PBR 3.0"
// (SIGGRAPH 2014), sec. 5.2, VERBATIM: "Light buffer Float16. Exposure is
// pre-applied. [...] We use the exposure information described in Section 5.1 to
// pre-expose all our lighting before storing it. We either use a fixed exposure or
// use the exposure calculated by auto-exposure of the previous frame. [...] The
// pre-exposure is performed inside an epilogue present in all lighting shaders."
// Listing 31:  outColor0 = epilogueLighting(outColor0, g_exposureMultiplier);
// POR QUE: com o sol em lux, o disco solar vale E/Omega ~ 1e7 cd/m2 e o alvo R16F
// satura em 65504. Sem pre-exposicao as unidades fisicas nao cabem no buffer.
Texture2D tex_expo : register(t4, space2);
SamplerState samp_expo : register(s4, space2);
float expo_mul()
{
    float e = tex_expo.SampleLevel(samp_expo, float2(0.5f, 0.5f), 0).r;
    return (e > 0.0f && e < 1e6f) ? e : 1.0f; // 1o frame / textura ainda nao escrita
}


cbuffer SunBlock : register(b0, space3)
{
    float3 sun_dir;
    float ambient;
    float3 sun_color;
    float toon_cuts;
    float3 ps_cam_pos;
    float toon_soft;
    float2 wind_dir_ps;
    float fog_density;
    float cloud_time;
    float fog_color_r;
    float fog_color_g;
    float fog_color_b;
    float cloud_cover;
    float shadow_str;
    float grass_backlight;
    float grass_specular;
    float obj_shadow_str;
    float obj_shadow_bias;
    float debug_shadows;
    float pad_g1;
    float pad_g2;
    float4x4 light_vp; // alinhamento 112 da entrega (o certo — reversao do meu over-fix)
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

struct PSOutput
{
    float4 color : SV_Target0;
    float4 normal_id : SV_Target1;
};

PSOutput PSMain(PSInput input)
{
    float dist = length(ps_cam_pos - input.world_pos);
    float fog_f = 1.0f - exp(-fog_density * dist);

    float f_base = lerp(0.55f, 1.0f, fog_f);
    float f_topo = lerp(1.25f, 1.0f, fog_f);

    float3 base = input.inst_color * f_base;
    float3 topo = input.inst_color * f_topo;
    float3 albedo = lerp(base, topo, input.h01);
    albedo *= lerp(0.35f, 1.0f, input.h01);

    float3 N = normalize(lerp(normalize(input.local_normal), float3(0.0f, 1.0f, 0.0f), input.h01 * 0.6f));

    float day_gate = smoothstep(-0.02f, 0.12f, -sun_dir.y);

    if (debug_shadows > 0.5f && input.pos.x < 256.0f && input.pos.y < 256.0f)
    {
        float d = shadow_map.SampleLevel(shadow_diag, input.pos.xy / 256.0f, 0).r;
        PSOutput dbg;
        dbg.color = float4(d, d, d, 1.0f);
        dbg.normal_id = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return dbg;
    }

    // M2.5-24: Grama operando sob Sombra de Objeto
    float obj_shadow = 1.0f;
    if (day_gate > 0.0f)
    {
        float4 proj = mul(light_vp, float4(input.world_pos, 1.0f));
        float3 proj_ndc = proj.xyz / proj.w;
        float2 shadow_uv = float2(proj_ndc.x * 0.5f + 0.5f, -proj_ndc.y * 0.5f + 0.5f);
        float shadow_z = proj_ndc.z;

        if (shadow_uv.x >= 0.0f && shadow_uv.x <= 1.0f && shadow_uv.y >= 0.0f && shadow_uv.y <= 1.0f && shadow_z <= 1.0f)
        {
            float fade = smoothstep(0.0f, 0.05f, shadow_uv.x) * smoothstep(1.0f, 0.95f, shadow_uv.x) *
                         smoothstep(0.0f, 0.05f, shadow_uv.y) * smoothstep(1.0f, 0.95f, shadow_uv.y);

            float shadow_val = 0.0f;
            float2 off = 1.0f / 2048.0f;
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    shadow_val += shadow_map.SampleCmpLevelZero(shadow_cmp, shadow_uv + float2(x, y) * off, shadow_z - obj_shadow_bias);
                }
            }
            shadow_val /= 9.0f;
            obj_shadow = lerp(1.0f, lerp(1.0f, shadow_val, obj_shadow_str * day_gate), fade);
        }
    }

    // M2.5-25-6 (BUG B): Half-Lambert de Valve, na MESMA forma da basic.hlsl - o
    // saturate vem DEPOIS do wrap. Antes vinha antes, o que criava um piso de 0.5:
    // a grama ficava iluminada a >=50% em qualquer hora, sol onde estivesse.
    float t_hl = saturate(dot(N, -sun_dir) * 0.5f + 0.5f);

    // M2.5-24G-2: sombra de nuvem le o MESMO weather map do ceu (sincronia por construcao)
    float2 sh_off = wind_dir_ps * cloud_time * -200.0f;
    float sh_env = saturate((weather_map.SampleLevel(weather_samp, (input.world_pos.xz + sh_off) * 0.0002f, 0).r - 0.32f) / 0.36f);
    float sh_det = saturate((weather_map.SampleLevel(weather_samp, (input.world_pos.xz + sh_off) * 0.0015f, 0).r - 0.35f) / 0.30f);
    float sombra_de_nuvem = 1.0f - shadow_str * sh_det * saturate(cloud_cover) * lerp(0.45f, 1.0f, sh_env);

    float final_shadow = max(obj_shadow * sombra_de_nuvem, 0.12f); // piso anti-buraco-preto (dupla sombra)

    float3 lit_pre = sun_color * (ambient + (1.0f - ambient) * t_hl * final_shadow);
    float3 lit = albedo * lit_pre;

    float3 view_dir = normalize(input.world_pos - ps_cam_pos);
    float back = pow(saturate(dot(view_dir, -sun_dir)), 4.0f) * input.h01;
    lit += albedo * sun_color * back * grass_backlight * final_shadow * day_gate;

    float3 half_vec = normalize(-sun_dir + (-view_dir));
    float spec = pow(saturate(dot(N, half_vec)), 24.0f) * input.h01 * grass_specular;
    lit += sun_color * spec * final_shadow * day_gate;

    float3 final_color = lerp(lit, float3(fog_color_r, fog_color_g, fog_color_b), fog_f);

    PSOutput output;
    output.color = float4(final_color * expo_mul(), 1.0f); // 26-2a: epilogueLighting
    output.normal_id = float4(float3(0.0f, 1.0f, 0.0f) * 0.5f + 0.5f, 5.0f / 255.0f);
    return output;
}
