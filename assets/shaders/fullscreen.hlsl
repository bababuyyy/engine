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
Texture2D tex_norm_id : register(t1, space2);
Texture2D tex_bloom : register(t2, space2); // M2.5-23: resultado da piramide dual-Kawase
SamplerState samp : register(s0, space2);

// M2.5-24H: insumos da marcha volumetrica (Wronski 2014 / Hillaire 2015 / Killzone GPU Pro 5)
Texture2D tex_depth : register(t3, space2);
SamplerState samp_point : register(s3, space2);
Texture2D shadow_map : register(t4, space2);
SamplerComparisonState shadow_cmp : register(s4, space2);
Texture2D weather_map : register(t5, space2);
SamplerState weather_samp : register(s5, space2);
Texture2D tex_expo : register(t6, space2);        // M2.5-26-4: exposicao medida (1x1)
SamplerState samp_expo : register(s6, space2);

cbuffer FsBlock : register(b0, space3)
{
    float4x4 inv_viewproj;
    float4x4 light_vp;
    float3 cam_pos;
    float shaft_intensity;
    float3 to_sun;
    float shaft_g;
    float3 sun_color;
    float shaft_samples;
    float2 tex_size;
    float2 wind_off;
    float shaft_sigma;
    float cloud_cover;
    float shadow_str;
    float bloom_intensity;
    float tonemap_shoulder; // M2.5-24I
    float exposure;         // M2.5-24I-2
    float shaft_cloud_occ;
    float cloud_mid;        // M2.5-24I-3
    float shaft_debug;      // M2.5-24I-4
    float shaft_height_falloff; // M2.5-24J (B6): ocupa o antigo pad_j0
    float dither_amt; // M2.5-25-U6d
    float expo_auto;            // M2.5-26-4: 1 = ler a exposicao da textura
};

static const float bayer[16] =
{
    0.0f / 16.0f, 8.0f / 16.0f, 2.0f / 16.0f, 10.0f / 16.0f,
    12.0f / 16.0f, 4.0f / 16.0f, 14.0f / 16.0f, 6.0f / 16.0f,
    3.0f / 16.0f, 11.0f / 16.0f, 1.0f / 16.0f, 9.0f / 16.0f,
    15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f, 5.0f / 16.0f
};

// M2.5-25-U6a: conversao EXATA de linear para sRGB.
// Lagarde & de Rousiers, "Moving Frostbite to Physically Based Rendering 3.0"
// (SIGGRAPH 2014), secao 5.1.5, Listing 30 - VERBATIM.
// A fonte manda EVITAR a aproximacao por gama 2.2: "the inaccuracy of the
// approximation for dark values [...] is critical since eyes are very sensitive
// to variations in the low range" (Fig. 75), e ela nao produz preto verdadeiro
// (Fig. 76). Nosso swapchain e B8G8R8A8_UNORM (sonda U6: formato 12, sem _SRGB),
// entao o hardware nao codifica - a codificacao e responsabilidade deste shader.
float3 accurateLinearToSRGB(float3 linearCol)
{
    float3 sRGBLo = linearCol * 12.92f;
    float3 sRGBHi = (pow(abs(linearCol), 1.0f / 2.4f) * 1.055f) - 0.055f;
    return (linearCol <= 0.0031308f) ? sRGBLo : sRGBHi;
}

float hg_phase(float cos_theta, float g)
{
    float g2 = g * g;
    return (1.0f - g2) / (pow(1.0f + g2 - 2.0f * g * cos_theta, 1.5f) * 12.5663706f);
}

float4 PSMain(PSInput input) : SV_Target0
{
    // Sharp Bilinear / Fat Pixel Upscale Base
    float2 box = clamp(fwidth(input.uv) * tex_size, 1e-5, 1.0);
    float2 tx = input.uv * tex_size - 0.5 * box;
    float2 off = smoothstep(1.0 - box, 1.0, frac(tx));
    float2 fuv = (floor(tx) + 0.5 + off) / tex_size;

    float4 final_color = tex_color.Sample(samp, fuv);

    // ========================================================================
    // M2.5-24H: SUN SHAFTS VOLUMETRICOS
    // Marcha por pixel da camera ate o depth (arquitetura minima de Wronski 2014
    // p.14-15 / linhagem Killzone Shadow Fall), com a integral conservadora de
    // energia do Hillaire 2015 (slide 28): Sint = (S - S*exp(-sigma*dt)) / sigma.
    // Sombra do sol via shadow map + sombra de nuvem via weather map: os raios
    // vazam pelos buracos das nuvens e aterrissam no mundo (paralaxe real).
    // Aditivo por decisao (v1): o fog analitico da cena permanece; unificacao
    // completa da nevoa e estacao futura (WeatherContext).
    // ========================================================================
    if (shaft_intensity > 0.001f)
    {
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

        // M2.5-24I-4: vista de depuracao - SO a marcha, sem cena/bloom/tonemap
        if (shaft_debug > 0.5f)
        {
            return float4(scat * 4.0f, 1.0f);
        }
        final_color.rgb += scat * shaft_intensity;
    }

    final_color.rgb += tex_bloom.Sample(samp, input.uv).rgb * bloom_intensity; // M2.5-23: o florescer
    // M2.5-26-4: com auto ligado, a exposicao vem MEDIDA da GPU (media log de
    // Reinhard + constante do fotometro do Frostbite). Com auto desligado, o slider
    // manual manda, exatamente como antes.
    float expo = (expo_auto > 0.5f) ? tex_expo.SampleLevel(samp_expo, float2(0.5f, 0.5f), 0).r : exposure;
    final_color.rgb *= expo; // M2.5-24I-2: exposicao antes do ombro (o par da intensidade)

    // M2.5-24I: tonemap de ombro exponencial - abaixo do ombro as cores ficam INTACTAS
    // (toon preservado); acima, rolloff filmico continuo (derivada 1 no ombro).
    float3 tc = final_color.rgb;
    float ts = tonemap_shoulder;
    float3 over = max(tc - ts, 0.0f);
    final_color.rgb = min(tc, ts) + (1.0f - ts) * (1.0f - exp(-over / max(1.0f - ts, 1e-4f)));

    // M2.5-25-U6a: a saida vira sRGB AQUI. Ordem do Frostbite (deck, Camera-Exposure):
    //   1. film stock / tone map  ->  2. style (LUT / grading)  ->  3. sRGB / Rec709
    final_color.rgb = accurateLinearToSRGB(saturate(final_color.rgb));

    // O dither de de-banding passou para DEPOIS da codificacao. Ele vale MEIO degrau
    // do buffer de 8 bits, e "meio degrau" so existe no espaco de tela. Aplicado em
    // linear (como estava), perto do preto ele valia ~6,5 niveis de 255 - era o
    // granulado que aparecia so nas areas escuras.
    // M2.5-25-U6c: o dither vive na grade do FAT PIXEL, nao na do monitor.
    // Este passe roda na resolucao da JANELA e amostra a cena de tex_size (640x360)
    // pelo upscale sharp-bilinear. Dentro de um mesmo fat pixel a cor e constante,
    // entao um dither por pixel de tela nao quebra banda nenhuma - ele so deposita
    // ruido por cima do bloco, e fica preso na tela quando a camera anda.
    // A banda acontece ENTRE texels da cena; e la que o dither precisa variar.
    // Precedente da casa: 24J-2c - "o jitter cobre o quantum" que ele disfarca.
    float2 dpx = floor(input.uv * tex_size);
    float dth = frac(sin(dot(dpx, float2(12.9898f, 78.233f))) * 43758.5453f);
    final_color.rgb += (dth - 0.5f) * dither_amt / 255.0f; // 25-U6d: forca no painel (0 = desligado)
    return final_color;
}
