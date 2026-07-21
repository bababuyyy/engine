# test.ps1 - o atomo automatizado: compila e testa, sempre nessa ordem
$CMAKE = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
& "$CMAKE\cmake.exe" --build "$PSScriptRoot\out\build\x64-Release"
if ($LASTEXITCODE -ne 0) { Write-Host "[test] BUILD FALHOU" -ForegroundColor Red; exit 1 }
& "$CMAKE\ctest.exe" --test-dir "$PSScriptRoot\out\build\x64-Release" --output-on-failure
if ($LASTEXITCODE -ne 0) { Write-Host "[test] TESTES VERMELHOS" -ForegroundColor Red; exit 1 }
Write-Host "[test] TUDO VERDE" -ForegroundColor Green