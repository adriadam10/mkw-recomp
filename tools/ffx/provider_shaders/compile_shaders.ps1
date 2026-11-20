#!/usr/bin/env pwsh
#
# Compile the pinned FidelityFX 3.1.6 frame-interpolation and optical-flow
# shaders into the FP32/Wave32 permutations embedded by the standalone DLL.
# Explicit Vulkan storage-image formats from the parent patch make every
# resource declaration visible to validation.

param(
    [Parameter(Mandatory)]
    [string]$BuildRoot,
    [Parameter(Mandatory)]
    [string]$Dxc,
    [Parameter(Mandatory)]
    [string]$SpirvVal,
    [Parameter(Mandatory)]
    [string]$SpirvDis,
    [int]$MaxParallel = 4
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$buildRoot = [IO.Path]::GetFullPath($BuildRoot)
$sdkRoot = Join-Path $buildRoot 'src\Kits\FidelityFX'
$shaderRoot = Join-Path $sdkRoot 'framegeneration\fsr3\internal\shaders'
$outputRoot = Join-Path $buildRoot 'shaders\spv\fp32-wave32'
$hdrOutputRoot = Join-Path $buildRoot 'shaders\spv\fp32-wave32-hdr'

foreach ($tool in @($Dxc, $SpirvVal, $SpirvDis)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "shader tool not found: $tool"
    }
}

New-Item -ItemType Directory -Force -Path $outputRoot, $hdrOutputRoot | Out-Null
Get-ChildItem -LiteralPath $outputRoot, $hdrOutputRoot -Filter '*.spv*' -File |
    Remove-Item -Force

$commonDefines = @(
    '-DFFX_GPU=1'
    '-DFFX_HLSL=1'
    '-DFFX_HALF=0'
)
$fiDefines = @(
    '-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1'
    '-DFFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS=0'
    '-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0'
)
$sdrOfDefines = @(
    '-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0'
)
$hdrOfDefines = @(
    '-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=1'
)
$includeFlags = @(
    '-I', (Join-Path $sdkRoot 'api\internal\gpu')
    '-I', (Join-Path $sdkRoot 'framegeneration\fsr3\include')
    '-I', (Join-Path $sdkRoot 'framegeneration\fsr3\include\gpu')
    '-I', $shaderRoot
)
$bindingShifts = @(
    '-fvk-t-shift', '0', '0'
    '-fvk-u-shift', '64', '0'
    '-fvk-b-shift', '128', '0'
    '-fvk-s-shift', '320', '0'
)

$fiShaders = @(
    'ffx_frameinterpolation_reconstruct_and_dilate_pass.hlsl'
    'ffx_frameinterpolation_disocclusion_mask_pass.hlsl'
    'ffx_frameinterpolation_reconstruct_previous_depth_pass.hlsl'
    'ffx_frameinterpolation_setup_pass.hlsl'
    'ffx_frameinterpolation_game_motion_vector_field_pass.hlsl'
    'ffx_frameinterpolation_optical_flow_vector_field_pass.hlsl'
    'ffx_frameinterpolation_pass.hlsl'
    'ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.hlsl'
    'ffx_frameinterpolation_compute_inpainting_pyramid_pass.hlsl'
    'ffx_frameinterpolation_inpainting_pass.hlsl'
    'ffx_frameinterpolation_debug_view_pass.hlsl'
)
$ofShaders = @(
    'ffx_opticalflow_compute_luminance_pyramid_pass.hlsl'
    'ffx_opticalflow_compute_optical_flow_advanced_pass_v5.hlsl'
    'ffx_opticalflow_compute_scd_divergence_pass.hlsl'
    'ffx_opticalflow_filter_optical_flow_pass_v5.hlsl'
    'ffx_opticalflow_generate_scd_histogram_pass.hlsl'
    'ffx_opticalflow_prepare_luma_pass.hlsl'
    'ffx_opticalflow_scale_optical_flow_advanced_pass_v5.hlsl'
)

$jobs = @()
foreach ($name in $fiShaders) {
    $jobs += [pscustomobject]@{
        Name = $name
        Defines = $fiDefines
        OutputRoot = $outputRoot
    }
}
foreach ($name in $ofShaders) {
    $jobs += [pscustomobject]@{
        Name = $name
        Defines = $sdrOfDefines
        OutputRoot = $outputRoot
    }
    $jobs += [pscustomobject]@{
        Name = $name
        Defines = $hdrOfDefines
        OutputRoot = $hdrOutputRoot
    }
}

$jobs | ForEach-Object -Parallel {
    $job = $_
    $dxc = $using:Dxc
    $shaderRoot = $using:shaderRoot
    $commonDefines = $using:commonDefines
    $includeFlags = $using:includeFlags
    $bindingShifts = $using:bindingShifts
    $spirvVal = $using:SpirvVal
    $spirvDis = $using:SpirvDis

    $source = Join-Path $shaderRoot $job.Name
    $stem = [IO.Path]::GetFileNameWithoutExtension($job.Name)
    $spv = Join-Path $job.OutputRoot ($stem + '.spv')
    $dxcArgs = @(
        '-spirv'
        '-fvk-use-dx-layout'
        '-fspv-use-unknown-image-format'
    ) + $bindingShifts + @(
        '-T', 'cs_6_6'
        '-E', 'CS'
        '-WX'
        '-O3'
        '-fspv-target-env=vulkan1.2'
    ) + $commonDefines + $job.Defines + $includeFlags + @(
        '-Fo', $spv
        $source
    )

    & $dxc @dxcArgs
    if ($LASTEXITCODE -ne 0) {
        throw "DXC failed for $($job.Name): $LASTEXITCODE"
    }

    & $spirvVal '--target-env' 'vulkan1.2' $spv
    if ($LASTEXITCODE -ne 0) {
        throw "spirv-val failed for $($job.Name): $LASTEXITCODE"
    }

    & $spirvDis '--raw-id' $spv '-o' ($spv + '.spvasm')
    if ($LASTEXITCODE -ne 0) {
        throw "spirv-dis failed for $($job.Name): $LASTEXITCODE"
    }
} -ThrottleLimit $MaxParallel

$sdrCount = @(Get-ChildItem -LiteralPath $outputRoot -Filter '*.spv' -File).Count
$hdrCount = @(Get-ChildItem -LiteralPath $hdrOutputRoot -Filter '*.spv' -File).Count
if ($sdrCount -ne 18 -or $hdrCount -ne 7) {
    throw "expected 18 SDR and 7 HDR blobs, found $sdrCount and $hdrCount"
}
Write-Host "compiled and validated 25 Vulkan SPIR-V shader blobs"
