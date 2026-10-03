# Windows: crea il venv del motore, collega il pannello ad After Effects e abilita le estensioni non firmate.
# Con una GPU NVIDIA imposta prima DEPTHYUM_TORCH_INDEX all'indice CUDA di pytorch.org, ad esempio
#   $env:DEPTHYUM_TORCH_INDEX = "https://download.pytorch.org/whl/cu126"
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$Venv = Join-Path $Root "engine\.venv"
$VenvPy = Join-Path $Venv "Scripts\python.exe"

& $Python -m venv $Venv
& $VenvPy -m pip install --upgrade pip
if ($env:DEPTHYUM_TORCH_INDEX) {
    & $VenvPy -m pip install torch --index-url $env:DEPTHYUM_TORCH_INDEX
}
& $VenvPy -m pip install -r (Join-Path $Root "engine\requirements.txt")
& $VenvPy -m depthyum check

$ExtDir = Join-Path $env:APPDATA "Adobe\CEP\extensions"
New-Item -ItemType Directory -Force -Path $ExtDir | Out-Null
$Link = Join-Path $ExtDir "com.depthyum.panel"
if (Test-Path $Link) { Remove-Item $Link -Force -Recurse }
New-Item -ItemType Junction -Path $Link -Target (Join-Path $Root "extension") | Out-Null

foreach ($v in 9..13) {
    $key = "HKCU:\Software\Adobe\CSXS.$v"
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty -Path $key -Name PlayerDebugMode -Value "1" -PropertyType String -Force | Out-Null
}
Write-Host "Pannello collegato in $ExtDir. Riavvia After Effects: Finestra > Estensioni > Depthyum."
