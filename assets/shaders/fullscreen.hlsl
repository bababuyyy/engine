struct PSInput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    
    // Geracao processual otimizada do triangulo unificado para Fullscreen Quad
    // Cobre as coordenadas clip -1.0 a 1.0 (UV map mapeado inversamente para raster)
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    
    return output;
}

Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);

// M2.5-6: Uniforme de resolucao interna injetado dinamicamente
cbuffer FsBlock : register(b0, space3)
{
    float2 tex_size;
    float2 pad;
};

float4 PSMain(PSInput input) : SV_Target0
{
    // Sharp Bilinear / Fat Pixel Upscale
    float2 box = clamp(fwidth(input.uv) * tex_size, 1e-5, 1.0);
    float2 tx = input.uv * tex_size - 0.5 * box;
    float2 off = smoothstep(1.0 - box, 1.0, frac(tx));
    float2 fuv = (floor(tx) + 0.5 + off) / tex_size;
    
    // Amostra usando as coordenadas corrigidas (exige sampler LINEAR no host)
    return tex.Sample(samp, fuv);
}