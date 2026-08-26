
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

// Modificado para acomodar o pad e a remocao do 'style'
cbuffer GrassVSBlock : register(b0, space1)
{
    float4x4 mvp;
    float3 cam_pos;
    float scale;
    float pad0;
    float3 vs_pad;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float3 vpos = input.pos * scale;
    
    // Hash deterministico estavel por instancia
    float rnd = frac(sin(dot(input.inst_pos.xz, float2(12.9898f, 78.233f))) * 43758.5453f);
    
    // Estilo C exclusivo: Cruzados ou Laminas. Gira em Y com variacao randomica
    float angle = rnd * 6.283185f;
    float s = sin(angle);
    float c = cos(angle);
    float x = vpos.x * c - vpos.z * s;
    float z = vpos.x * s + vpos.z * c;
    vpos.x = x;
    vpos.z = z;
    
    float3 world_pos = vpos + input.inst_pos;
    output.pos = mul(mvp, float4(world_pos, 1.0f));
    output.uv = input.uv;
    output.world_pos = world_pos;
    output.inst_color = input.inst_color;
    
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
    // A grama HERDA a tintura do solo processado no C++ (gradiente UV.y)
    float3 base = input.inst_color * 0.55f;
    float3 topo = input.inst_color * 1.25f;
    float3 albedo = lerp(base, topo, input.uv.y);
    
    // Iluminacao orientada ao Tufo global: Normal para cima
    float3 N = float3(0.0f, 1.0f, 0.0f);
    float ndl = saturate(dot(N, -sun_dir));
    float t_hl = ndl * 0.5f + 0.5f;
    
    float3 lit = albedo * sun_color * lerp(ambient, 1.0f, t_hl);
    
    float dist = length(ps_cam_pos - input.world_pos);
    float fog_f = 1.0f - exp(-fog_density * dist);
    float3 final_color = lerp(lit, fog_color, fog_f);
    
    PSOutput output;
    output.color = float4(final_color, 1.0f);
    
    // M2.5-15: ID 5/255 designada exlusivamente p/ grama. Evita edge outline escuro.
    output.normal_id = float4(N * 0.5f + 0.5f, 5.0f / 255.0f);
    return output;
}

