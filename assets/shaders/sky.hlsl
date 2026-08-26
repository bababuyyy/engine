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
    
    // MVP recebe translacao mas ignora rotacao (domo fixo na camera)
    output.pos = mul(mvp, float4(input.pos, 1.0f));
    
    // Anti-clip: projeta o domo infinitamente longe (na borda da Far Plane)
    output.pos.z = output.pos.w * 0.9999f;
    
    // Vetor cru de direcao direto da posicao local
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
};

struct PSOutput
{
    float4 color : SV_Target0;
    float4 normal_id : SV_Target1;
};

PSOutput PSMain(PSInput input)
{
    // Normalizacao exata efetuada no estagio do fragmento
    float3 dir = normalize(input.dir);
    
    float h = saturate(dir.y);
    float3 ceu = lerp(horizon_color, zenith_color, pow(h, 0.6f));
    
    // To_sun direto evita os flips e negatividades cruzadas
    float s = dot(dir, to_sun);
    
    ceu += sun_color * smoothstep(0.9985f, 0.9995f, s); // disco solar
    ceu += sun_color * 0.15f * pow(saturate(s), 8.0f); // glow (halo atm)
    
    PSOutput output;
    output.color = float4(ceu, 1.0f);
    output.normal_id = float4(0.0f, 0.0f, 0.0f, 0.0f); // Neutro (para nao intervir no Outline)
    
    return output;
}