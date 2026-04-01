$ErrorActionPreference = "Stop"

$modelsDir = "C:\Users\vibra\voice-unreal-chatbox\models"

$pairs = @(
    @{ key = "en-ja"; repo = "gaudi/opus-mt-en-jap-ctranslate2" },
    @{ key = "ja-en"; repo = "gaudi/opus-mt-ja-en-ctranslate2" }
)

$files = @("config.json", "model.bin", "shared_vocabulary.json", "source.spm", "target.spm", "vocabulary.json")

foreach ($pair in $pairs) {
    $outDir = Join-Path $modelsDir "opus-mt-$($pair.key)"
    $baseUrl = "https://huggingface.co/$($pair.repo)/resolve/main"

    if (!(Test-Path $outDir)) {
        New-Item -ItemType Directory -Path $outDir | Out-Null
    }

    Write-Host "`nDownloading $($pair.key) from $($pair.repo)..."

    foreach ($file in $files) {
        $outPath = Join-Path $outDir $file
        if (Test-Path $outPath) {
            Write-Host "  Already exists: $file"
            continue
        }
        try {
            Write-Host "  Downloading $file..."
            Invoke-WebRequest -Uri "$baseUrl/$file" -OutFile $outPath -UseBasicParsing
            $sizeMB = [math]::Round((Get-Item $outPath).Length / 1MB, 1)
            Write-Host "    $sizeMB MB"
        } catch {
            Write-Host "    Skipped (not found)"
            if (Test-Path $outPath) { Remove-Item $outPath }
        }
    }
}

Write-Host "`nDone. Models in:"
Get-ChildItem "$modelsDir\opus-mt-*" -Directory | ForEach-Object { Write-Host "  $($_.FullName)" }
