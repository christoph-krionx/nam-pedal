$ErrorActionPreference = 'Stop'
$previousPath = $env:PATH
try {
    $env:PATH = 'C:\Program Files\DaisyToolchain\bin;C:\msys64\usr\bin;' + $previousPath
    Push-Location -LiteralPath (Join-Path $PSScriptRoot '..\..\libDaisy')
    try {
        & make -j 4 'CC=arm-none-eabi-gcc -pipe' 'CXX=arm-none-eabi-g++ -pipe'
        if ($LASTEXITCODE -ne 0) { throw 'libDaisy-Build fehlgeschlagen.' }
    } finally { Pop-Location }
    Push-Location -LiteralPath $PSScriptRoot
    try {
        & make -j 4
        if ($LASTEXITCODE -ne 0) { throw 'Firmware-Build fehlgeschlagen.' }
    } finally { Pop-Location }
} finally { $env:PATH = $previousPath }
