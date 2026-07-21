// space1 é obrigatório na arquitetura do SDL3 GPU para o cbuffer do Vertex Shader.
// b0 é o slot zero que alimentaremos via push no cpp.
cbuffer Constants : register(b0, space1)
{
    float4x4 mvp;
};

// As semantics POSITION e COLOR originais do D3D são invalidadas pelo SDL GPU.
// O backend exige que todos os atributos entrem como TEXCOORD0..N.
struct VSInput
{
    float3 pos : TEXCOORD0; // Geometria base (local)
    float3 color : TEXCOORD1; // Cor da geometria
    float3 inst_pos : TEXCOORD2; // Translação da instância atual
    float3 inst_color : TEXCOORD3; // Cor base da instância atual
};

struct PSInput
{
    float4 pos : SV_Position;
    float3 color : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    
    // Calcula a verdadeira posição de mundo deslocando os vértices do mesh com os dados instanciados
    float3 final_pos = input.pos + input.inst_pos;
    
    output.pos = mul(mvp, float4(final_pos, 1.0f));
    
    // Mescla a cor individual do vértice com o matiz base da instância
    output.color = input.color * input.inst_color;
    output.worldPos = final_pos;
    
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