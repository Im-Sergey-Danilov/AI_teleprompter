<#
  Downloads a whisper.cpp ggml model next to call_assist.exe.

    .\scripts\get-model.ps1                    # base.en  (~148 MB, fast)
    .\scripts\get-model.ps1 -Model small       # ~488 MB
    .\scripts\get-model.ps1 -Model large-v3-turbo   # ~1.6 GB, best quality

  Point Settings > Speech-to-text > Model file at the file it writes.
#>
param(
    [string]$Model = "base.en",
    [string]$OutDir = "$PSScriptRoot\..\models"
)

$ErrorActionPreference = "Stop"
$url  = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$Model.bin"
$null = New-Item -ItemType Directory -Force -Path $OutDir
$dest = Join-Path (Resolve-Path $OutDir) "ggml-$Model.bin"

if (Test-Path $dest) {
    Write-Host "Already present: $dest"
    exit 0
}

Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
Write-Host "Saved to $dest"
