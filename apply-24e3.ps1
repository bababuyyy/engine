# apply-24e3.ps1 - M2.5-24E-3: Nubis ambient scattering (Schneider 2022) + energia propria da nuvem
$ErrorActionPreference = 'Stop'
$nl = "`r`n"

if (-not (Test-Path 'src\main.cpp')) { throw 'Rode da raiz do repo (src\main.cpp nao encontrado).' }
if (-not (Test-Path 'assets\shaders\sky.hlsl')) { throw 'assets\shaders\sky.hlsl nao encontrado.' }

$enc = New-Object System.Text.UTF8Encoding($false)

function Patch([string]$path, [System.Collections.ArrayList]$pairs) {
    $raw = [System.IO.File]::ReadAllText($path)
    foreach ($p in $pairs) {
        $old = $p[0]
        $n = [regex]::Matches($raw, [regex]::Escape($old)).Count
        if ($n -ne 1) {
            throw ("CONTRAPROVA FALHOU em {0}: ancora achada {1}x (esperado 1): [{2}...]" -f $path, $n, $old.Substring(0, [Math]::Min(60, $old.Length)))
        }
    }
    foreach ($p in $pairs) {
        $raw = $raw.Replace($p[0], $p[1])
    }
    [System.IO.File]::WriteAllText($path, $raw, $enc)
    Write-Host ("[ok] {0}: {1} patches aplicados" -f $path, $pairs.Count)
}

$mainPairs = New-Object System.Collections.ArrayList

[void]$mainPairs.Add(@(
'float cloud_curl_str = 0.4f;',
('float cloud_curl_str = 0.4f;' + $nl + 'float cloud_ambient_str = 1.0f; // M2.5-24E-3: energia propria da nuvem' + $nl + 'static_assert(sizeof(SkyParams) % 16 == 0, "SkyParams: alinhamento de 16 bytes quebrado");')
))

[void]$mainPairs.Add(@(
'    f << "cloud_curl_str=" << cloud_curl_str << "\n";',
('    f << "cloud_curl_str=" << cloud_curl_str << "\n";' + $nl + '    f << "cloud_ambient_str=" << cloud_ambient_str << "\n";')
))

[void]$mainPairs.Add(@(
'            else if (key == "cloud_curl_str") cloud_curl_str = std::stof(val);',
('            else if (key == "cloud_curl_str") cloud_curl_str = std::stof(val);' + $nl + '            else if (key == "cloud_ambient_str") cloud_ambient_str = std::stof(val);')
))

[void]$mainPairs.Add(@(
'    float erosao_str;       float cloud_silhouette; float cloud_bands; float pad_a;',
'    float erosao_str;       float cloud_silhouette; float cloud_bands; float cloud_ambient_str; // 24E-3: ocupa o antigo pad_a'
))

[void]$mainPairs.Add(@(
'            sky_params.pad_a = 0.0f;',
'            sky_params.cloud_ambient_str = cloud_ambient_str;'
))

[void]$mainPairs.Add(@(
'        ImGui::SliderFloat("G frontal", &cloud_hg_g_frontal, 0.5f, 0.85f);',
('        ImGui::SliderFloat("G frontal", &cloud_hg_g_frontal, 0.5f, 0.85f);' + $nl + '        ImGui::SliderFloat("Ambiente da nuvem", &cloud_ambient_str, 0.0f, 2.0f);')
))

Patch 'src\main.cpp' $mainPairs

$skyPairs = New-Object System.Collections.ArrayList

[void]$skyPairs.Add(@(
'    float cloud_bands;',
('    float cloud_bands;' + $nl + '    float cloud_ambient_str; // M2.5-24E-3: ocupa o padding implicito')
))

[void]$skyPairs.Add(@(
('                    float3 base_ambient = lerp(cloud_shadow_color, sky_ambient_color, 0.4f);' + $nl + '                    float3 ambient_term = base_ambient * sun_ambient * lerp(0.72f, 1.0f, hf);'),
('                    // M2.5-24E-3: Nubis ambient scattering (Schneider 2022) - maximo na casca, cai no miolo' + $nl + '                    float dim_profile = prof * saturate(cobertura * wcov);' + $nl + '                    float amb_scat = pow(saturate(1.0f - dim_profile), 0.5f);' + $nl + '                    float3 ambient_term = sky_ambient_color * (amb_scat * cloud_ambient_str);')
))

Patch 'assets\shaders\sky.hlsl' $skyPairs

Write-Host ''
Write-Host '24E-3 aplicado. Rode .\test.ps1 e abra o jogo.'
