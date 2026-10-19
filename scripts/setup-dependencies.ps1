[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json
$baseline = $manifest.'builtin-baseline'
$registry = Join-Path $root 'out\tools\vcpkg'

function Invoke-Checked {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE." }
}

Push-Location $root
try {
    if (-not (Test-Path -LiteralPath $registry)) {
        Invoke-Checked 'git' @('clone', '--no-checkout', 'https://github.com/microsoft/vcpkg.git', $registry)
        Invoke-Checked 'git' @('-C', $registry, 'checkout', '--detach', $baseline)
    }
    $current = & git -C $registry rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $current -ne $baseline) {
        throw 'The dependency registry does not match vcpkg.json. Use a separate matching registry; existing contents were not changed.'
    }
    $vcpkg = Join-Path $registry 'vcpkg.exe'
    if (-not (Test-Path -LiteralPath $vcpkg)) {
        Invoke-Checked (Join-Path $registry 'bootstrap-vcpkg.bat') @('-disableMetrics')
    }
    Invoke-Checked $vcpkg @('install', '--triplet', 'x64-windows')
} finally {
    Pop-Location
}