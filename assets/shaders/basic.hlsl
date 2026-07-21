// space1 é obrigatório na arquitetura do SDL3 GPU para o cbuffer do Vertex Shader.
// b0 é o slot zero que alimentaremos via push no cpp.
cbuffer Constants : register(b0, space1)
{
    float4x4 mvp;
};

struct VSInput
{
    float3 pos : TEXCOORD0;
    float3 color : TEXCOORD1;
};

struct PSInput
{
    float4 pos : SV_Position;
    float3 color : COLOR;
    float3 worldPos : TEXCOORD0; // Repassamos a posição original de mundo para textura procedural
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.pos = mul(mvp, float4(input.pos, 1.0f));
    output.color = input.color;
    output.worldPos = input.pos;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    float3 col = input.color;
    
    // Grid procedural no chão: criamos linhas a cada 1.0 unidades nos eixos X e Z
    // Apenas se a geometria estiver efetivamente na altura Y=0 do chão.
    if (abs(input.worldPos.y) < 0.001f)
    {
        float2 grid = frac(input.worldPos.xz);
        if (grid.x < 0.05f || grid.y < 0.05f)
        {
            col *= 0.5f; // Escurece as regiões correspondentes às "linhas"
        }
    }
    
    return float4(col, 1.0f);
}