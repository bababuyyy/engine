# test.ps1 - o atomo: ambiente VS + compila + testa, de QUALQUER terminal
if (-not $env:VCToolsInstallDir) {
    $vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null
}
$CMAKE = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
& "$CMAKE\cmake.exe" --build "$PSScriptRoot\out\build\x64-Release"
if ($LASTEXITCODE -ne 0) { Write-Host "[test] BUILD FALHOU" -ForegroundColor Red; exit 1 }
& "$CMAKE\ctest.exe" --test-dir "$PSScriptRoot\out\build\x64-Release" --output-on-failure
if ($LASTEXITCODE -ne 0) { Write-Host "[test] TESTES VERMELHOS" -ForegroundColor Red; exit 1 }
Write-Host "[test] TUDO VERDE" -ForegroundColor Green