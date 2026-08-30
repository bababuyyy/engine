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
// M2.5-26-14: a log-luminancia local, ja borrada, em 64x32 (Patry 2021, slide 140)
Texture2D tex_local : register(t7, space2);
SamplerState samp_local : register(s7, space2);
// M2.5-26-23a: o atlas 256x96 da grade bilateral, so para o modo de Vista novo.
Texture2D tex_grid : register(t8, space2);
SamplerState samp_grid : register(s8, space2);

// M2.5-26-14: upsample BICUBICO B-SPLINE em 4 amostras bilineares. A fonte,
// slide 140, VERBATIM: "We then upsampled this to 256x128 using bicubic B-spline
// filtering, which avoided bilinear filtering artifacts, and added even more blur."
float amostra_bspline(float2 uv, float2 tam)
{
    float2 p = uv * tam - 0.5f;
    float2 f = frac(p);
    p = floor(p);
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0f / 6.0f) * (-f3 + 3.0f * f2 - 3.0f * f + 1.0f);
    float2 w1 = (1.0f / 6.0f) * (3.0f * f3 - 6.0f * f2 + 4.0f);
    float2 w2 = (1.0f / 6.0f) * (-3.0f * f3 + 3.0f * f2 + 3.0f * f + 1.0f);
    float2 w3 = (1.0f / 6.0f) * f3;
    float2 s0 = w0 + w1;
    float2 s1 = w2 + w3;
    float2 o0 = w1 / max(s0, 1e-6f) - 1.0f;
    float2 o1 = w3 / max(s1, 1e-6f) + 1.0f;
    float2 t0 = (p + o0 + 0.5f) / tam;
    float2 t1 = (p + o1 + 0.5f) / tam;
    float a = tex_local.SampleLevel(samp_local, float2(t0.x, t0.y), 0).r;
    float b = tex_local.SampleLevel(samp_local, float2(t1.x, t0.y), 0).r;
    float c = tex_local.SampleLevel(samp_local, float2(t0.x, t1.y), 0).r;
    float d = tex_local.SampleLevel(samp_local, float2(t1.x, t1.y), 0).r;
    return lerp(lerp(a, b, s1.x), lerp(c, d, s1.x), s1.y);
}
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
    float tonemap_modo;     // M2.5-26-11: 0 = ombro exponencial da 24I, 1 = Khronos PBR Neutral
    float shaft_cloud_occ;
    float cloud_mid;        // M2.5-24I-3
    float shaft_debug;      // M2.5-24I-4
    float shaft_height_falloff; // M2.5-24J (B6): ocupa o antigo pad_j0
    float dither_amt; // M2.5-25-U6d
    float expo_auto;            // M2.5-26-4: 1 = ler a exposicao da textura
    float lt_on;            // M2.5-26-14: operador LOCAL ligado (0/1)
    float lt_c;             // escala de contraste  (slide 137)
    float lt_d;             // forca do detalhe     (slide 137)
    float lt_m;             // ponto medio em log   (slide 137) - FALLBACK desde a 26-15
    float lt_c_lo;          // M2.5-26-15: contraste do lado BAIXO (slide 145)
    // M2.5-26-23c: o eixo Z da grade, para o slicing. Ocupam os tres pads que ja
    // existiam - ZERO mudanca de layout no FsParams.
    float bg_sr;            // sigma_r em ln (Durand & Dorsey 2002: 0.921)
    float bg_lo;            // sigma_r abaixo de M
    float bg_hi;            // sigma_r acima de M
    // O bloco cresce de 68 para 72 floats (288 B, multiplo de 16) para caber o
    // peso da mistura. Patry slide 140 publica 0.40, mas 0.40 sozinho nao deixa
    // ver se a grade funcionou - e preciso poder varrer o intervalo inteiro.
    float bg_mix;           // peso da grade no blend (slide 140: 0.40)
    float bg_pad0;
    float bg_pad1;
    float bg_pad2;
    // M2.5-26-24: a matriz de adaptacao cromatica, linha por linha. NOVE ESCALARES,
    // nao um float3x3 nem um array: matriz em cbuffer tem ordem (row/column major)
    // que depende de flag do compilador, e cada elemento de ARRAY em cbuffer come um
    // registrador de 16 bytes inteiro. Escalar empacota 4 por registrador e casa,
    // byte a byte, com o 'float wb_m[9]' do C++. A guarda confere os dois lados.
    float wb_m0; float wb_m1; float wb_m2;   // linha 0
    float wb_m3; float wb_m4; float wb_m5;   // linha 1
    float wb_m6; float wb_m7; float wb_m8;   // linha 2
    float wb_pad0;
    float wb_pad1;
    float wb_pad2;
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
// M2.5-26-2a: a exposicao usada NESTE quadro (a do quadro anterior, medida na GPU).
// Ela ja foi aplicada nos shaders de iluminacao (epilogo do Listing 31), entao aqui
// ela serve so para pre-expor o que NASCE neste passe: os shafts volumetricos.
float expo_mul()
{
    float e = tex_expo.SampleLevel(samp_expo, float2(0.5f, 0.5f), 0).r;
    return (e > 0.0f && e < 1e6f) ? e : 1.0f;
}

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
    // ========================================================================
    // M2.5-26-I: MEDIDORES. Sem readback, sem numero na tela: o valor sai
    // CODIFICADO no nivel de cinza, e a screenshot vira a leitura. Retorno
    // ANTES de tudo (tonemap, sRGB, dither), entao o que o buffer recebe e o
    // proprio numero. O motivo de existirem: ha quatro estacoes seguidas em que
    // eu conserto sem enxergar a exposicao, e o "Exposicao efetiva" do painel
    // ainda mostra o estimador analitico velho - o unico numero na tela e
    // justamente o que nao vale mais.
    // ========================================================================
    if (shaft_debug > 1.5f && shaft_debug < 2.5f)
    {
        // MEDIDOR 1: a exposicao MEDIDA, como EV100. Tela toda de um cinza so.
        // EV100 = log2(1 / (1.2 * exposure))    (Listing 28 invertido)
        // Leitura: EV100 = (nivel_de_cinza / 255) * 32 - 6
        float e_m = tex_expo.SampleLevel(samp_expo, float2(0.5f, 0.5f), 0).r;
        float ev = log2(1.0f / max(1.2f * e_m, 1e-30f));
        float g = saturate((ev + 6.0f) / 32.0f);
        return float4(g, g, g, 1.0f);
    }
    if (shaft_debug > 3.5f)
    {
        // ====================================================================
        // M2.5-26-23a: MEDIDOR 3 - A GRADE BILATERAL, CRUA.
        // Mostra o atlas 256x96: 4 tiles x 3 = as 12 fatias Z, da mais ESCURA
        // (canto superior esquerdo) para a mais CLARA (canto inferior direito).
        //   CINZA    = B da celula, normalizado por (B - lo) / (hi - lo)
        //   VERMELHO = celula VAZIA (wsum ~ 0) - nenhuma amostra caiu nessa caixa
        //
        // O QUE PROCURAR. A faixa do horizonte tem que aparecer como uma LINHA
        // CLARA em uma das fatias de baixo (as mais brilhantes) e estar AUSENTE
        // das de cima. E isso que prova que a grade separa por luminancia - que
        // e exatamente o que o B do PS_Down nao faz. Se a faixa aparecer borrada
        // em todas as fatias, a grade nao esta separando nada e ligar ela nao
        // adiantaria. Muito vermelho = o eixo Z esta largo demais para a cena, e
        // o remedio sao os sliders 'Grade: Z abaixo/acima de M'.
        // ====================================================================
        float4 g4 = tex_grid.SampleLevel(samp_grid, input.uv, 0);
        if (g4.g < 1e-4f) return float4(0.55f, 0.0f, 0.0f, 1.0f);
        float2 em = tex_expo.SampleLevel(samp_expo, float2(0.5f, 0.5f), 0).rg;
        bool ok_g = (em.r > 0.0f && em.r < 1e6f && em.g > 1e-5f && em.g < 6e4f);
        float Mg = ok_g ? log(em.g * em.r) : 0.0f;
        float log_g = Mg - 3.0f * 0.921f;
        float hig_g = Mg + 9.0f * 0.921f;
        float Bg = g4.r / max(g4.g, 1e-6f);
        float gg = saturate((Bg - log_g) / max(hig_g - log_g, 1e-4f));
        return float4(gg, gg, gg, 1.0f);
    }
    if (shaft_debug > 2.5f)
    {
        // MEDIDOR 2: a luminancia ABSOLUTA de cada pixel, em log10(cd/m2).
        // A cena chega pre-exposta, entao desfazemos a pre-exposicao primeiro.
        // Leitura: log10(cd/m2) = (nivel_de_cinza / 255) * 8 - 2
        //   preto = 0.01 cd/m2 ... branco = 1.000.000 cd/m2
        float e_m = tex_expo.SampleLevel(samp_expo, float2(0.5f, 0.5f), 0).r;
        e_m = (e_m > 0.0f && e_m < 1e6f) ? e_m : 1.0f;
        float3 c = tex_color.SampleLevel(samp, input.uv, 0).rgb / e_m;
        float L = dot(max(c, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
        float g = saturate((log10(max(L, 1e-8f)) + 2.0f) / 8.0f);
        return float4(g, g, g, 1.0f);
    }
    // ========================================================================
    // M2.5-26-18: O "SHARP BILINEAR / FAT PIXEL UPSCALE" MORREU AQUI.
    // Ele remapeava a UV para travar a amostragem no centro do texel, com uma
    // transicao estreita por smoothstep - o que dava a borda DURA do pixel-art
    // em vez do borrao do bilinear puro. Era a peca que fazia o estilo, e nao a
    // resolucao sozinha.
    // Com a resolucao interna igual a da janela, o remap deixa de ter sentido:
    // um texel da fonte passa a valer um pixel da tela, e travar no centro do
    // texel viraria uma operacao nula, cara e com fwidth() no meio.
    // FICA REGISTRADO PARA VOLTAR: o bloco original esta no backup desta estacao
    // e a direcao de arte pode retoma-lo a qualquer momento junto com a escada
    // de resolucao, que continua no painel.
    // ========================================================================
    float4 final_color = tex_color.Sample(samp, input.uv);

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
    // M2.5-26-25: A MARCHA DOS SHAFTS SAIU DAQUI.
    // Ela vivia neste ponto do PSMain, que roda DEPOIS de tudo que mede. O
    // comentario do main.cpp da a ordem, verbatim: "composite_color ->
    // luma_map_tex ... Roda DEPOIS do outline (composite pronto) e ANTES do
    // fullscreen". Consequencia medida na 26-23d: os shafts eram invisiveis
    // para o medidor de exposicao, para o bloom, para o B do operador local,
    // para a grade bilateral e para os tres relatorios - e visiveis so para o
    // Ii do operador e para a tela. A exposicao e o operador julgavam uma
    // imagem diferente da que aparecia.
    // Agora a marcha e um passe proprio (assets/shaders/shafts.hlsl) que soma
    // no composite_color ANTES do medidor. O codigo dela nao mudou uma linha:
    // foi extraido daqui por script e colado la verbatim.
    // O modo 3 da Vista (depuracao) sobrevive pelo load_op = CLEAR daquele
    // passe, e agora e MENSURAVEL - o [MEDIDA] reporta a marcha em cd/m2.
    // ========================================================================


    final_color.rgb += tex_bloom.Sample(samp, input.uv).rgb * bloom_intensity; // M2.5-23: o florescer
    // M2.5-26-2a: NAO ha mais multiplicacao por exposicao aqui. A fonte e explicita:
    // "With pre-exposure there is no need to apply exposure at the end of the
    // pipeline" (Frostbite 5.2). A cena, o ceu e a grama ja saem pre-expostos do
    // epilogo; os shafts foram pre-expostos acima; o bloom vem do buffer ja exposto.
    // Consequencia declarada: o "Bloom: Limiar" passa a operar sobre valores EXPOSTOS
    // (antes operava sobre absolutos e ignorava a exposicao). E mais correto, e e o
    // unico slider que muda de significado nesta estacao - recalibrar uma vez.

    // ========================================================================
    // M2.5-26-14: O OPERADOR LOCAL, ANTES da curva global.
    // Patry, SIGGRAPH 2021, slide 137, VERBATIM:
    //   Io = c * (B - M) + d * (Ii - B) + M
    // Ii = log-luminancia deste pixel; B = a media LOCAL borrada; M = o ponto medio.
    // Aplicamos o resultado como um GANHO sobre o RGB - exp(Io - Ii) - em vez de
    // reconstruir a cor a partir da luminancia. Isso preserva matiz e saturacao por
    // construcao, que e a preocupacao que a fonte levanta no slide 130 ("wanted to
    // avoid losing color") e que o PBR Neutral tambem resolve por outro caminho.
    // ORDEM: local primeiro, curva global depois. O local reduz a AMPLITUDE que
    // chega na curva; a curva so precisa lidar com o que sobrou.
    if (lt_on > 0.5f)
    {
        // M2.5-26-15 (C): o ponto medio vem da MEDIDA, nao da regua.
        // O canal G da textura 1x1 leva a media aparada do quadro (em cd/m2) e o
        // canal R leva a exposicao aplicada. O produto e a media EXPOSTA - a mesma
        // unidade em que B vive. Se a textura nao estiver pronta, cai no lt_m da CPU.
        float2 e_med = tex_expo.SampleLevel(samp_expo, float2(0.5f, 0.5f), 0).rg;
        bool  med_ok = (e_med.r > 0.0f && e_med.r < 1e6f && e_med.g > 1e-5f && e_med.g < 6e4f);
        float M = med_ok ? log(e_med.g * e_med.r) : lt_m;

        float lum = dot(max(final_color.rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
        float Ii = log(max(lum, 1e-5f));
        // ====================================================================
        // M2.5-26-23c: O 'B' PASSA A PODER VIR DA GRADE BILATERAL.
        //
        // POR QUE. O B antigo vem do PS_Down, media de blocos de ~40x44 pixels
        // feita ANTES de qualquer bilateral existir - logo ele nunca passa da
        // media do bloco e a faixa fina do horizonte e invisivel para ele.
        // MEDIDO no atlas da 26-23b, quadro das 5h50: a grade distribui as 2048
        // celulas por TODAS as 12 fatias (733 941 945 895 567 299 218 218 168
        // 95 58 19) com o B por fatia subindo monotonicamente de -4.7 a +9.3
        // stops em torno de M - e o pico do quadro, a +9.7 stops, cai na fatia
        // de cima. A grade poe o B EM CIMA do pico. E isso que o B antigo nao faz.
        //
        // O SLICING, Patry slide 136 VERBATIM: "Bilaterally filtered value for a
        // pixel obtained by trilinearly sampling grid and normalizing result".
        // Fazemos bilinear no plano (hardware) e a interpolacao em Z na mao: sao
        // DOIS lookups em vez de oito, que e exatamente a razao pela qual Chen et
        // al. guardam a grade como textura 2D com as fatias lado a lado.
        //
        // O GRAMPO DE MEIO TEXEL nas bordas do tile nao e detalhe: sem ele a
        // filtragem bilinear do hardware sangra entre fatias Z vizinhas, que
        // estao coladas no atlas. E o unico risco que o layout 2D introduz.
        //
        // A MISTURA CONTINUA SENDO A DA FONTE. Patry slide 140 VERBATIM: "Blend
        // between bilateral filter and very wide 2D Gaussian" - 40% bilateral,
        // 60% gaussiana. Aqui o lerp e com o B ANTIGO, que e a gaussiana larga.
        // APROXIMACAO DECLARADA: o lt_tex[1] carrega tambem 40% do bilateral
        // falso da 26-16 - mas aquele passe foi PROVADO cego a faixas finas
        // (o PS_Down ja tinha feito a media do bloco antes dele), entao ele e
        // ~= a gaussiana e a mistura continua valendo. Para ficar EXATO, ponha
        // 'Local: mistura bilateral' em 0.00: o lt_tex[1] vira a gaussiana pura
        // e o 0.40 daqui passa a ser literalmente o blend do slide 140.
        // (O passe da 26-16 continua rodando sem servir para nada; ele sai na
        // limpeza, nao aqui, para esta estacao mexer numa coisa so.)
        // ====================================================================
        float B  = amostra_bspline(input.uv, float2(64.0f, 32.0f));
        if (lt_on > 1.5f)
        {
            float g_sr = max(bg_sr, 1e-3f);
            float g_lo = M - max(bg_lo, 0.1f) * g_sr;
            float g_hi = M + max(bg_hi, 0.1f) * g_sr;
            float g_dz = max((g_hi - g_lo) / 12.0f, 1e-4f);
            float fz = clamp((Ii - g_lo) / g_dz - 0.5f, 0.0f, 11.0f);
            int   z0 = (int)floor(fz);
            float tz = fz - (float)z0;
            int   z1 = min(z0 + 1, 11);
            // meio texel de margem dentro do tile - sem isto o bilinear sangra
            float2 iuv = float2(clamp(input.uv.x, 0.5f / 64.0f, 1.0f - 0.5f / 64.0f),
                                clamp(input.uv.y, 0.5f / 32.0f, 1.0f - 0.5f / 32.0f));
            float2 t0 = (float2((float)(z0 % 4), (float)(z0 / 4)) + iuv) / float2(4.0f, 3.0f);
            float2 t1 = (float2((float)(z1 % 4), (float)(z1 / 4)) + iuv) / float2(4.0f, 3.0f);
            float4 s0 = tex_grid.SampleLevel(samp_grid, t0, 0);
            float4 s1 = tex_grid.SampleLevel(samp_grid, t1, 0);
            float4 sg = lerp(s0, s1, tz);
            float Bg = (sg.g > 1e-6f) ? (sg.r / sg.g) : B;
            // Patry slide 140 VERBATIM: "Blend between bilateral filter and very
            // wide 2D Gaussian" - 40% bilateral. E o default de bg_mix. Ele e
            // slider SEPARADO do 'Local: mistura bilateral', que governa o passe
            // antigo - misturar os dois significados num numero so foi exatamente
            // o tipo de confusao que esta temporada catalogou.
            B = lerp(B, Bg, saturate(bg_mix));
        }

        // M2.5-26-15 (A): slide 145 - "75% High Contrast, 100% Low Contrast".
        // A escala de contraste depende do LADO em que a media local esta. Em B = M
        // os dois ramos dao zero, entao a funcao e CONTINUA - nao ha degrau.
        float c_sel = (B >= M) ? lt_c : lt_c_lo;
        float Io = c_sel * (B - M) + lt_d * (Ii - B) + M;
        final_color.rgb *= exp(Io - Ii);
    }

    // ========================================================================
    // M2.5-26-11: A CURVA. Khronos PBR Neutral.
    // FONTE: Khronos Group, "PBR Neutral Tone Mapper", especificacao publica e
    // implementacao de referencia. Constantes verbatim da especificacao:
    //   F90 = 0.04 ; Ks = 0.8 - F90 = 0.76 ; Kd = 0.15
    // GARANTIAS QUE A ESPECIFICACAO DA (e por isso escolhemos esta, e nao outra):
    //  1. "for all input colors there are no hue shifts (angle around the white
    //     axis), since the only changes are within the plane defined by the input
    //     color and the white axis" - resolve o desvio de matiz que uma curva por
    //     canal produz. O proprio Narkowicz anota no ACES fit dele: "This is a very
    //     simple luminance only fit, which over saturates brights".
    //  2. c_out = c_in - F90 EXATO para toda cor com 0.08 <= canal <= 0.8. Ou seja:
    //     uma FAIXA INTEIRA onde a curva e uma translacao rigida. Nossas bandas de
    //     toon vivem ali dentro e passam INTACTAS, so deslocadas de 0.04. Nenhuma
    //     outra curva do tribunal oferece isso, e era exatamente a pergunta A3.
    //  3. Todas as derivadas parciais continuas, desaparecendo nas fronteiras.
    //  4. Analiticamente inversivel.
    // MEDIDO POR NOS: a saida atinge 255/255 (depois do sRGB) em tc = 13,45.
    // O ombro exponencial da 24I atingia em tc = 1,757. Ganho: +2,94 stops.
    // No quadro de 5h56 isso tira do branco chapado tudo entre 1,76 e 13,45 - que
    // eram 2,9 dos 3,2 stops que a curva antiga jogava fora.
    // O caminho antigo fica atras do seletor, para comparacao A/B.
    // ========================================================================
    // ========================================================================
    // M2.5-26-24: BALANCO DE BRANCO, em luz LINEAR, ANTES da curva.
    // Ghost of Tsushima, SIGGRAPH 2021, slide 155 VERBATIM: "Free, since we can
    // roll it into the Rec.709 => tonemap matrix." E onde a fonte poe, e e onde
    // a fisica exige: a adaptacao cromatica de Von Kries modela o olho se
    // adaptando ao ILUMINANTE e so vale em luz linear. Depois de uma curva
    // nao-linear ela deixa de ser o modelo que diz ser.
    // DIVERGENCIA DECLARADA do Frostbite, que poe "style (LUT/grading)" DEPOIS do
    // tonemap na ordem do deck. Seguimos o Ghost neste ponto, pela razao acima.
    // Com wb_alvo = (1,1,1) a matriz e a identidade (medido: desvio 3.3e-07),
    // entao o caminho default nao muda um bit.
    // O max(0) existe porque adaptacao cromatica pode devolver componente negativa
    // para cores fora do gamut de destino - e negativo entra na curva como lixo.
    // ========================================================================
    {
        float3 wbc = float3(dot(float3(wb_m0, wb_m1, wb_m2), final_color.rgb),
                            dot(float3(wb_m3, wb_m4, wb_m5), final_color.rgb),
                            dot(float3(wb_m6, wb_m7, wb_m8), final_color.rgb));
        final_color.rgb = max(wbc, 0.0f);
    }

    if (tonemap_modo > 0.5f)
    {
        float3 col = final_color.rgb;
        const float startCompression = 0.8f - 0.04f;
        const float desaturation = 0.15f;

        float xm = min(col.r, min(col.g, col.b));
        float offset = (xm < 0.08f) ? (xm - 6.25f * xm * xm) : 0.04f;
        col -= offset;

        float peak = max(col.r, max(col.g, col.b));
        if (peak >= startCompression)
        {
            const float dd = 1.0f - startCompression;
            float newPeak = 1.0f - dd * dd / (peak + dd - startCompression);
            col *= newPeak / max(peak, 1e-6f);
            float gg = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
            col = lerp(col, newPeak.xxx, gg);
        }
        final_color.rgb = col;
    }
    else
    {
        // M2.5-24I: tonemap de ombro exponencial - abaixo do ombro as cores ficam
        // INTACTAS (toon preservado); acima, rolloff filmico continuo. LEGADO:
        // satura em tc = 1,757 e nenhuma escolha de 'ombro' passa de ~4,9.
        float3 tc = final_color.rgb;
        float ts = tonemap_shoulder;
        float3 over = max(tc - ts, 0.0f);
        final_color.rgb = min(tc, ts) + (1.0f - ts) * (1.0f - exp(-over / max(1.0f - ts, 1e-4f)));
    }

    // M2.5-25-U6a: a saida vira sRGB AQUI. Ordem do Frostbite (deck, Camera-Exposure):
    //   1. film stock / tone map  ->  2. style (LUT / grading)  ->  3. sRGB / Rec709
    final_color.rgb = accurateLinearToSRGB(saturate(final_color.rgb));

    // O dither de de-banding passou para DEPOIS da codificacao. Ele vale MEIO degrau
    // do buffer de 8 bits, e "meio degrau" so existe no espaco de tela. Aplicado em
    // linear (como estava), perto do preto ele valia ~6,5 niveis de 255 - era o
    // granulado que aparecia so nas areas escuras.
    // M2.5-26-18: o raciocinio abaixo era do FAT PIXEL, que morreu nesta estacao.
    // Ele dizia que o dither tinha de viver na grade da CENA porque dentro de um fat
    // pixel a cor era constante - verdade enquanto a cena era 640x360 e a tela era
    // 1080p. Com render NATIVO um texel da cena E um pixel da tela: as duas grades
    // viraram a mesma, e a distincao deixou de existir. O dither continua na grade de
    // tela (a 26-10a ja o tinha movido para la) e agora isso e simplesmente o certo,
    // em vez de um conserto de um problema que a resolucao criava.
    // Precedente da casa: 24J-2c - "o jitter cobre o quantum" que ele disfarca.
    // M2.5-26-10a: REVERSAO DA 25-U6c, por erro de raciocinio meu, ja registrado em
    // U6.2. Eu invoquei o precedente 24J-2c ("o jitter cobre o quantum") para mover o
    // dither para a grade do fat pixel. A analogia estava errada: la o quantum era o
    // passo da marcha, no dominio de origem; aqui o quantum e a quantizacao de 8 bits
    // do swapchain, que acontece na resolucao da TELA, DEPOIS do upscale.
    // Com dither por pixel de tela, os pixels dentro de um mesmo fat pixel arredondam
    // para lados diferentes e o olho integra a media correta: a banda some e o ruido
    // fica na frequencia espacial ALTA, onde a visao e menos sensivel.
    // Com dither por fat pixel, o bloco inteiro se desloca junto: a banda vira borda
    // serrilhada e o ruido desce para uma frequencia MAIS visivel. Foi por isso que o
    // padrao ficou mais grosso depois da U6c.
    // LACUNA QUE PERMANECE: o hash ainda e o sin() que a propria casa condenou em
    // 26/08 (exaustao de mantissa; NVIDIA nao garante sin() entre plataformas). A
    // troca por hash inteiro ou mascara de blue noise espera fonte lida - esta na
    // fornada. Esta estacao corrige a GRADE, nao o gerador.
    float2 dpx = input.pos.xy;
    float dth = frac(sin(dot(dpx, float2(12.9898f, 78.233f))) * 43758.5453f);
    final_color.rgb += (dth - 0.5f) * dither_amt / 255.0f; // 25-U6d: forca no painel (0 = desligado)
    return final_color;
}
