// ============================================================================
//  local_tonemap.hlsl - M2.5-26-14: OPERADOR LOCAL DE TONEMAP.
// ----------------------------------------------------------------------------
//  FONTE: Jasmin Patry, Sucker Punch, "Lighting, Atmosphere, and Tonemapping in
//  Ghost of Tsushima", SIGGRAPH 2021, Advances in Real-Time Rendering.
//
//  O PROBLEMA, nas palavras deles (slide 130, VERBATIM):
//    "Large variation between interior and exterior luminance. Maintain
//     visibility while avoiding 'blow-outs'. Skies drove most of our lighting.
//     Brighter than environment. Wanted to avoid losing color."
//    Notas: "...a difference of UP TO 10 EV between the interiors and exteriors."
//  Nos medimos 10 stops entre o chao e a faixa do horizonte as 5h56. Mesmo
//  problema, mesma magnitude, mesma causa.
//
//  A TECNICA (slide 131, VERBATIM):
//    "In photography and cinematography: graduated filters, dodging & burning,
//     digital shadow/highlight adjustment [...] Bilateral filter can be used to
//     maintain image detail while reducing overall contrast."
//
//  A FORMULA (slide 137, VERBATIM), tudo em LOG-LUMINANCIA:
//    Io = c * (B - M) + d * (Ii - B) + M
//      Ii: log-luminancia da imagem de entrada
//      Io: log-luminancia de saida
//      B : log-luminancia FILTRADA (a media local)
//      M : log-luminancia do ponto medio ("the point that the luminance values
//          will be drawn towards as we reduce contrast")
//      c : escala de contraste     d : forca do detalhe
//  O primeiro termo comprime o contraste GLOBAL; o segundo devolve o detalhe
//  LOCAL. E dodging & burning automatico, por regiao.
//
//  ESCOPO DESTA ESTACAO - declarado: a fonte usa B = 40% bilateral + 60%
//  gaussiana larga (slide 140, VERBATIM: "We used a weighting of 40% for the
//  bilateral blur and 60% for the Gaussian blur"). O bilateral exige um Bilateral
//  Grid 64x32x64, que e uma operacao de SCATTER - cada pixel escreve numa fatia
//  escolhida pela sua luminancia - e este motor NAO TEM NENHUM PIPELINE DE
//  COMPUTE. Fazer o scatter por rasterizacao exige nuvem de pontos e blend
//  aditivo: e um subsistema, nao uma estacao.
//  Aqui implementamos os 60% GAUSSIANOS, que sao exatamente a metade da mistura
//  que a fonte usa - e a metade que ela diz EVITAR o ringing (slide 138: "Blurring
//  smooth gradients can result in ringing [...] E.g., clouds, filtered shadows,
//  specular highlights"). O bilateral entra depois, se a retina pedir.
//
//  PARAMETROS DA FONTE que adotamos verbatim (slide 140):
//    - blur gaussiano de RAIO 13 sobre imagem 64x32
//    - upsample BICUBICO B-SPLINE ("avoided bilinear filtering artifacts")
// ============================================================================

struct PSInput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput o;
    o.uv  = float2((vertexID << 1) & 2, vertexID & 2);
    o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

Texture2D    tex_src : register(t0, space2);
SamplerState samp    : register(s0, space2);
// M2.5-26-15: o bloom, que ate a 26-14 era INVISIVEL para este passe.
Texture2D    tex_bl  : register(t1, space2);
SamplerState samp_bl : register(s1, space2);

// M2.5-26-23a: a exposicao 1x1, so para o PS_Grid. Os outros tres entry points
// nao a referenciam, entao ela nao entra na reflexao deles - cada shader declara
// no create info so o numero de samplers que de fato usa.
Texture2D    tex_grid_expo  : register(t2, space2);
SamplerState samp_grid_expo : register(s2, space2);

cbuffer LtBlock : register(b0, space3)
{
    float2 lt_texel;  // 1/64, 1/32
    float  lt_dir;    // 1 = horizontal, 0 = vertical
    float  lt_bloom;  // M2.5-26-15: a mesma bloom_intensity do fullscreen
    float  lt_sr;     // M2.5-26-16: sigma da FAIXA (log-luminancia) do bilateral
    float  lt_mix;    // M2.5-26-16: peso do bilateral na mistura (fonte: 0.40)
    // M2.5-26-23a: a GRADE BILATERAL. O bloco cresce de 8 para 12 floats (48 B,
    // multiplo de 16). Os tres passes antigos ignoram os campos novos.
    float  lt_srcw;   // largura da fonte em pixels (para a conta de 10% de taps)
    float  lt_srch;   // altura da fonte
    float  lt_gz_sr;  // sigma_r em ln - Durand & Dorsey 2002: 0.4 log10 = 0.921 ln
    float  lt_gz_lo;  // quantos sigma_r ABAIXO do ponto medio M o eixo Z comeca
    float  lt_gz_hi;  // quantos sigma_r ACIMA de M ele termina
    float  lt_gz_taps;// N; a celula e varrida com N x N amostras
};

// ============================================================================
// M2.5-26-23a: A GRADE BILATERAL DE VERDADE (construcao + medidor, so isso).
//
// POR QUE ELA EXISTE. O 'B' de hoje vem do PS_Down, que faz MEDIA de blocos de
// ~40x44 pixels ANTES de qualquer bilateral existir. Logo B nunca pode passar da
// media do bloco, e a faixa fina do horizonte - medida em +5.05 stops acima do
// branco as 5h50 - e invisivel para ele. A grade classifica por LUMINANCIA em
// resolucao cheia: e o mecanismo, nao uma otimizacao.
//
// A FONTE, e a fonte da fonte:
//   Patry, SIGGRAPH 2021, slide 135, VERBATIM:
//     "The Bilateral Grid algorithm (Chen et al., 2007) allows efficient
//      computation [...] The algorithm allows the image data to be aggressively
//      downsampled (we used a volume of width 64, height 32 and depth 64)"
//   slide 136, VERBATIM:
//     "Populate grid with homogeneous log-luminance values (wiVi, wi)"
//     "Initial weight for each sample based on linear Z weights - each sample
//      contributes to two nearest Z slices"
//     "Weighting in X & Y not necessary due to blurring"
//   Chen, Paris & Durand, ACM TOG 26(3), 2007, sec. 3.2, VERBATIM:
//     "we set the sampling rates s_s and s_r to correspond to the bandwidths of
//      the Gaussian sigma_s and sigma_r"
//     -> A LARGURA DA CAIXA Z E O RANGE SIGMA. Sao um parametro, nao dois.
//   Durand & Dorsey, SIGGRAPH 2002, VERBATIM:
//     "The value sigma_r = 0.4 performed consistently well for all our
//      experiments" (em log10)  ->  0.4 * ln(10) = 0.921 ln
//
// A PROFUNDIDADE E DERIVADA, NAO ESCOLHIDA. Pela regra do Chen:
//     faixa exposta medida as 5h54 = ln(156.55 / 0.002986) = 10.87 ln
//     profundidade = 10.87 / 0.921 = 11.8  ->  12 FATIAS
// 12 cai DENTRO da faixa util publicada pelo Chen (sigma_r em [0.05,0.40] =
// 2.5 a 20 fatias). As 64 fatias do Patry estao de 3 a 25x FORA dela - o que e
// consistente com o ringing que ele reporta no slide 138 e diz reduzir com
// "larger luminance buckets". Nos seguimos o Chen, nao o Patry, neste eixo.
// O espacial 64x32 valida por DOIS caminhos: slide 135 do Patry, e s_s = 40 e
// 44.3 sobre 2560x1417, dentro do [10,50] publicado pelo Chen.
//
// GATHER, NAO SCATTER - e por que isso importa. A construcao do Chen e um
// scatter (cada pixel escreve numa celula 3D). Este motor tem ZERO pipelines de
// compute e nunca usou POINTLIST nem sampler no vertex. Invertendo para GATHER -
// cada texel de saida varre a PROPRIA regiao da fonte e aceita so as amostras
// cuja ln-luminancia cai na sua caixa Z - a construcao vira um passe de
// fragmento comum, do mesmo tipo que o motor ja tem dez. Nenhuma API nova.
//
// O LAYOUT NAO E INVENCAO NOSSA. Chen et al. guardam a grade como TEXTURA 2D com
// as fatias Z lado a lado no plano, justamente para usar filtragem bilinear de
// hardware e reduzir os lookups do slicing de 8 para 2. Atlas de 4x3 tiles de
// 64x32 = 256x96.
//
// OS 10% DE AMOSTRAS, com a condicao de validade junto. Chen sec. 3.5, VERBATIM:
//   "For typical values of sigma_s in [10,50] and sigma_r in [0.05,0.4], using
//    only 10% of the input pixels produces an output with no visual artifacts.
//    We choose the 10% of pixels by rotating through a sequence of precomputed
//    Poisson-disk patterns [...] we apply a temporal exponential filter with a
//    decay constant of 5 frames [...] except at hard scene transitions."
// A celula tem 40 x 44.28 = 1771 pixels da fonte. N = 13 da 169 amostras =
// 9.54% - ou seja, o default N=13 E os 10% do Chen, derivado e nao escolhido.
// E nos satisfazemos AS DUAS metades da condicao: s_s = 40/44.3 em [10,50] e
// sigma_r = 1/12 = 0.083 em [0.05,0.40]. Com as 64 fatias do Patry, a segunda
// metade seria violada e a garantia nao valeria.
//
// DESVIO DECLARADO, com o motivo: amostragem REGULAR em vez de Poisson-disk
// rotativo, e sem o filtro temporal de 5 frames. O filtro do Chen existe para
// combater "swimming" de padroes que VARIAM NO TEMPO; um padrao fixo nao produz
// swimming, entao a mitigacao nao se aplica. O custo e um vies de amostragem
// fixo em vez de ruido - visivel como aliasing na grade, nao como cintilacao.
//
// FORA DA FAIXA: uma amostra abaixo de lo ou acima de hi e GRAMPEADA na fatia
// extrema para escolher a caixa, mas contribui com o seu valor VERDADEIRO para
// o (wiVi). Assim nada e perdido e a media da caixa extrema continua honesta.
// (Mesma politica do histograma da exposicao, 26-12.)
//
// ESTA ESTACAO NAO LIGA A GRADE EM NADA. Ela constroi e da um modo de Vista para
// olhar. O 'B' do fullscreen continua vindo do caminho antigo. Zero mudanca de
// imagem - o que muda so aparece se voce escolher a Vista nova.
// ============================================================================
float4 PS_Grid(PSInput input) : SV_Target0
{
    // posicao no atlas 256x96 -> (celula 64x32, fatia z 0..11)
    int2 ap = (int2)(input.uv * float2(256.0f, 96.0f));
    ap = clamp(ap, int2(0, 0), int2(255, 95));
    int tx = ap.x / 64;
    int ty = ap.y / 32;
    int z  = ty * 4 + tx;                              // 4 tiles x 3 = 12 fatias
    int2 cell = int2(ap.x - tx * 64, ap.y - ty * 32);

    // o ponto medio exposto, EXATAMENTE como o fullscreen o calcula
    float2 e_med = tex_grid_expo.SampleLevel(samp_grid_expo, float2(0.5f, 0.5f), 0).rg;
    bool med_ok = (e_med.r > 0.0f && e_med.r < 1e6f && e_med.g > 1e-5f && e_med.g < 6e4f);
    float M = med_ok ? log(e_med.g * e_med.r) : 0.0f;

    float sr = max(lt_gz_sr, 1e-3f);
    float lo = M - max(lt_gz_lo, 0.1f) * sr;
    float hi = M + max(lt_gz_hi, 0.1f) * sr;
    float dz = max((hi - lo) / 12.0f, 1e-4f);
    float zc = lo + ((float)z + 0.5f) * dz;            // centro desta fatia

    int N = (int)clamp(lt_gz_taps, 2.0f, 24.0f);
    float2 c0 = float2(cell) / float2(64.0f, 32.0f);
    float2 c1 = float2(cell + int2(1, 1)) / float2(64.0f, 32.0f);

    float wv = 0.0f, wsum = 0.0f;
    [loop] for (int y = 0; y < N; ++y)
    {
        [loop] for (int x = 0; x < N; ++x)
        {
            float2 t = (float2((float)x, (float)y) + 0.5f) / (float)N;
            float2 uv = lerp(c0, c1, t);
            float3 c = tex_src.SampleLevel(samp, uv, 0).rgb;
            if (lt_bloom > 0.0f) c += tex_bl.SampleLevel(samp_bl, uv, 0).rgb * lt_bloom;
            float l = dot(max(c, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
            float V = log(max(l, 1e-5f));
            // grampeia SO para escolher a caixa; o valor que entra e o verdadeiro
            float Vz = clamp(V, lo + 0.5f * dz, hi - 0.5f * dz);
            // peso Z linear: contribui para as DUAS fatias mais proximas (slide 136)
            float w = saturate(1.0f - abs(Vz - zc) / dz);
            wv   += w * V;
            wsum += w;
        }
    }
    // (wiVi, wi) - o par homogeneo do slide 136. A normalizacao e no slicing.
    return float4(wv, wsum, 0.0f, 1.0f);
}

// --- PASSE 1: cena HDR pre-exposta -> log-luminancia 64x32 -------------------
// 4x4 amostras bilineares por texel. A saida ja e log, entao o blur seguinte
// opera no dominio em que a formula do slide 137 vive.
float4 PS_Down(PSInput input) : SV_Target0
{
    float soma = 0.0f;
    [loop] for (int y = 0; y < 4; ++y)
    {
        [loop] for (int x = 0; x < 4; ++x)
        {
            float2 uv = input.uv + (float2((float)x, (float)y) - 1.5f) * lt_texel * 0.25f;
            // M2.5-26-15 (B): a MESMA composicao do fullscreen.
            //   final_color = composite + shafts + bloom * bloom_intensity
            // Ate a 26-14 este passe lia so o composite. Resultado medido: a media
            // local B nao enxergava o halo do bloom, o termo de detalhe d*(Ii-B)
            // tratava o halo inteiro como detalhe a PRESERVAR, e com d = 1.0 ele
            // passava intacto. Era por isso que a unica forma de matar o estouro
            // era desligar o bloom: ele era invisivel para o operador.
            // Calculado as 5h50: com B cego ao bloom a faixa de 53.2 virava 60.6
            // (mais brilhante); com B enxergando, vira 11.2 - abaixo dos 13.45 em
            // que a curva satura.
            // LACUNA DECLARADA: os shafts continuam invisiveis. Sao uma marcha
            // volumetrica e nao ha como repeti-la aqui a custo aceitavel. O bloom
            // e o termo dominante - foi o proprio teste do interruptor que mostrou.
            float3 c = tex_src.SampleLevel(samp, uv, 0).rgb
                     + tex_bl.SampleLevel(samp_bl, uv, 0).rgb * lt_bloom;
            float  l = dot(max(c, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)); // Rec.709
            soma += log(max(l, 1e-5f));
        }
    }
    return float4(soma * (1.0f / 16.0f), 0.0f, 0.0f, 1.0f);
}

// --- PASSE 4: O FILTRO BILATERAL --------------------------------------------
//  FONTE: Patry, SIGGRAPH 2021, slide 131, VERBATIM:
//    "Bilateral filter can be used to maintain image detail while reducing
//     overall contrast."
//  slide 140, VERBATIM:
//    "we adopted a hybrid approach by blending between the output of the
//     bilateral filter and a very wide 2D Gaussian blur [...] we used a
//     weighting of 40% for the bilateral blur and 60% for the Gaussian blur"
//
//  POR QUE FORCA BRUTA E NAO A GRADE 3D. A propria fonte diz por que a grade
//  existe, slide 135, VERBATIM:
//    "Naive bilateral filters are expensive, but the Bilateral Grid algorithm
//     allows them to be computed efficiently with a low memory footprint."
//  Caro em RESOLUCAO CHEIA: ele monta a grade a partir dos pixels de 1080p. Nos
//  ja temos a log-luminancia reduzida a 64x32 pelo PS_Down. Sobre 2.048 texels o
//  filtro ingenuo custa 2.048 x 27x27 = 1,49 M amostras - contra as ~22 M da
//  marcha de nuvens. A grade seria otimizar o que ja e barato, ao preco de um
//  scatter (que exige compute, e este motor nao tem nenhum pipeline de compute).
//
//  DIVERGENCIA DECLARADA, COM NUMERO. A nossa gaussiana e CEGA a uma faixa fina:
//  o PS_Down faz media de blocos de ~10x11 pixels, entao uma faixa de 1-2 texels
//  no horizonte contribui so ~18% do peso do borrao de raio 13. Medido no quadro
//  das 5h54: B gaussiano = -1.504 enquanto a faixa vale +3.197 em log. Com a
//  mistura 40/60 da fonte, 60% de um B cego DOMINA, e nenhum valor positivo de
//  'c' alcanca a faixa. Por isso o slider da mistura vai ate 1.00 e o default
//  nasce em 0.40 (o da fonte): a fonte fica sendo o ponto de partida honesto, e
//  a divergencia fica no painel, medida, em vez de escondida num default.
//
//  O PESO ESPACIAL e o MESMO da gaussiana (sigma 4.33333, raio 13), para que
//  mistura = 0 devolva exatamente o comportamento da 26-15.
float4 PS_Bilat(PSInput input) : SV_Target0
{
    const int R = 13;
    const float ss2 = 2.0f * 4.33333f * 4.33333f;
    float sr = max(lt_sr, 1e-3f);
    float sr2 = 2.0f * sr * sr;

    float Vc = tex_src.SampleLevel(samp, input.uv, 0).r;
    float soma = 0.0f, peso = 0.0f;
    [loop] for (int dy = -R; dy <= R; ++dy)
    {
        [loop] for (int dx = -R; dx <= R; ++dx)
        {
            float2 uv = input.uv + float2((float)dx, (float)dy) * lt_texel;
            float Vs = tex_src.SampleLevel(samp, uv, 0).r;
            float dv = Vs - Vc;
            float w = exp(-(float)(dx * dx + dy * dy) / ss2) * exp(-dv * dv / sr2);
            soma += Vs * w;
            peso += w;
        }
    }
    float b_bil = soma / max(peso, 1e-6f);
    // aqui tex_bl NAO e o bloom: neste passe ele carrega o B GAUSSIANO ja pronto.
    // O registrador e o mesmo (t1) porque cada entry point tem a sua reflexao.
    float b_gau = tex_bl.SampleLevel(samp_bl, input.uv, 0).r;
    return float4(lerp(b_gau, b_bil, saturate(lt_mix)), 0.0f, 0.0f, 1.0f);
}

// --- PASSE 2 e 3: gaussiana SEPARAVEL, raio 13 (slide 140) ------------------
// Uma passagem horizontal e uma vertical. O raio 13 sobre uma imagem de 64x32 e
// enorme de proposito: a fonte diz que a gaussiana "must be wide enough to avoid
// noticeable haloing".
float4 PS_Blur(PSInput input) : SV_Target0
{
    const int   R = 13;
    const float sigma = 4.33333f;   // raio/3: a gaussiana cai a ~1% na borda do kernel
    float2 passo = float2(lt_dir, 1.0f - lt_dir) * lt_texel;

    float soma = 0.0f, peso = 0.0f;
    [loop] for (int i = -R; i <= R; ++i)
    {
        float w = exp(-0.5f * (float(i) * float(i)) / (sigma * sigma));
        soma += tex_src.SampleLevel(samp, input.uv + passo * (float)i, 0).r * w;
        peso += w;
    }
    return float4(soma / max(peso, 1e-6f), 0.0f, 0.0f, 1.0f);
}
