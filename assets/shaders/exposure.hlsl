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
    float e_speed;  // adaptacao, 1/s
    float e_dt;     // segundos desde o frame anterior
    float e_pad0;
    float e_pad1;
};

// --- PASSE 1: cena HDR -> mapa 32x32 de LOG-luminancia -----------------------
// Cada texel de saida integra a sua regiao com 6x6 amostras bilineares bem
// distribuidas (36 taps x 1024 texels = ~37 mil amostras por frame, cobrindo o
// quadro inteiro de forma uniforme). A saida ja e log, entao a media aritmetica
// do passe seguinte produz a MEDIA GEOMETRICA - o L_barra_w de Reinhard.
float4 PS_Luma(PSInput input) : SV_Target0
{
    float2 base = floor(input.uv * 32.0f) / 32.0f;
    float sum = 0.0f;
    [loop] for (int y = 0; y < 6; ++y)
    {
        [loop] for (int x = 0; x < 6; ++x)
        {
            float2 uv = base + (float2((float)x, (float)y) + 0.5f) / (6.0f * 32.0f);
            float3 c = tex_a.SampleLevel(samp, uv, 0).rgb;
            float l = dot(max(c, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)); // luma Rec.709
            sum += log(max(l, 1e-5f)); // delta de Reinhard: nenhum pixel preto zera a media
        }
    }
    return float4(sum * (1.0f / 36.0f), 0.0f, 0.0f, 1.0f);
}

// --- PASSE 2: 32x32 -> 1x1, alvo + adaptacao (ping-pong) ---------------------
float4 PS_Adapt(PSInput input) : SV_Target0
{
    // Meio texel de deslocamento: cada amostra bilinear ja media 2x2, entao
    // 16x16 amostras cobrem os 32x32 texels EXATAMENTE, sem peso duplicado.
    float sum = 0.0f;
    [loop] for (int y = 0; y < 16; ++y)
    {
        [loop] for (int x = 0; x < 16; ++x)
        {
            float2 uv = (float2((float)x, (float)y) * 2.0f + 1.0f) / 32.0f;
            sum += tex_a.SampleLevel(samp, uv, 0).r;
        }
    }
    float l_avg = exp(sum * (1.0f / 256.0f)); // media geometrica

    float alvo = e_key / max(l_avg, 1e-5f) * exp2(e_ec);
    alvo = clamp(alvo, min(e_min, e_max), max(e_min, e_max));

    float prev = tex_b.SampleLevel(samp, float2(0.5f, 0.5f), 0).r;
    if (!(prev > 0.0f)) prev = alvo; // primeiro frame (ou NaN): entra ja exposto

    // adaptacao exponencial do "olho": converge igual em 60 ou 240 fps
    float cur = prev + (alvo - prev) * (1.0f - exp(-e_dt * max(e_speed, 0.01f)));
    return float4(cur, 0.0f, 0.0f, 1.0f);
}
