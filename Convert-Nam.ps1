param(
    [Parameter(Mandatory = $true)][string]$InputModel,
    [string]$OutputModel
)

$ErrorActionPreference = 'Stop'
$converter = Join-Path $PSScriptRoot 'nam-binary-loader\build\nam2namb.exe'
$loader = Join-Path $PSScriptRoot 'nam-binary-loader\build\loadmodel.exe'
if (!(Test-Path -LiteralPath $converter)) {
    throw 'nam2namb.exe fehlt. Siehe WINDOWS-NAM.md zum Build.'
}
$inputPath = (Resolve-Path -LiteralPath $InputModel).Path
$model = Get-Content -LiteralPath $inputPath -Raw | ConvertFrom-Json
if (!$OutputModel) {
    $filename = if ($model.architecture -eq 'SlimmableContainer') { 'model.namb' } else { 'nano_relu.namb' }
    $OutputModel = Join-Path (Split-Path $inputPath) $filename
}
$outputPath = [System.IO.Path]::GetFullPath($OutputModel)
if (Test-Path -LiteralPath $outputPath) {
    throw "Ausgabedatei existiert bereits: $outputPath. Bitte anderen Namen angeben."
}
if ($model.architecture -eq 'SlimmableContainer') {
    & 'C:\Program Files\KiCad\9.0\bin\python.exe' (Join-Path $PSScriptRoot 'a2-pod\convert_a2.py') $inputPath $outputPath
    if ($LASTEXITCODE -ne 0) { throw 'A2-Konvertierung fehlgeschlagen.' }
    Write-Host 'Diese Datei gehoert zur neuen Firmware a2-pod, SD-Dateiname: model.namb.'
    return
}
$previousPath = $env:PATH
Push-Location -LiteralPath $PSScriptRoot
try {
    $env:PATH = 'C:\msys64\ucrt64\bin;' + $previousPath
    # Avoid passing the Unicode Windows user directory to the MinGW runtime.
    $relativeInput = [System.IO.Path]::GetRelativePath($PSScriptRoot, $inputPath)
    $relativeOutput = [System.IO.Path]::GetRelativePath($PSScriptRoot, $outputPath)
    & $converter $relativeInput $relativeOutput
    if ($LASTEXITCODE -ne 0) { throw 'NAM-Konvertierung fehlgeschlagen.' }
    if (Test-Path -LiteralPath $loader) {
        & $loader $relativeOutput
        if ($LASTEXITCODE -ne 0) { throw 'Die erzeugte NAMB-Datei konnte nicht geladen werden.' }
    }
    $size = (Get-Item -LiteralPath $outputPath).Length
    if ($size -gt 4096) {
        Write-Warning "Modell hat $size Bytes und passt nicht in den 4096-Byte-Puffer der vorhandenen Firmware."
    }
    Write-Host "Erstellt: $outputPath ($size Bytes)"
    Write-Host 'Die Echtzeit-Tauglichkeit muss auf dem Daisy separat geprueft werden.'
} finally {
    Pop-Location
    $env:PATH = $previousPath
}
