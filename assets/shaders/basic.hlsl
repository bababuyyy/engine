struct VSInput
{
    float3 pos : TEXCOORD0; // loc 0
    float3 color : TEXCOORD1; // loc 1
    float3 inst_pos : TEXCOORD2; // loc 2
    float3 inst_color : TEXCOORD3; // loc 3
    float2 uv : TEXCOORD4; // loc 4
    float3 normal : TEXCOORD5; // loc 5
};

struct PSInput
{
    float4 pos : SV_Position;
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 world_pos : TEXCOORD2;
};

struct PSOutput
{
    float4 color : SV_Target0;
    float4 normal_id : SV_Target1;
};

// SDL3 GPU: buffers uniformes (vertex) = space1
cbuffer UniformBlock : register(b0, space1)
{
    float4x4 mvp;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 world_pos = float4(input.pos + input.inst_pos, 1.0f);
    
    output.pos = mul(mvp, world_pos);
    output.color = input.color * input.inst_color;
    output.uv = input.uv;
    
    output.normal = input.normal;
    output.world_pos = world_pos.xyz;
    
    return output;
}

Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);

// M2.5-19B: SunBlock Repacked (Alinhado a 16B - Espelho com C++ e Grass)
cbuffer SunBlock : register(b0, space3)
{
    float3 sun_dir;
    float ambient;
    float3 sun_color;
    float pad;
    float3 cam_pos;
    float pad2;
    float toon_cuts;
    float toon_soft;
    float2 wind_dir;
    float3 fog_color;
    float fog_density;
    float cloud_time;
    float cloud_cover;
    float shadow_str;
    float pad3;
};

cbuffer ObjectIdBlock : register(b1, space3)
{
    float object_id;
    float3 pad_obj;
};

// M2.5-19B: Noise helpers duplicados cirurgicamente (FXC limpo sem extends)
float hash12w(float2 pp)
{
    return frac(sin(dot(pp, float2(127.1f, 311.7f))) * 43758.5453f);
}
float vnoise2(float2 pp)
{
    float2 i = floor(pp);
    float2 f = frac(pp);
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = hash12w(i);
    float b = hash12w(i + float2(1, 0));
    float c = hash12w(i + float2(0, 1));
    float dd = hash12w(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, dd, u.x), u.y);
}
float wfbm(float2 pp)
{
    return vnoise2(pp) * 0.66f + vnoise2(pp * 2.7f) * 0.34f;
}

PSOutput PSMain(PSInput input)
{
    PSOutput output;
    
    float4 tex_color = tex.Sample(samp, input.uv);
    float3 albedo = input.color * tex_color.rgb;
    
    float3 N = normalize(input.normal);
    
    float ndl = dot(N, -sun_dir);
    float t = saturate(ndl * 0.5f + 0.5f);
    
    float scaled = t * toon_cuts;
    float band = min(floor(scaled), toon_cuts - 1.0f);
    
    float fw = fwidth(scaled) * toon_soft;
    float f = scaled - band;
    band += smoothstep(1.0f - fw, 1.0f, f);
    float ramp = band / max(toon_cuts - 1.0f, 1.0f);
    
    // M2.5-19B: Sombras de Nuvem atreladas estritamente a luz direta
    float2 sh_off = wind_dir * cloud_time * -200.0f;
    float sh_env = saturate((wfbm((input.world_pos.xz + sh_off) * 0.0008f) - 0.32f) / 0.36f);
    float sh_det = saturate((wfbm((input.world_pos.xz + sh_off) * 0.006f) - 0.35f) / 0.30f);
    float shadow = 1.0f - shadow_str * sh_det * saturate(cloud_cover) * lerp(0.45f, 1.0f, sh_env); // detalhe e o astro; envelope so modula
    float3 lit = albedo * sun_color * (ambient + (1.0f - ambient) * ramp * shadow); // lerp restaurado: teto 1.0
    
    float3 V = normalize(cam_pos - input.world_pos);
    float rim = pow(1.0f - saturate(dot(N, V)), 3.0f);
    lit += albedo * sun_color * rim * 0.25f * shadow;
    
    float dist = length(cam_pos - input.world_pos);
    float fog_f = 1.0f - exp(-fog_density * dist);
    
    float3 final_color = lerp(lit, fog_color, fog_f);
    
    output.color = float4(final_color, tex_color.a);
    output.normal_id = float4(N * 0.5f + 0.5f, object_id);
    
    return output;
}