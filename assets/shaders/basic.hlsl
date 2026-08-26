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
    
    // ATENÇÃO: Nunca inverter essa ordem com matrizes do GLM
    output.pos = mul(mvp, world_pos);
    
    output.color = input.color * input.inst_color;
    output.uv = input.uv;
    
    output.normal = input.normal;
    output.world_pos = world_pos.xyz;
    
    return output;
}

// SDL3 GPU: buffers de imagem e samplers (fragment) = space2
Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);

// SDL3 GPU: buffers uniformes (fragment) = space3
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
    float2 pad3;
    float3 fog_color;
    float fog_density;
};

cbuffer ObjectIdBlock : register(b1, space3)
{
    float object_id;
    float3 pad_obj;
};

PSOutput PSMain(PSInput input)
{
    PSOutput output;
    
    float4 tex_color = tex.Sample(samp, input.uv);
    float3 albedo = input.color * tex_color.rgb;
    
    float3 N = normalize(input.normal);
    
    // Toon Pass M2.5-9: Banding com Anti-Aliasing analítico nas transições
    float ndl = dot(N, -sun_dir);
    float t = saturate(ndl * 0.5f + 0.5f);
    
    float scaled = t * toon_cuts;
    float band = min(floor(scaled), toon_cuts - 1.0f);
    
    float fw = fwidth(scaled) * toon_soft;
    float f = scaled - band;
    band += smoothstep(1.0f - fw, 1.0f, f);
    
    float ramp = band / max(toon_cuts - 1.0f, 1.0f);
    
    float3 lit = albedo * sun_color * lerp(ambient, 1.0f, ramp);
    
    float3 V = normalize(cam_pos - input.world_pos);
    float rim = pow(1.0f - saturate(dot(N, V)), 3.0f);
    
    lit += albedo * sun_color * rim * 0.25f;
    
    // M2.5-11: Aplicacao autonoma da Distancia Atmosferica (Fog Integrado)
    float dist = length(cam_pos - input.world_pos);
    float fog_f = 1.0f - exp(-fog_density * dist);
    
    float3 final_color = lerp(lit, fog_color, fog_f);
    
    output.color = float4(final_color, tex_color.a);
    output.normal_id = float4(N * 0.5f + 0.5f, object_id);
    
    return output;
}