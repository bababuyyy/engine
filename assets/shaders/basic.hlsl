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
    
    // As instâncias só transladam — a normal atravessa sem rotação; 
    // quando houver rotação de instância no futuro, a normal rotaciona junto.
    output.normal = input.normal;
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
};

float4 PSMain(PSInput input) : SV_Target0
{
    float4 tex_color = tex.Sample(samp, input.uv);
    float3 albedo = input.color * tex_color.rgb;
    
    float ndl = max(dot(normalize(input.normal), -sun_dir), 0.0);
    float3 final_color = albedo * (ambient + ndl * sun_color);
    
    return float4(final_color, tex_color.a);
}