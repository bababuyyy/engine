// ============================================================================
// M2.5-26-4: EXPOSICAO MEDIDA NA GPU (o estimador analitico sai de cena)
//
// ---------------------------------------------------------------------------
// A FONTE, VERBATIM. Lagarde & de Rousiers, "Moving Frostbite to Physically
// Based Rendering 3.0", SIGGRAPH 2014, sec. 5.1.1 (logo abaixo da eq. 69):
//
//   "In games, the usual way to get Lavg is to take the average log luminance
//    of all the image pixels. This proves to be a bit unstable with high
//    intensity flickering lights or highlights. A better way is to smooth the
//    value over time, or use a histogram of the luminance to remove extreme
//    values [Vla08]. However these methods suffer from exposing based on pixel
//    luminance multiplied by albedo. This means that if the scene is fully 10%
//    grey or fully 90% grey, it will result in the same color. An alternative
//    is to expose based on the luminance before the multiplication with albedo
//    [Koj+13]. However in practice it is expensive to maintain a separate
//    lighting buffer with white-only luminance. In Frostbite, we have adopted
//    the histogram method."
//
// ---------------------------------------------------------------------------
// O QUE ESTA IMPLEMENTACAO ESCOLHEU, DECLARADO SEM RODEIO:
//   media log de todos os pixels  +  suavizacao no tempo.
// A fonte nomeia a media log como "the usual way", e oferece DOIS remedios para
// a instabilidade dela, ligados por "ou": suavizar no tempo, OU histograma.
// Pegamos o primeiro. A Frostbite pegou o segundo.
// Isto NAO e a nossa preferencia disfarcada de fonte: e uma das duas opcoes que
// a fonte apresenta, e a escolha esta aqui escrita para quem vier depois julgar.
// O histograma continua na fila (26-4c) e reaproveita esta MESMA encanacao -
// os dois passes ficam, so a reducao do meio muda. Nada aqui vira lixo.
//
// ---------------------------------------------------------------------------
// LIMITES DECLARADOS (os dois sao da fonte, nao invencao nossa):
// 1. INSTABILIDADE COM HIGHLIGHTS. A fonte nomeia exatamente este modo de falha.
//    No nosso caso o suspeito e o disco do sol: com raio de 3 graus ele tem ~121x
//    a area fisica, entao a exposicao pode PULSAR quando ele entra e sai do
//    quadro. A suavizacao temporal atenua; nao elimina. Se acontecer, a cura da
//    fonte e histograma com descarte de extremos - nao mexer na Chave.
// 2. EXPOSICAO SOBRE LUMINANCIA x ALBEDO. Vale para a media log E para o
//    histograma: "if the scene is fully 10% grey or fully 90% grey, it will
//    result in the same color". Andar de uma floresta escura para areia clara
//    produz compensacao onde nao deveria haver. A alternativa ([Koj+13]) e expor
//    sobre a luminancia ANTES do albedo, e a propria fonte diz que e cara
//    ("expensive to maintain a separate lighting buffer"). Herdamos o limite,
//    como a Frostbite herdou.
//
// ---------------------------------------------------------------------------
// FISICA COM FONTE (blindada, sem slider):
//   - Media log de Reinhard (Luksch, "Realtime HDR", TU Wien; checklist N4.3):
//       L_barra_w = exp( (1/N) * SOMA( log(delta + Lw) ) )
//   - Constante do fotometro (eq. 69 + Listing 28, K = 12.5):
//       exposure = K / (1.2 * 100) / Lavg = 0.1041667 / Lavg
//   - Compensacao de arte (eq. 68): EV100' = EV100 - EC  ->  fator 2^EC
//   - Luma Rec.709 (0.2126 / 0.7152 / 0.0722)
//
// ENGENHARIA DA CASA (sem numero na fonte; escolhas reversiveis, declaradas):
//   - resolucao intermediaria 32x32; 6x6 taps por texel; 16x16 taps no colapso
//   - delta de Reinhard = 1e-5
//   - dois passes + par de texturas 1x1 em ping-pong
//   - medimos composite_color (antes dos shafts e do bloom), nao o quadro final
//   - rodamos ANTES do fullscreen, e nao sobre o quadro anterior: a exposicao
//     mede ESTE quadro. E melhor que a pratica da fonte, mas e escolha nossa.
//
// ---------------------------------------------------------------------------
// POR QUE ESTA ESTACAO EXISTE. O estimador analitico da 25-4 CALCULAVA a
// luminancia a partir do modelo de luz em vez de medir a imagem - um metodo que
// NAO EXISTE na fonte. Cobrou tres contas: (1) cego para a lua, a noite rodava
// com a exposicao do meio-dia; (2) o teto mordia ao meio-dia, dia e noite no
// mesmo numero; (3) mede um receptor HORIZONTAL numa cena feita de paredes - as
// 16/17h estouram enquanto ele acha que escureceu. A (3) foi confirmada por
// experimento do autor. Um medidor que le a IMAGEM nao tem nenhum desses pontos
// cegos: ele nao precisa saber que existe lua, parede ou tocha.
//
// DOUTRINA: a regra da casa proibe readback GPU->CPU. Ela NAO e violada - a
// medida nasce, adapta e e consumida sem sair da GPU. A CPU nunca ve o numero.
// ============================================================================

struct PSInput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput o;
    o.uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

Texture2D tex_a : register(t0, space2);  // PS_Luma: a cena HDR | PS_Adapt: o mapa 32x32
Texture2D tex_b : register(t1, space2);  // PS_Adapt: a exposicao do frame anterior (1x1)
SamplerState samp : register(s0, space2);

cbuffer ExpoBlock : register(b0, space3)
{
    float e_key;    // K/(1.2*100) = 0.1041667 - constante do fotometro, nao arte
    float e_ec;     // compensacao em stops (eq. 68) - ARTE
    float e_min;
    float e_max;
    float e_vel_up; // M2.5-26-19: stops/s indo para o CLARO  (Unreal Speed Up)
    float e_dt;     // segundos desde o frame anterior
    float e_plo;    // M2.5-26-12: percentil INFERIOR do histograma (0..1)
    float e_phi;    // M2.5-26-12: percentil SUPERIOR
    float e_texw;   // M2.5-26-17: largura da fonte, em texels
    float e_texh;   // M2.5-26-17: altura
    float e_vel_dn; // M2.5-26-19: stops/s indo para o ESCURO (Unreal Speed Down)
    float e_trans;  // M2.5-26-19: stops onde a rampa vira exponencial (Unreal 1.5)
};

// --- PASSE 1: cena HDR -> mapa 32x32 de LOG-luminancia -----------------------
// Cada texel de saida integra a sua regiao com 6x6 amostras bilineares bem
// distribuidas (36 taps x 1024 texels = ~37 mil amostras por frame, cobrindo o
// quadro inteiro de forma uniforme). A saida ja e log, entao a media aritmetica
// do passe seguinte produz a MEDIA GEOMETRICA - o L_barra_w de Reinhard.
float4 PS_Luma(PSInput input) : SV_Target0
{
    // M2.5-26-2a: a cena chega PRE-EXPOSTA (epilogo do Listing 31). Para medir
    // luminancia ABSOLUTA, desfazemos a pre-exposicao - a propria fonte diz que "the
    // pre-exposure can be undone to retrieve the absolute luminance value".
    float e_prev = tex_b.SampleLevel(samp, float2(0.5f, 0.5f), 0).r;
    e_prev = (e_prev > 0.0f && e_prev < 1e6f) ? e_prev : 1.0f;
    float inv_prev = 1.0f / e_prev;

    // ========================================================================
    // M2.5-26-17: O MEDIDOR PASSA A DIZER A VERDADE SOBRE O PICO.
    //
    // DEFEITO 1 - a amostra era BILINEAR. SampleLevel com o upscale_sampler
    // (FILTER_LINEAR) ja media 2x2 pixels por tap. Media de media: o pico de
    // qualquer coisa fina saia diluido DUAS vezes antes de ser lido. Agora e
    // Load(), texel exato, sem filtro nenhum.
    //
    // DEFEITO 2 - so existia a MEDIA. Os canais G e B estavam zerados, e o
    // "max" que o [MEDIDA] reportava era o maior entre MEDIAS DE BLOCO - nunca
    // o maior pixel. Uma faixa fina e brilhante dentro de um bloco escuro
    // desaparecia nessa media, e o numero saia como se fosse o pico. Custou uma
    // estacao inteira de contas feitas sobre um piso tratado como teto.
    //   R = media do log (o que a metricacao usa - nao muda de significado)
    //   G = MAIOR log do bloco   (o pico honesto)
    //   B = MENOR log do bloco   (o vale honesto)
    //
    // APROXIMACAO DECLARADA: 8x8 = 64 texels EXATOS por bloco. Em 640x360 o
    // bloco tem ~20x11 = 225 pixels, entao 64 cobre 28% deles. O G continua
    // sendo um PISO do pico verdadeiro do bloco - mas um piso muito mais alto
    // que a media, e a razao G/R passa a dizer QUANTO o bloco esconde.
    // ========================================================================
    int2 bloco = (int2)floor(input.uv * 32.0f);
    float2 tam = float2(max(e_texw, 1.0f), max(e_texh, 1.0f)) / 32.0f;
    int2 p0 = (int2)(float2(bloco) * tam);
    int2 p1 = (int2)(float2(bloco + int2(1, 1)) * tam);
    int2 lim = int2((int)max(e_texw, 1.0f) - 1, (int)max(e_texh, 1.0f) - 1);
    int2 ext = max(p1 - p0, int2(1, 1));

    float sum = 0.0f;
    float pico = -1e30f;
    float vale =  1e30f;
    [loop] for (int y = 0; y < 8; ++y)
    {
        [loop] for (int x = 0; x < 8; ++x)
        {
            int2 p = p0 + (int2)((float2((float)x, (float)y) + 0.5f) / 8.0f * float2(ext));
            p = clamp(p, int2(0, 0), lim);
            float3 c = tex_a.Load(int3(p, 0)).rgb;
            float l = dot(max(c, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)); // luma Rec.709
            float lg = log(max(l * inv_prev, 1e-5f)); // 26-2a: desfaz a pre-exposicao
            sum += lg;
            pico = max(pico, lg);
            vale = min(vale, lg);
        }
    }
    return float4(sum * (1.0f / 64.0f), pico, vale, 1.0f);
}

// --- PASSE 2: 32x32 -> 1x1, alvo + adaptacao (ping-pong) ---------------------
float4 PS_Adapt(PSInput input) : SV_Target0
{
    // ========================================================================
    // M2.5-26-12: O METODO DO HISTOGRAMA.
    // Frostbite (Lagarde & de Rousiers, SIGGRAPH 2014), sec. 5.1.1, VERBATIM:
    //   "In games, the usual way to get Lavg is to take the average log luminance
    //    of all the image pixels. This proves to be a bit unstable with high
    //    intensity flickering lights or highlights. A better way is to smooth the
    //    value over time, or use a histogram [...] In Frostbite, we have adopted
    //    the histogram method."
    // Era o ULTIMO desvio documentado no caminho da camera. Ate aqui usavamos a
    // media geometrica - o metodo que a fonte chama de instavel e diz ter deixado.
    //
    // POR QUE, COM MEDIDA NOSSA: no quadro das 12h antes da 26-6a, com o ceu preso
    // em 8 cd/m2 e o chao em 11.340, a media geometrica devolveu 117 - um valor
    // que caia no VAZIO entre os dois modos, onde nao existia um unico pixel.
    // Expor para 117 era expor para nada. Uma media aparada por percentil e imune
    // a isso por construcao: ela descarta as caudas e mede o CORPO da distribuicao.
    //
    // COMO: duas passagens sobre os 1024 texels.
    //   (1) histograma de 64 caixas sobre ln(L) -> acha os limiares dos percentis
    //   (2) media EXATA dos texels dentro da faixa (sem erro de quantizacao)
    // Usamos Load() e nao SampleLevel(): leitura de texel exata, sem filtragem
    // bilinear misturando vizinhos - o que tambem corrige o vies de amostragem
    // que a vistoria apontou.
    //
    // APROXIMACAO DECLARADA: o histograma e sobre os 1024 texels do mapa 32x32,
    // e cada texel ja e uma media de log da sua regiao (36 amostras). Ou seja: e
    // um histograma de medias regionais, nao de pixels. A fonte nao especifica a
    // resolucao, e esta e a que temos sem readback.
    // M2.5-26-19: A LACUNA DOS PERCENTIS FECHOU - e o valor que estava aqui
    // estava errado de LEITURA, nao so de numero.
    // Frostbite nomeia o metodo e nao publica os percentis. Quem publica e a
    // Unreal (Post Process Volume, Metering Mode Auto Exposure Histogram),
    // VERBATIM:
    //   "Low Percent - a good starting range is 70 to 80"
    //   "High Percent - a good starting range is 80 to 95"
    // A 26-12 leu o parametro como "apara as caudas" e escolheu 5/95, uma janela
    // que cobre 90% do quadro - ou seja, quase a media de tudo. A leitura certa e
    // JANELA ESTREITA E ALTA: 75/85 sao 10% do quadro, tirados do alto da
    // distribuicao. Ela responde "qual e a luminancia do decil claro", nao "qual
    // e o brilho medio".
    // MEDIDO no histograma real das 5h50 desta engine: 75/85 le 4,4x mais
    // brilhante que 5/95 = 2,1 stops A MENOS de exposicao.
    // ========================================================================
    const int NB = 64;
    const float LN_MIN = -9.21034f;  // ln(1e-4) - abaixo disso e noite sem lua
    const float LN_MAX = 13.81551f;  // ln(1e6)  - acima disso e disco solar
    const float LN_ESC = (float)NB / (LN_MAX - LN_MIN);

    float hist[NB];
    [unroll] for (int b0 = 0; b0 < NB; ++b0) hist[b0] = 0.0f;

    [loop] for (int y = 0; y < 32; ++y)
    {
        [loop] for (int x = 0; x < 32; ++x)
        {
            float lnl = tex_a.Load(int3(x, y, 0)).r;
            int b = (int)clamp((lnl - LN_MIN) * LN_ESC, 0.0f, (float)(NB - 1));
            hist[b] += 1.0f;
        }
    }

    // limiares dos percentis, em ln
    float n_lo = clamp(e_plo, 0.0f, 0.98f) * 1024.0f;
    float n_hi = clamp(e_phi, 0.02f, 1.0f) * 1024.0f;
    if (n_hi <= n_lo) { n_lo = 0.0f; n_hi = 1024.0f; } // faixa invertida: mede tudo
    float ln_lo = LN_MIN, ln_hi = LN_MAX;
    float acc = 0.0f;
    [loop] for (int b1 = 0; b1 < NB; ++b1)
    {
        float ini = acc; acc += hist[b1];
        float borda_ini = LN_MIN + (float)b1 / LN_ESC;
        float borda_fim = LN_MIN + (float)(b1 + 1) / LN_ESC;
        if (ini <= n_lo && acc > n_lo) ln_lo = borda_ini;
        if (ini <  n_hi && acc >= n_hi) ln_hi = borda_fim;
    }

    // media EXATA dentro da faixa (segunda passagem, sem quantizacao de caixa)
    float soma = 0.0f, peso = 0.0f;
    [loop] for (int y2 = 0; y2 < 32; ++y2)
    {
        [loop] for (int x2 = 0; x2 < 32; ++x2)
        {
            float lnl = tex_a.Load(int3(x2, y2, 0)).r;
            if (lnl >= ln_lo && lnl <= ln_hi) { soma += lnl; peso += 1.0f; }
        }
    }
    // se a faixa esvaziou (distribuicao degenerada), cai para a media de tudo
    if (peso < 0.5f)
    {
        [loop] for (int y3 = 0; y3 < 32; ++y3)
            [loop] for (int x3 = 0; x3 < 32; ++x3)
            { soma += tex_a.Load(int3(x3, y3, 0)).r; peso += 1.0f; }
    }
    float l_avg = exp(soma / max(peso, 1.0f));

    float alvo = e_key / max(l_avg, 1e-5f) * exp2(e_ec);
    alvo = clamp(alvo, min(e_min, e_max), max(e_min, e_max));

    float prev = tex_b.SampleLevel(samp, float2(0.5f, 0.5f), 0).r;
    if (!(prev > 0.0f)) prev = alvo; // primeiro frame (ou NaN): entra ja exposto

    // ========================================================================
    // M2.5-26-19: A ADAPTACAO ANDA EM STOPS, NAO EM MULTIPLICADOR.
    //
    // O que estava aqui era  cur = prev + (alvo-prev)*(1-exp(-dt*speed))  - uma
    // interpolacao no MULTIPLICADOR. Como o multiplicador e exponencial no EV, a
    // mesma constante de tempo vira taxas completamente diferentes em stops
    // conforme a direcao. Medido no nosso codigo, salto de 11 stops com speed=2:
    //   escuro -> claro: ~2,9 stops/s, converge em ~5 s
    //   claro -> escuro: 10,3 stops NO PRIMEIRO MEIO SEGUNDO
    // Assimetria de ~7x, e na direcao errada.
    //
    // Unity HDRP, documentacao de Exposure, VERBATIM:
    //   "The human eye adapts slower to darkness than to lightness"
    // Unreal, Post Process Volume: Speed Up / Speed Down em f-stops por segundo,
    // defaults 3.0 e 1.0. r.EyeAdaptation.ExponentialTransitionDistance = 1.5:
    // acima dessa distancia do alvo a adaptacao e uma RAMPA de taxa constante,
    // dentro dela vira exponencial para assentar.
    //
    // A conversao (Listing 28, Frostbite 5.1):  exposure = 1/(1.2 * 2^EV100)
    //                              logo:        EV100 = -log2(1.2 * exposure)
    // ========================================================================
    float ev_prev = -log2(max(prev, 1e-9f) * 1.2f);
    float ev_alvo = -log2(max(alvo, 1e-9f) * 1.2f);
    float d_ev = ev_alvo - ev_prev;
    // EV subindo = a exposicao FECHA = a cena CLAREOU. Esse e o lado rapido.
    float vel = (d_ev > 0.0f) ? max(e_vel_up, 0.01f) : max(e_vel_dn, 0.01f);
    float trans = max(e_trans, 0.01f);
    float passo = (abs(d_ev) > trans)
                ? (d_ev > 0.0f ? vel * e_dt : -vel * e_dt)        // rampa
                : d_ev * (1.0f - exp2(-e_dt * vel));              // assentamento
    if (abs(passo) > abs(d_ev)) passo = d_ev;                     // nunca ultrapassa
    float cur = 1.0f / (1.2f * exp2(ev_prev + passo));
    // O aquecimento (dt = 1000 nos dois primeiros frames, e sempre que o
    // automatico esta desligado) continua funcionando: a rampa pediria 3000
    // stops, o clamp de nao-ultrapassar corta em d_ev, e o resultado cai
    // exatamente no alvo - que e o comportamento que esses frames precisam.
    // M2.5-26-15: o canal G passa a levar a MEDIA MEDIDA (aparada por percentil).
    // Custa zero: a textura ja e R16G16B16A16_FLOAT e so o R estava em uso. Com ela,
    // o operador local calcula o proprio ponto medio como log(l_avg * exposicao) -
    // a media REAL do quadro exposto - em vez de acreditar na chave. Vale igual com
    // a exposicao automatica DESLIGADA, porque nesse caso emin = emax = manual e o
    // 'cur' ja e a exposicao manual: o produto continua sendo a media exposta certa.
    return float4(cur, l_avg, 0.0f, 1.0f);
}
