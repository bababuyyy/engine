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
SamplerState samp : register(s0, space2);

cbuffer BloomBlock : register(b0, space3)
{
    float2 texel;
    float threshold;
    float knee;
    float cap;        // M2.5-26-11
    float3 pad_bl;
};

// --- PASSE 1: Threshold Extraction com Soft Knee ---
float4 PS_Threshold(PSInput input) : SV_Target0
{
    float4 c = tex_color.Sample(samp, input.uv);
    
    // Extracao de Luma rapida (G eh majoritario)
    // M2.5-26-11: TETO NO QUE ENTRA NA PIRAMIDE.
    // A FORMA vem da fonte. Frostbite (SIGGRAPH 2014) sec. 5.1.3, verbatim:
    //   "High intensity values that saturate sensor cells and leak to neighboring
    //    sensor cells."
    // Uma celula de sensor tem capacidade FINITA. Saturada, ela nao retem nem vaza
    // proporcionalmente mais. Sem teto, o disco solar (E/Omega ~ 1e9 cd/m2)
    // domina a piramide inteira. MEDIDO aqui: a lua chegava a 243 expostos, o
    // transbordo de 242 virava +113 unidades de brilho novo - 64x o ponto branco.
    // O NUMERO e arte declarada, com precedente de producao: o Godot expoe
    // 'glow_hdr_luminance_cap' exatamente para isto, com default 12.0 e um piso
    // de 6.0 no divisor. Nenhuma fonte do acervo prescreve o valor - a fornada
    // procurou e voltou com lacuna. Por isso ele e SLIDER, nao constante.
    c.rgb = min(c.rgb, max(cap, 1.0f));
    float luma = dot(c.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    
    // Soft Knee (curva quadratica que impede o pop de pixels brilhantes)
    float rq = clamp(luma - threshold + knee, 0.0f, 2.0f * knee);
    rq = (rq * rq) / (4.0f * max(knee, 0.0001f));
    
    float contrib = max(rq, luma - threshold);
    contrib /= max(luma, 0.0001f);
    
    return float4(c.rgb * contrib, 1.0f);
}

// --- PASSE 2: Downsample ---
// Box de 4 taps padrao para resolucoes internas baixas
float4 PS_Down(PSInput input) : SV_Target0
{
    float2 uv = input.uv;
    float dx = texel.x;
    float dy = texel.y;
    
    float3 sum = tex_color.SampleLevel(samp, uv + float2(-dx, -dy), 0).rgb;
    sum += tex_color.SampleLevel(samp, uv + float2(dx, -dy), 0).rgb;
    sum += tex_color.SampleLevel(samp, uv + float2(-dx, dy), 0).rgb;
    sum += tex_color.SampleLevel(samp, uv + float2(dx, dy), 0).rgb;
    
    return float4(sum * 0.25f, 1.0f);
}

// --- PASSE 3: Upsample (Kawase 9-tap Tent Filter) ---
// O segredo do bloom macio em resolucoes miniaturalizadas.
float4 PS_Up(PSInput input) : SV_Target0
{
    float2 uv = input.uv;
    float dx = texel.x * 1.5f; // Amplificacao do footprint
    float dy = texel.y * 1.5f;
    
    float3 sum = tex_color.SampleLevel(samp, uv + float2(-dx, -dy), 0).rgb;
    sum += tex_color.SampleLevel(samp, uv + float2(dx, -dy), 0).rgb;
    sum += tex_color.SampleLevel(samp, uv + float2(-dx, dy), 0).rgb;
    sum += tex_color.SampleLevel(samp, uv + float2(dx, dy), 0).rgb;
    
    sum *= 1.0f; // 4 cantos = peso 1
    
    sum += tex_color.SampleLevel(samp, uv + float2(0.0f, -dy), 0).rgb * 2.0f;
    sum += tex_color.SampleLevel(samp, uv + float2(0.0f, dy), 0).rgb * 2.0f;
    sum += tex_color.SampleLevel(samp, uv + float2(-dx, 0.0f), 0).rgb * 2.0f;
    sum += tex_color.SampleLevel(samp, uv + float2(dx, 0.0f), 0).rgb * 2.0f;
    
    sum += tex_color.SampleLevel(samp, uv, 0).rgb * 4.0f; // Centro = peso 4
    
    return float4(sum * (1.0f / 16.0f), 1.0f);
}