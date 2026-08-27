struct PSInput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    
    // Geracao processual otimizada do triangulo unificado para Fullscreen Quad
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    
    return output;
}

Texture2D tex_color : register(t0, space2);
Texture2D tex_norm_id : register(t1, space2); // Adicionado para servir de gate oclusor pros God Rays
Texture2D tex_bloom : register(t2, space2); // M2.5-23: resultado da piramide dual-Kawase
SamplerState samp : register(s0, space2);

cbuffer FsBlock : register(b0, space3)
{
    float2 tex_size;
    float2 sun_screen;
    float3 ray_color;
    float ray_intensity;
    float ray_decay;
    float ray_samples;
    float bloom_intensity;
    float pad_r;
};

float4 PSMain(PSInput input) : SV_Target0
{
    // Sharp Bilinear / Fat Pixel Upscale Base
    float2 box = clamp(fwidth(input.uv) * tex_size, 1e-5, 1.0);
    float2 tx = input.uv * tex_size - 0.5 * box;
    float2 off = smoothstep(1.0 - box, 1.0, frac(tx));
    float2 fuv = (floor(tx) + 0.5 + off) / tex_size;
    
    float4 final_color = tex_color.Sample(samp, fuv);
    
    // M2.5-20: God Rays Aditivos via Radial Blur Screen-Space
    // Somente opera se os raios n�o estiverem desbotados de vez (costas viradas pro sol)
    if (ray_intensity > 0.001f)
    {
        float2 texcoord = input.uv;
        float2 delta = (texcoord - sun_screen) * (1.0f / ray_samples);
        float illumination_decay = 1.0f;
        float3 ray_accum = float3(0.0f, 0.0f, 0.0f);
        
        int n_samples = (int) ray_samples;
        
        for (int i = 0; i < n_samples; ++i)
        {
            texcoord -= delta;
            
            // Guard Band
            if (texcoord.x < 0.0f || texcoord.x > 1.0f || texcoord.y < 0.0f || texcoord.y > 1.0f)
            {
                break;
            }
            
            // Sampleia cor e id do ponto do blur no buffer atual (sem offsets afiados p/ n gerar artefatos no espaco continuo)
            float4 sample_col = tex_color.SampleLevel(samp, texcoord, 0);
            float sample_id = tex_norm_id.SampleLevel(samp, texcoord, 0).a;
            
            // Sky-Mask Oclusion sem sujar a geometria
            // Se ID > 0.5/255 -> objeto/terreno (oclusor, logo intensidade 0), se 0 -> ceu (emissor de luz, extrai a luma)
            float emissor = (sample_id > (0.5f / 255.0f)) ? 0.0f : 1.0f;
            float luminance = dot(sample_col.rgb, float3(0.299f, 0.587f, 0.114f));
            
            ray_accum += sample_col.rgb * luminance * emissor * illumination_decay;
            illumination_decay *= ray_decay;
        }
        
        final_color.rgb += (ray_accum / ray_samples) * ray_color * ray_intensity;
    }

    final_color.rgb += tex_bloom.Sample(samp, input.uv).rgb * bloom_intensity; // M2.5-23: o florescer
    float dth = frac(sin(dot(input.uv, float2(12.9898f, 78.233f))) * 43758.5453f);
    final_color.rgb += (dth - 0.5f) / 255.0f; // de-banding do 8-bit (estagio final, padrao HDRP)
    return final_color;
}