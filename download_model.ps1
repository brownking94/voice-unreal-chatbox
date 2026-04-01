$url = 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin'
$out = 'C:\Users\vibra\voice-unreal-chatbox\models\ggml-small.bin'
Write-Host 'Downloading ggml-small.bin (~466 MB)...'
Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
$sizeMB = [math]::Round((Get-Item $out).Length / 1MB, 1)
Write-Host "Done. $sizeMB MB saved to $out"
