struct VSInput
{
    float3 pos : TEXCOORD0; // loc 0
    float3 color : TEXCOORD1; // loc 1
    float3 inst_pos : TEXCOORD2; // loc 2
    float3 inst_color : TEXCOORD3; // loc 3
    float2 uv : TEXCOORD4; // loc 4
};

struct PSInput
{
    float4 pos : SV_Position;
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
};

// SDL3 GPU: buffers uniformes = space1
cbuffer UniformBlock : register(b0, space1)
{
    float4x4 mvp;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 world_pos = float4(input.pos + input.inst_pos, 1.0f);
    
    // glm::value_ptr passa matrizes column-major. O mul(vector, matrix) as interpreta como row-major, 
    // efetuando matematicamente vetor * transposta, que é exatamente o pipeline correto!
    output.pos = mul(mvp, world_pos);
    
    output.color = input.color * input.inst_color;
    output.uv = input.uv;
    return output;
}

// SDL3 GPU: buffers de imagem e samplers fragment = space2
Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);

float4 PSMain(PSInput input) : SV_Target0
{
    float4 tex_color = tex.Sample(samp, input.uv);
    return float4(input.color, 1.0f) * tex_color; // Mix perfeito de fallback
}