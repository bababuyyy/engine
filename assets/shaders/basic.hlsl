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

cbuffer UniformBlock : register(b0, space1)
{
    float4x4 mvp;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 world_pos = float4(input.pos + input.inst_pos, 1.0f);
    
    output.pos = mul(mvp, world_pos);
    output.color = input.color * input.inst_color;
    output.uv = input.uv;
    
    output.normal = input.normal;
    output.world_pos = world_pos.xyz;
    
    return output;
}

Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);

Texture2D shadow_map : register(t1, space2);
SamplerComparisonState shadow_cmp : register(s1, space2);
SamplerState shadow_diag : register(s2, space2);

cbuffer SunBlock : register(b0, space3)
{
    float3 sun_dir;
    float ambient;
    float3 sun_color;
    float toon_cuts;
    float3 cam_pos;
    float toon_soft;
    float2 wind_dir;
    float fog_density;
    float cloud_time;
    float fog_color_r;
    float fog_color_g;
    float fog_color_b;
    float cloud_cover;
    float shadow_str;
    float grass_backlight;
    float grass_specular;
    float obj_shadow_str;
    float obj_shadow_bias;
    float debug_shadows;
    float pad_g1;
    float pad_g2;
    float4x4 light_vp; // alinhamento 112 da entrega (o certo — reversao do meu over-fix)
};

cbuffer ObjectIdBlock : register(b1, space3)
{
    float object_id;
    float3 pad_obj;
};

float hash12w(float2 pp)
{
    return frac(sin(dot(pp, float2(127.1f, 311.7f))) * 43758.5453f);
}
float vnoise2(float2 pp)
{
    float2 i = floor(pp);
    float2 f = frac(pp);
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float a = hash12w(i);
    float b = hash12w(i + float2(1, 0));
    float c = hash12w(i + float2(0, 1));
    float dd = hash12w(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, dd, u.x), u.y);
}
float wfbm(float2 pp)
{
    float2 pr = float2(pp.x * 0.8f - pp.y * 0.6f, pp.x * 0.6f + pp.y * 0.8f);
    return vnoise2(pp) * 0.66f + vnoise2(pr * 2.7f) * 0.34f;
}

PSOutput PSMain(PSInput input)
{
    PSOutput output;
    
    float4 tex_color = tex.Sample(samp, input.uv);
    float3 albedo = input.color * tex_color.rgb;
    float3 N = normalize(input.normal);
    
    float day_gate = smoothstep(-0.02f, 0.12f, -sun_dir.y);
    
    // M2.5-24: Modo de Diagnostico Direto da Sombra (Exclusivo na UI)
    if (debug_shadows > 0.5f && input.pos.x < 256.0f && input.pos.y < 256.0f)
    {
        float d = shadow_map.SampleLevel(shadow_diag, input.pos.xy / 256.0f, 0).r;
        output.color = float4(d, d, d, 1.0f);
        output.normal_id = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return output;
    }
    
    // M2.5-24: Computacao da Sombra Projetada (PCF 3x3 com Clamp)
    float obj_shadow = 1.0f;
    if (day_gate > 0.0f)
    {
        float4 proj = mul(light_vp, float4(input.world_pos, 1.0f));
        float3 proj_ndc = proj.xyz / proj.w;
        float2 shadow_uv = float2(proj_ndc.x * 0.5f + 0.5f, -proj_ndc.y * 0.5f + 0.5f);
        float shadow_z = proj_ndc.z;
        
        if (shadow_uv.x >= 0.0f && shadow_uv.x <= 1.0f && shadow_uv.y >= 0.0f && shadow_uv.y <= 1.0f && shadow_z <= 1.0f)
        {
            float fade = smoothstep(0.0f, 0.05f, shadow_uv.x) * smoothstep(1.0f, 0.95f, shadow_uv.x) *
                         smoothstep(0.0f, 0.05f, shadow_uv.y) * smoothstep(1.0f, 0.95f, shadow_uv.y);
            
            float shadow_val = 0.0f;
            float2 off = 1.0f / 2048.0f;
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    shadow_val += shadow_map.SampleCmpLevelZero(shadow_cmp, shadow_uv + float2(x, y) * off, shadow_z - obj_shadow_bias);
                }
            }
            shadow_val /= 9.0f;
            obj_shadow = lerp(1.0f, lerp(1.0f, shadow_val, obj_shadow_str * day_gate), fade);
        }
    }
    
    float ndl = dot(N, -sun_dir);
    float t = saturate(ndl * 0.5f + 0.5f);
    
    float scaled = t * toon_cuts;
    float band = min(floor(scaled), toon_cuts - 1.0f);
    float fw = fwidth(scaled) * toon_soft;
    float f = scaled - band;
    band += smoothstep(1.0f - fw, 1.0f, f);
    float ramp = band / max(toon_cuts - 1.0f, 1.0f);
    
    float2 sh_off = wind_dir * cloud_time * -200.0f;
    float sh_env = saturate((wfbm((input.world_pos.xz + sh_off) * 0.0008f) - 0.32f) / 0.36f);
    float sh_det = saturate((wfbm((input.world_pos.xz + sh_off) * 0.006f) - 0.35f) / 0.30f);
    float sombra_de_nuvem = 1.0f - shadow_str * sh_det * saturate(cloud_cover) * lerp(0.45f, 1.0f, sh_env);
    
    float final_shadow = max(obj_shadow * sombra_de_nuvem, 0.12f); // piso anti-buraco-preto (dupla sombra)
    
    float3 direct_light = sun_color * ramp * final_shadow;
    float3 lit = albedo * sun_color * (ambient + (1.0f - ambient) * ramp * final_shadow); // teto 1.0 restaurado (sagrada, 2a reincidencia)
    float dist = length(cam_pos - input.world_pos);
    float fog_f = 1.0f - exp(-fog_density * dist);
    
    float3 final_color = lerp(lit, float3(fog_color_r, fog_color_g, fog_color_b), fog_f);
    
    output.color = float4(final_color, tex_color.a);
    output.normal_id = float4(N * 0.5f + 0.5f, object_id);
    
    return output;
}