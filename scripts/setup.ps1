$ErrorActionPreference = "Stop"

$cmake    = "C:\Program Files\CMake\bin\cmake.exe"
$ninjaDir = "C:\Users\vibra\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe"
$vsWhere  = "C:\Program Files (x86)\Microsoft Visual Studio\Installer"
$vcvars   = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
$root     = "C:\Users\vibra\voice-unreal-chatbox"

# Add vswhere + ninja to PATH so vcvars64.bat and ninja can be found
$env:PATH = "$vsWhere;$ninjaDir;$env:PATH"

# Source VS environment by capturing output of "vcvars64.bat && set"
Write-Host "Initializing Visual Studio x64 environment..."
$envDump = cmd /c "`"$vcvars`" && set" 2>&1
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

$ErrorActionPreference = "Continue"
Write-Host "cmake : $(& $cmake --version | Select-Object -First 1)"
$clOutput = cl 2>&1 | Out-String; Write-Host "cl    : $($clOutput.Trim().Split([Environment]::NewLine)[0])"
$nvccOutput = nvcc --version 2>&1 | Out-String; Write-Host "nvcc  : $($nvccOutput.Trim().Split([Environment]::NewLine)[-1])"
Write-Host "ninja : $(ninja --version)"
$ErrorActionPreference = "Stop"

# Remove stale build dir if configure previously failed mid-way
if (Test-Path "$root\build\CMakeCache.txt") {
    $cache = Get-Content "$root\build\CMakeCache.txt" -Raw
    if ($cache -notmatch "CMAKE_GENERATOR:INTERNAL=Ninja") {
        Write-Host "Removing stale build dir (wrong generator)..."
        Remove-Item -Recurse -Force "$root\build"
    }
}

# Configure with Ninja (uses cl.exe + nvcc directly, no VS CUDA toolset needed)
Write-Host "`n--- CMake Configure ---"
& $cmake -B "$root\build" -S $root `
    -G Ninja `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DCMAKE_C_COMPILER=cl `
    -DCMAKE_CXX_COMPILER=cl
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# Build (first build compiles CUDA kernels — takes 10-15 min)
Write-Host "`n--- CMake Build ---"
& $cmake --build "$root\build" --config RelWithDebInfo --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

Write-Host "`nBuild complete!"
Write-Host "Executables: $root\build\voice-server.exe and $root\build\test-client.exe"
