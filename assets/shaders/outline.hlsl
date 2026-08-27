// outline.hlsl - M2.5-8 + patches: crease unilateral (1px) e screen blend
// Passe fullscreen: le scene_color + normal_id + depth, escreve composite.

struct PSInput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

Texture2D tex_color : register(t0, space2);
Texture2D tex_norm_id : register(t1, space2);
Texture2D tex_depth : register(t2, space2);
SamplerState samp0 : register(s0, space2);
SamplerState samp1 : register(s1, space2);
SamplerState samp2 : register(s2, space2);

cbuffer OutlineBlock : register(b0, space3)
{
    float2 texel;
    float z_cutoff;
    float grazing_scale;
    float crease_low;
    float crease_high;
    float line_darken;
    float crease_brighten;
    float3 cam_fwd;
    float pad;
};

float4 PSMain(PSInput input) : SV_Target0
{
    // A. Extracao central
    float4 c0 = tex_color.Sample(samp0, input.uv);
    float4 nid0 = tex_norm_id.Sample(samp1, input.uv);
    float3 N0 = nid0.xyz * 2.0f - 1.0f;
    float id0 = nid0.a;
    float d0 = tex_depth.Sample(samp2, input.uv).r;

    // B. Vizinhos cardeais (1 texel)
    float2 uvL = input.uv - float2(texel.x, 0.0f);
    float2 uvR = input.uv + float2(texel.x, 0.0f);
    float2 uvT = input.uv - float2(0.0f, texel.y);
    float2 uvB = input.uv + float2(0.0f, texel.y);

    float dL = tex_depth.Sample(samp2, uvL).r;
    float dR = tex_depth.Sample(samp2, uvR).r;
    float dT = tex_depth.Sample(samp2, uvT).r;
    float dB = tex_depth.Sample(samp2, uvB).r;

    float idL = tex_norm_id.Sample(samp1, uvL).a;
    float idR = tex_norm_id.Sample(samp1, uvR).a;
    float idT = tex_norm_id.Sample(samp1, uvT).a;
    float idB = tex_norm_id.Sample(samp1, uvB).a;

    float3 nL = tex_norm_id.Sample(samp1, uvL).xyz * 2.0f - 1.0f;
    float3 nR = tex_norm_id.Sample(samp1, uvR).xyz * 2.0f - 1.0f;
    float3 nT = tex_norm_id.Sample(samp1, uvT).xyz * 2.0f - 1.0f;
    float3 nB = tex_norm_id.Sample(samp1, uvB).xyz * 2.0f - 1.0f;

    float3 cL = tex_color.Sample(samp0, uvL).rgb;
    float3 cR = tex_color.Sample(samp0, uvR).rgb;
    float3 cT = tex_color.Sample(samp0, uvT).rgb;
    float3 cB = tex_color.Sample(samp0, uvB).rgb;

    // C. Mascara de angulo rasante (ameye) - limiar modulado por
    //    profundidade e por NdotV: mata o ruido de borda no chao raso.
    float ndv = saturate(abs(dot(N0, -cam_fwd)));
    float limiar = z_cutoff * max(d0, 1e-4f) * (1.0f + grazing_scale * (1.0f - ndv));

    // D. Silhueta (borda externa, unilateral por construcao)
    float d_max = max(max(dL, dR), max(dT, dB));
    bool depth_edge = (d_max - d0) > limiar;

    float id_thresh = 0.5f / 255.0f;
    bool id_edge = (abs(idL - id0) > id_thresh) || (abs(idR - id0) > id_thresh) ||
                   (abs(idT - id0) > id_thresh) || (abs(idB - id0) > id_thresh);

    bool sil = depth_edge || id_edge;

    float d_min = dL;
    float3 cor_silhueta = cL;
    if (dR < d_min)
    {
        d_min = dR;
        cor_silhueta = cR;
    }
    if (dT < d_min)
    {
        d_min = dT;
        cor_silhueta = cT;
    }
    if (dB < d_min)
    {
        d_min = dB;
        cor_silhueta = cB;
    }

    cor_silhueta *= line_darken;

    // E. Crease (borda interna) - directional contrast + UNILATERAL:
    //    detecta como sempre (imune a curvas), mas so desenha no lado
    //    da aresta mais proximo da camera -> linha de 1px.
    float diffT = 1.0f - dot(N0, nT);
    float diffB = 1.0f - dot(N0, nB);
    float diffL = 1.0f - dot(N0, nL);
    float diffR = 1.0f - dot(N0, nR);

    float cx = abs(diffL - diffR);
    float cy = abs(diffT - diffB);

    // Bilateral (2px) com COR UNIFORME: os dois lados da aresta usam o
    // mesmo par {face propria, face do outro lado} e clareiam o mais
    // claro do par -> mesma cor nos dois pixels.
    float creaseX = smoothstep(crease_low, crease_high, cx);
    float creaseY = smoothstep(crease_low, crease_high, cy);
    float crease = max(creaseX, creaseY);

    float3 cox = (diffL > diffR) ? cL : cR;
    float3 coy = (diffT > diffB) ? cT : cB;
    float3 c_cross = (creaseX >= creaseY) ? cox : coy;
    float3 base_crease = max(c0.rgb, c_cross);

    if (sil)
        crease = 0.0f; // silhueta tem prioridade sobre crease

    // Screen blend: clareia ate superficies ja claras
    // (brighten 1.0 = neutro; 2.0 = branco puro)
    float3 cor_crease = min(base_crease * crease_brighten, 1.0f);

    // F. Composicao final
    float3 final_color = sil ? cor_silhueta : lerp(c0.rgb, cor_crease, crease);

    // G3: OUTLINE S�-EM-OBJETOS
    // Se o pixel central pertence � grama (5/255) ou ao terreno (6/255), bypass global do trace.
    bool is_grass_or_terrain = (abs(id0 - 5.0f / 255.0f) <= id_thresh) ||
                               (abs(id0 - 6.0f / 255.0f) <= id_thresh);
    if (is_grass_or_terrain)
    {
        final_color = c0.rgb;
    }

    return float4(final_color, c0.a);
}