param(
    [switch]$AcceptNvidiaLicense,
    [switch]$DryRun
)
# Downloads the pinned build inputs from their official repositories into ..\deps.
# Nothing here is stored in this repository. The NVIDIA files are covered by the NVIDIA RTX SDKs LICENSE:
#   https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$deps = Join-Path $root 'deps'

if (-not $AcceptNvidiaLicense) {
    Write-Host 'This downloads NVIDIA DLSS SDK files (v310.9.16), which are under the NVIDIA RTX SDKs LICENSE:' -ForegroundColor Yellow
    Write-Host '  https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt'
    Write-Host 'Read it, then run again with -AcceptNvidiaLicense to confirm.'
    exit 1
}

$reshade = 'https://raw.githubusercontent.com/crosire/reshade/v6.8.0/include'
$imgui   = 'https://raw.githubusercontent.com/ocornut/imgui/v1.92.5-docking'
$nv      = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16'

$jobs = @()
foreach ($f in 'reshade.hpp','reshade_api.hpp','reshade_api_device.hpp','reshade_api_format.hpp','reshade_api_pipeline.hpp','reshade_api_resource.hpp','reshade_events.hpp','reshade_overlay.hpp') {
    $jobs += ,@("$reshade/$f", "deps\reshade\include\$f")
}
foreach ($f in 'imgui.h','imconfig.h') { $jobs += ,@("$imgui/$f", "deps\imgui\$f") }
foreach ($f in 'nvsdk_ngx.h','nvsdk_ngx_defs.h','nvsdk_ngx_helpers.h','nvsdk_ngx_helpers_cuda.h','nvsdk_ngx_helpers_d3d.h','nvsdk_ngx_params.h') {
    $jobs += ,@("$nv/include/$f", "deps\nvidia-dlss-sdk\include\$f")
}
$jobs += ,@("$nv/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib", 'deps\nvidia-dlss-sdk\lib\nvsdk_ngx_d.lib')

foreach ($j in $jobs) {
    $dest = Join-Path $root $j[1]
    if ($DryRun) { Write-Host "[dry run] $($j[0]) -> $($j[1])"; continue }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Invoke-WebRequest -Uri $j[0] -OutFile $dest -UseBasicParsing
    Write-Host "downloaded $($j[1])"
}
Write-Host 'Done. Now run build\build.bat' -ForegroundColor Green
