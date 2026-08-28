# apply-24e4.ps1 - M2.5-24E-4: silver lining atras de slider (fase normalizada + max artistico)
$ErrorActionPreference = 'Stop'
$nl = "`r`n"
if (-not (Test-Path 'src\main.cpp')) { throw 'Rode da raiz do repo.' }
$enc = New-Object System.Text.UTF8Encoding($false)
function Patch([string]$path, [System.Collections.ArrayList]$pairs) {
    $raw = [System.IO.File]::ReadAllText($path)
    foreach ($p in $pairs) {
        $n = [regex]::Matches($raw, [regex]::Escape($p[0])).Count
        if ($n -ne 1) { throw ("CONTRAPROVA FALHOU em {0}: ancora {1}x (esperado 1): [{2}...]" -f $path, $n, $p[0].Substring(0, [Math]::Min(60, $p[0].Length))) }
    }
    foreach ($p in $pairs) { $raw = $raw.Replace($p[0], $p[1]) }
    [System.IO.File]::WriteAllText($path, $raw, $enc)
    Write-Host ("[ok] {0}: {1} patches" -f $path, $pairs.Count)
}

$m = New-Object System.Collections.ArrayList
[void]$m.Add(@(
'static_assert(sizeof(SkyParams) % 16 == 0, "SkyParams: alinhamento de 16 bytes quebrado");',
('static_assert(sizeof(SkyParams) % 16 == 0, "SkyParams: alinhamento de 16 bytes quebrado");' + $nl + 'float cloud_silver_k = 0.0f;      // M2.5-24E-4: silver lining (0 = fisica pura)' + $nl + 'float cloud_silver_spread = 0.3f;')
))
[void]$m.Add(@(
'    float cloud_shadow_color[3];              float curl_str;',
('    float cloud_shadow_color[3];              float curl_str;' + $nl + '    float silver_k;         float silver_spread;    float pad_e4[2]; // 24E-4: anexado no fim (224B)')
))
[void]$m.Add(@(
'            sky_params.curl_str = cloud_curl_str;',
('            sky_params.curl_str = cloud_curl_str;' + $nl + '            sky_params.silver_k = cloud_silver_k;' + $nl + '            sky_params.silver_spread = cloud_silver_spread;' + $nl + '            sky_params.pad_e4[0] = 0.0f; sky_params.pad_e4[1] = 0.0f;')
))
[void]$m.Add(@(
'        ImGui::SliderFloat("Ambiente da nuvem", &cloud_ambient_str, 0.0f, 2.0f);',
('        ImGui::SliderFloat("Ambiente da nuvem", &cloud_ambient_str, 0.0f, 2.0f);' + $nl + '        ImGui::SliderFloat("Silver lining", &cloud_silver_k, 0.0f, 1.5f);' + $nl + '        ImGui::SliderFloat("Silver spread", &cloud_silver_spread, 0.05f, 0.5f);')
))
[void]$m.Add(@(
'    f << "cloud_ambient_str=" << cloud_ambient_str << "\n";',
('    f << "cloud_ambient_str=" << cloud_ambient_str << "\n";' + $nl + '    f << "cloud_silver_k=" << cloud_silver_k << "\n";' + $nl + '    f << "cloud_silver_spread=" << cloud_silver_spread << "\n";')
))
[void]$m.Add(@(
'            else if (key == "cloud_ambient_str") cloud_ambient_str = std::stof(val);',
('            else if (key == "cloud_ambient_str") cloud_ambient_str = std::stof(val);' + $nl + '            else if (key == "cloud_silver_k") cloud_silver_k = std::stof(val);' + $nl + '            else if (key == "cloud_silver_spread") cloud_silver_spread = std::stof(val);')
))
Patch 'src\main.cpp' $m

$s = New-Object System.Collections.ArrayList
[void]$s.Add(@(
'    float curl_str; // M2.5-24C: Forca da distorcao do curl noise',
('    float curl_str; // M2.5-24C: Forca da distorcao do curl noise' + $nl + '    float cloud_silver_k;      // M2.5-24E-4' + $nl + '    float cloud_silver_spread;' + $nl + '    float pad_e4a;' + $nl + '    float pad_e4b;')
))
[void]$s.Add(@(
'    float phase_val = lerp(hg_phase(cos_sun, hg_g_frontal), hg_phase(cos_sun, -0.2f), 0.5f);',
('    // M2.5-24E-4: soma convexa normalizada + silver lining artistico via max (Nubis 2017); k=0 = fisica pura' + $nl + '    float phase_base = lerp(hg_phase(cos_sun, hg_g_frontal), hg_phase(cos_sun, -0.2f), 0.5f);' + $nl + '    float phase_val = max(phase_base, cloud_silver_k * hg_phase(cos_sun, 0.99f - cloud_silver_spread));')
))
Patch 'assets\shaders\sky.hlsl' $s

Write-Host ''
Write-Host '24E-4 aplicado. Rode .\test.ps1, abra o jogo e suba o Silver lining olhando contra o sol.'
