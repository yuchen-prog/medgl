[CmdletBinding()]
param(
    [ValidateSet('host-only', 'cuda-debug', 'cuda-release', 'v1-release')]
    [string]$Preset = 'cuda-debug',
    [string]$CudaRoot,
    [string]$VisualStudioRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Invoke-NativeCommand {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable failed with exit code $LASTEXITCODE."
    }
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$cudaCompiler = $null
if ($Preset -ne 'host-only') {
    if ($CudaRoot) {
        $cudaCompiler = Join-Path $CudaRoot 'bin\nvcc.exe'
    } elseif ($env:CUDA_PATH) {
        $CudaRoot = $env:CUDA_PATH
        $cudaCompiler = Join-Path $CudaRoot 'bin\nvcc.exe'
    } else {
        $nvccCommand = Get-Command nvcc.exe -ErrorAction SilentlyContinue
        if ($nvccCommand) {
            $cudaCompiler = $nvccCommand.Source
            $CudaRoot = Split-Path -Parent (Split-Path -Parent $cudaCompiler)
        }
    }

    if (-not $cudaCompiler -or -not (Test-Path -LiteralPath $cudaCompiler)) {
        throw ('CUDA Toolkit was not detected. Install a compatible NVIDIA CUDA ' +
            'Toolkit and reopen the terminal, or pass -CudaRoot. ' +
            'Use -Preset host-only for C++ checks only; it does not validate CUDA.')
    }
}

if (-not $VisualStudioRoot) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'vswhere was not found. Install Visual C++ tools or pass -VisualStudioRoot.'
    }
    $installationJson = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json
    if ($LASTEXITCODE -ne 0) {
        throw 'Visual Studio discovery failed.'
    }
    $installations = @($installationJson | ConvertFrom-Json)
    if ($installations.Count -eq 0) {
        throw 'A Visual Studio installation with the x64 C++ toolset was not found.'
    }
    $VisualStudioRoot = $installations[0].installationPath
}

$developerShell = Join-Path $VisualStudioRoot 'Common7\Tools\Launch-VsDevShell.ps1'
$cmakeDirectory = Join-Path $VisualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDirectory = Join-Path $VisualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$cmake = Join-Path $cmakeDirectory 'cmake.exe'
$ctest = Join-Path $cmakeDirectory 'ctest.exe'
foreach ($requiredTool in @($developerShell, $cmake, $ctest, (Join-Path $ninjaDirectory 'ninja.exe'))) {
    if (-not (Test-Path -LiteralPath $requiredTool)) {
        throw "Required Visual Studio component was not found: $requiredTool"
    }
}

& $developerShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
if (-not $env:VSCMD_VER) {
    throw 'The MSVC x64 developer environment did not initialize.'
}
$compilerDirectory = Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64'
$hostCompiler = Join-Path $compilerDirectory 'cl.exe'
if (-not (Test-Path -LiteralPath $hostCompiler) -or -not $env:INCLUDE -or -not $env:LIB) {
    throw 'The x64 MSVC compiler, INCLUDE or LIB environment is unavailable.'
}
$paths = @($compilerDirectory, $cmakeDirectory, $ninjaDirectory)
if ($CudaRoot) {
    $env:CUDA_PATH = $CudaRoot
    $paths += Join-Path $CudaRoot 'bin'
}
$paths += $env:PATH -split ';'
$uniquePaths = New-Object 'Collections.Generic.List[string]'
$seenPaths = @{}
foreach ($path in $paths) {
    $path = $path.Trim().Trim('"').TrimEnd('\')
    if ($path -and -not $seenPaths.ContainsKey($path)) { $seenPaths[$path] = $true; $uniquePaths.Add($path) }
}
$env:PATH = $uniquePaths -join ';'

Push-Location $projectRoot
try {
    $configureArguments = @('--preset', $Preset, '--fresh')
    if ($cudaCompiler) {
        $configureArguments += "-DCMAKE_CUDA_COMPILER=$cudaCompiler"
        $configureArguments += "-DCMAKE_CUDA_HOST_COMPILER=$hostCompiler"
        $configureArguments += "-DCUDAToolkit_ROOT=$CudaRoot"
        Invoke-NativeCommand $cudaCompiler @('--version')
    }
    Invoke-NativeCommand $cmake $configureArguments
    Invoke-NativeCommand $cmake @('--build', '--preset', $Preset)
    Invoke-NativeCommand $ctest @('--preset', $Preset, '--verbose')
} finally {
    Pop-Location
}