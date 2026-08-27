// shadow.hlsl - M2.5-24: passe depth-only do sol
struct VSInput
{
    float3 pos : TEXCOORD0;
    float3 color : TEXCOORD1;
    float3 inst_pos : TEXCOORD2;
    float3 inst_color : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float3 normal : TEXCOORD5;
};

cbuffer ShadowBlock : register(b0, space1)
{
    float4x4 light_mvp;
};

float4 VSMain(VSInput input) : SV_Position
{
    return mul(light_mvp, float4(input.pos + input.inst_pos, 1.0f));
}

void PSMain()
{
}
