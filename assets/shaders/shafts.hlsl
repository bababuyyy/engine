// ============================================================================
// M2.5-26-25: OS SUN SHAFTS SAEM DO FULLSCREEN E ENTRAM NO COMPOSITE.
//
// O DEFEITO, achado pela 26-23d e medido no proprio codigo. A marcha vivia
// dentro do PSMain do fullscreen.hlsl, que roda DEPOIS de tudo que mede. O
// comentario do main.cpp diz a ordem, verbatim:
//     "composite_color -> luma_map_tex ... Roda DEPOIS do outline (composite
//      pronto) e ANTES do fullscreen"
// Consequencia: os shafts eram invisiveis para CINCO consumidores -
//   medidor de exposicao (le composite_color)        -> NAO via
//   bloom, PS_Threshold (le composite_color)         -> NAO via
//   B do operador local, PS_Down (composite + bloom) -> NAO via
//   grade bilateral, PS_Grid (composite + bloom)     -> NAO via
//   [MEDIDA]/[GRADE]/[OPER]                          -> NAO viam
// - e visiveis para dois: o Ii do operador (que le final_color) e a tela.
// Somados com multiplicador 2.0, em unidade absoluta.
//
// O QUE ISSO SIGNIFICAVA: a exposicao, o bloom, o operador local e a grade
// estavam todos julgando uma imagem DIFERENTE da que aparecia na tela. E o
// operador local com d = 1.0 nao podia sequer comprimi-los, porque o ganho
// dele depende so de B - e os shafts nao estavam em B.
//
// A DOUTRINA MANDA, e ela esta escrita no mapa-do-projeto: "Adiar gosto, nunca
// adiar instrumento - se um defeito muda o que voce ve de TODO O RESTO, ele
// entra agora." Este e exatamente o caso nomeado.
//
// A MARCHA NAO MUDOU UMA LINHA. Ela foi extraida do fullscreen.hlsl por script
// e colada aqui verbatim - mesma integral, mesmas constantes, mesmo bayer,
// mesma fase de Henyey-Greenstein. O que muda e QUANDO ela roda.
//
// A EXPOSICAO NAO MUDA: o expo_mul() sempre leu o expo_prev (t6 no fullscreen,
// rotulado "a MESMA exposicao que pre-expos a cena"), e o expo_prev ja existe
// muito antes deste passe. Os shafts continuam pre-expostos pelo mesmo numero
// que pre-expos o composite - que e a condicao para eles poderem ser SOMADOS
// nele sem quebrar a unidade.
//
// A VISTA DE DEPURACAO sobrevive, por outro mecanismo. Antes, o modo 3 fazia
// 'return scat * 4.0' dentro do PSMain, pulando cena, bloom e tonemap. Agora o
// passe usa load_op = CLEAR quando view_mode == 3: o composite passa a conter
// SO os shafts, e o resto do pipeline roda em cima deles.
// MUDANCA DECLARADA: a vista agora passa pelo tonemap, que antes ela pulava. Em
// troca, os shafts viraram MENSURAVEIS - com o modo 3 ligado, o [MEDIDA]
// reporta a marcha em cd/m2, coisa que nenhum instrumento fazia.
//
// CBUFFER PROPRIO, e nao o FsParams: o FsParams so e preenchido dezenas de
// linhas depois deste passe, e reordenar aquele bloco seria mexer em codigo que
// funciona por uma razao que nao e a desta estacao. Este bloco carrega os 52
// floats que a marcha usa, e nada mais.
// ============================================================================

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

Texture2D tex_depth : register(t0, space2);
SamplerState samp_point : register(s0, space2);
Texture2D shadow_map : register(t1, space2);
SamplerComparisonState shadow_cmp : register(s1, space2);
Texture2D weather_map : register(t2, space2);
SamplerState weather_samp : register(s2, space2);
Texture2D tex_expo : register(t3, space2);
SamplerState samp_expo : register(s3, space2);

// Os nomes dos campos sao os MESMOS do FsBlock de proposito: assim o corpo da
// marcha entra verbatim, sem uma unica renomeacao. Um campo renomeado seria um
// lugar onde a copia poderia divergir do original em silencio.
cbuffer ShaftBlock : register(b0, space3)
{
    float4x4 inv_viewproj;
    float4x4 light_vp;
    float3 cam_pos;    float shaft_intensity;
    float3 to_sun;     float shaft_g;
    float3 sun_color;  float shaft_samples;
    float2 wind_off;   float shaft_sigma; float shaft_height_falloff;
    float shaft_cloud_occ; float cloud_mid; float cloud_cover; float shaft_pad0;
};

static const float bayer[16] =
{
    0.0f / 16.0f, 8.0f / 16.0f, 2.0f / 16.0f, 10.0f / 16.0f,
    12.0f / 16.0f, 4.0f / 16.0f, 14.0f / 16.0f, 6.0f / 16.0f,
    3.0f / 16.0f, 11.0f / 16.0f, 1.0f / 16.0f, 9.0f / 16.0f,
    15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f, 5.0f / 16.0f
};

float hg_phase(float cos_theta, float g)
{
    float g2 = g * g;
    return (1.0f - g2) / (pow(1.0f + g2 - 2.0f * g * cos_theta, 1.5f) * 12.5663706f);
}

float expo_mul()
{
    float e = tex_expo.SampleLevel(samp_expo, float2(0.5f, 0.5f), 0).r;
    return (e > 0.0f && e < 1e6f) ? e : 1.0f;
}

float4 PSMain(PSInput input) : SV_Target0
{
    if (shaft_intensity <= 0.001f) { return float4(0.0f, 0.0f, 0.0f, 1.0f); }

    float depth = tex_depth.SampleLevel(samp_point, input.uv, 0).r;
    float4 ndc = float4(input.uv.x * 2.0f - 1.0f, -(input.uv.y * 2.0f - 1.0f), depth, 1.0f);
    float4 wp = mul(inv_viewproj, ndc);
    float3 world = wp.xyz / max(abs(wp.w), 1e-6f) * sign(wp.w);

    float3 ray = world - cam_pos;
    float dist = min(length(ray), 480.0f);
    float3 rdir = normalize(ray);

    int n = clamp((int) shaft_samples, 4, 32);
    float step_len = dist / n;

    int2 px = int2(input.pos.xy) % 4;
    float t = step_len * bayer[px.y * 4 + px.x];

    float phase_v = hg_phase(dot(rdir, to_sun), shaft_g);

    float T = 1.0f;
    float3 scat = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (int i = 0; i < n; ++i)
    {
        float3 p = cam_pos + rdir * t;

        // Visibilidade do sol: shadow map (fora do mapa = iluminado)
        float vis = 1.0f;
        float4 lp = mul(light_vp, float4(p, 1.0f));
        float3 lndc = lp.xyz / max(abs(lp.w), 1e-6f);
        float2 suv = float2(lndc.x * 0.5f + 0.5f, -lndc.y * 0.5f + 0.5f);
        if (suv.x >= 0.0f && suv.x <= 1.0f && suv.y >= 0.0f && suv.y <= 1.0f && lndc.z <= 1.0f)
        {
            vis = shadow_map.SampleCmpLevelZero(shadow_cmp, suv, lndc.z - 0.0015f);
        }

        // M2.5-24I-3: a nuvem que tapa este ponto NAO e a de cima dele - e a que esta
        // ENTRE ele e o sol. Projetar a amostra ate a camada na direcao solar.
        float sun_y = max(to_sun.y, 0.06f);
        float t_layer = clamp((cloud_mid - p.y) / sun_y, 0.0f, 4000.0f);
        float2 cpos = p.xz + to_sun.xz * t_layer;
        float cov = saturate((weather_map.SampleLevel(weather_samp, (cpos + wind_off) * 0.0002f, 0).r - 0.22f) / 0.55f);
        // M2.5-24I-5: presenca = onde a densidade NASCE (cobertura x campo), mapeada
        // quase-binaria (pesquisa: T ~ 0.02-0.1 sob nuvem; media global e anti-padrao)
        float presenca = smoothstep(0.12f, 0.30f, saturate(cloud_cover) * cov);
        vis *= 1.0f - shaft_cloud_occ * presenca;

        // Integral conservadora de energia (Hillaire 2015, verbatim adaptado)
        // M2.5-24J (B6): a densidade do ar cai exponencialmente com a ALTURA (Wronski 2014, sl.36-37):
        // o raio nasce denso perto do chao e morre antes de virar veu no ceu. falloff 0 = ar uniforme.
        float sigma_h = shaft_sigma * exp(-max(p.y, 0.0f) * shaft_height_falloff);
        float3 S = sun_color * (phase_v * sigma_h * vis);
        float3 Sint = (S - S * exp(-sigma_h * step_len)) / max(sigma_h, 1e-5f);
        scat += T * Sint;
        T *= exp(-sigma_h * step_len);

        t += step_len;
    }

    // 26-2a: os shafts nascem em unidade absoluta; pre-expoe aqui, com o MESMO
    // expo_prev que pre-expos o composite em que eles vao ser somados.
    return float4(scat * shaft_intensity * expo_mul(), 1.0f);
}
