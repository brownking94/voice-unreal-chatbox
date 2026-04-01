$ErrorActionPreference = "Stop"

$modelDir = "C:\Users\vibra\voice-unreal-chatbox\models\nllb-200-distilled-600M"
$baseUrl  = "https://huggingface.co/entai2965/nllb-200-distilled-600M-ctranslate2/resolve/main"

if (!(Test-Path $modelDir)) {
    New-Item -ItemType Directory -Path $modelDir | Out-Null
}

$files = @(
    "config.json",
    "model.bin",
    "shared_vocabulary.json",
    "sentencepiece.bpe.model"
)

foreach ($file in $files) {
    $outPath = Join-Path $modelDir $file
    if (Test-Path $outPath) {
        Write-Host "Already exists: $file"
        continue
    }
    Write-Host "Downloading $file..."
    Invoke-WebRequest -Uri "$baseUrl/$file" -OutFile $outPath -UseBasicParsing
    $sizeMB = [math]::Round((Get-Item $outPath).Length / 1MB, 1)
    Write-Host "  $sizeMB MB saved"
}

Write-Host "`nDone. Model files in: $modelDir"
