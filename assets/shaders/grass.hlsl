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

// Alinhamento exato de 96 bytes sem pads.
cbuffer GrassVSBlock : register(b0, space1)
{
    float4x4 mvp;
    float3 cam_pos;
    float scale;
    float2 wind_dir;
    float wind_strength;
    float time;
};

// Funcões utilitárias de Noise para o Vento
float hash12(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

float vnoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = hash12(i + float2(0.0f, 0.0f));
    float b = hash12(i + float2(1.0f, 0.0f));
    float c = hash12(i + float2(0.0f, 1.0f));
    float d = hash12(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float gust(float2 pos_xz, float t)
{
    // Escala ~0.08 e scroll ao longo do vetor do vento
    float2 p = pos_xz * 0.08f - wind_dir * t * 2.0f;
    return vnoise(p) * 0.66f + vnoise(p * 2.0f) * 0.34f;
}

PSInput VSMain(VSInput input)
{
    PSInput output;
    
    // Hash do Tufo para variações dessincronizadas
    float tuft_hash = frac(sin(dot(input.inst_pos.xz, float2(12.9898f, 78.233f))) * 43758.5453f);
    
    // Variação de altura do tufo [0.75..1.25]
    float tuft_h = lerp(0.75f, 1.25f, frac(tuft_hash * 13.37f));
    
    // Altura do Clump (recuperada recriando o Worley base pela célula 2.5) [0.8..1.2]
    float clump_hash = frac(sin(dot(floor(input.inst_pos.xz / 2.5f), float2(12.9898f, 78.233f))) * 43758.5453f);
    float clump_h = lerp(0.8f, 1.2f, clump_hash);
    
    float total_h = tuft_h * clump_h;
    
    float3 vpos = input.pos * scale;
    vpos.y *= total_h;
    
    // Estilo da lâmina: gira em Y
    float angle = tuft_hash * 6.283185f;
    float s = sin(angle);
    float c = cos(angle);
    float x = vpos.x * c - vpos.z * s;
    float z = vpos.x * s + vpos.z * c;
    vpos.x = x;
    vpos.z = z;
    
    // Vento GoT-style: desloca topo da lâmina na direção global + força oscilante + jitter de fase
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
    
    // Desvio de Matiz Secundário [0..0.15] (Var. Tufo)
    float3 sec_green = input.inst_color * float3(0.85f, 1.15f, 0.7f);
    float color_shift = frac(tuft_hash * 42.0f) * 0.15f;
    output.inst_color = lerp(input.inst_color, sec_green, color_shift);
    
    return output;
}

Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);

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
    float2 pad3;
    float3 fog_color;
    float fog_density;
};

struct PSOutput
{
    float4 color : SV_Target0;
    float4 normal_id : SV_Target1;
};

PSOutput PSMain(PSInput input)
{
    float3 base = input.inst_color * 0.55f;
    float3 topo = input.inst_color * 1.25f;
    float3 albedo = lerp(base, topo, input.uv.y);
    
    float3 N = float3(0.0f, 1.0f, 0.0f);
    float ndl = saturate(dot(N, -sun_dir));
    float t_hl = ndl * 0.5f + 0.5f;
    
    float3 lit = albedo * sun_color * lerp(ambient, 1.0f, t_hl);
    
    float dist = length(ps_cam_pos - input.world_pos);
    float fog_f = 1.0f - exp(-fog_density * dist);
    float3 final_color = lerp(lit, fog_color, fog_f);
    
    PSOutput output;
    output.color = float4(final_color, 1.0f);
    
    // Grama fixa em ID 5
    output.normal_id = float4(N * 0.5f + 0.5f, 5.0f / 255.0f);
    return output;
}