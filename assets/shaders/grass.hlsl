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
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f); // fade quintico: a grade some
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
    
    float wind_weight = input.uv.y * input.uv.y;
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
    
    return output;
}

Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);

// M2.5-19B: SunBlock Repacked (Alinhado a 16B - Espelho com C++ e Basic)
cbuffer SunBlock : register(b0, space3)
{
    float3 sun_dir;
    float ambient;
    float3 sun_color;
    float pad;
    float3 ps_cam_pos;
    float pad2;
    float toon_cuts;
    float toon_soft;
    float2 wind_dir_ps;
    float3 fog_color;
    float fog_density;
    float cloud_time;
    float cloud_cover;
    float shadow_str;
    float pad3;
};

float hash12w(float2 pp)
{
    return frac(sin(dot(pp, float2(127.1f, 311.7f))) * 43758.5453f);
}
float vnoise2(float2 pp)
{
    float2 i = floor(pp);
    float2 f = frac(pp);
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f); // fade quintico: a grade some
    float a = hash12w(i);
    float b = hash12w(i + float2(1, 0));
    float c = hash12w(i + float2(0, 1));
    float dd = hash12w(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, dd, u.x), u.y);
}
float wfbm(float2 pp)
{
    float2 pr = float2(pp.x * 0.8f - pp.y * 0.6f, pp.x * 0.6f + pp.y * 0.8f); // 2a octava rotacionada
    return vnoise2(pp) * 0.66f + vnoise2(pr * 2.7f) * 0.34f;
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
    float3 albedo = lerp(base, topo, input.uv.y);
    
    float3 N = float3(0.0f, 1.0f, 0.0f);
    float ndl = saturate(dot(N, -sun_dir));
    float t_hl = ndl * 0.5f + 0.5f;
    
    // M2.5-19B: Sombra da grama operando igual ao basic
    float2 sh_off = wind_dir_ps * cloud_time * -200.0f;
    float sh_env = saturate((wfbm((input.world_pos.xz + sh_off) * 0.0008f) - 0.32f) / 0.36f);
    float sh_det = saturate((wfbm((input.world_pos.xz + sh_off) * 0.006f) - 0.35f) / 0.30f);
    float shadow = 1.0f - shadow_str * sh_det * saturate(cloud_cover) * lerp(0.45f, 1.0f, sh_env); // detalhe e o astro; envelope so modula
    float3 lit_pre = sun_color * (ambient + (1.0f - ambient) * t_hl * shadow);
    
    float3 lit = albedo * lit_pre; // lerp restaurado: teto 1.0
    
    float3 final_color = lerp(lit, fog_color, fog_f);
    
    PSOutput output;
    output.color = float4(final_color, 1.0f);
    output.normal_id = float4(N * 0.5f + 0.5f, 5.0f / 255.0f);
    return output;
}