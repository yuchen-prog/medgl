[CmdletBinding()]
param(
    [string]$ConfigPath,
    [string]$DataRoot,
    [ValidateRange(1, 65535)][int]$Port,
    [ValidateSet('default', 'fast')][string]$PngCompression,
    [switch]$AllowInstanceUidMismatch,
    [switch]$CheckOnly
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $ConfigPath) { $ConfigPath = Join-Path $root 'deployment\medgl.config.json' }
if (-not [Environment]::Is64BitProcess -or $env:OS -ne 'Windows_NT') {
    throw 'MedGL requires Windows x64 PowerShell.'
}
if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) { throw 'Configuration file was not found.' }
$config = Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json
$required = @('port', 'dataRoot', 'pngCompression', 'allowInstanceUidMismatch')
$names = @($config.PSObject.Properties.Name)
if (@(Compare-Object $required $names).Count -ne 0) { throw 'Configuration has missing or unknown fields.' }
if (($config.port -isnot [int] -and $config.port -isnot [long]) -or $config.port -lt 1 -or $config.port -gt 65535) {
    throw 'Configuration port must be an integer between 1 and 65535.'
}
if ($config.dataRoot -isnot [string] -or [string]::IsNullOrWhiteSpace($config.dataRoot)) {
    throw 'Configuration dataRoot must be a nonempty directory.'
}
if ($config.pngCompression -isnot [string] -or $config.pngCompression -notin @('default', 'fast')) {
    throw 'Configuration pngCompression must be default or fast.'
}
if ($config.allowInstanceUidMismatch -isnot [bool]) { throw 'Configuration UID compatibility must be a Boolean.' }
if ($PSBoundParameters.ContainsKey('Port')) { $config.port = $Port }
if ($PSBoundParameters.ContainsKey('PngCompression')) { $config.pngCompression = $PngCompression }
if ($PSBoundParameters.ContainsKey('AllowInstanceUidMismatch')) {
    $config.allowInstanceUidMismatch = [bool]$AllowInstanceUidMismatch
}
if ($PSBoundParameters.ContainsKey('DataRoot')) {
    if ([string]::IsNullOrWhiteSpace($DataRoot)) { throw 'DataRoot must be a nonempty directory.' }
    $config.dataRoot = $DataRoot
}
$data = if ([IO.Path]::IsPathRooted($config.dataRoot)) { $config.dataRoot } else { Join-Path $root $config.dataRoot }
if (-not (Test-Path -LiteralPath $data -PathType Container)) { throw 'Configured DICOM directory does not exist.' }
$data = (Resolve-Path -LiteralPath $data).ProviderPath
$packaged = Test-Path -LiteralPath (Join-Path $root 'bin\medgl_server.exe') -PathType Leaf
$bin = if ($packaged) { Join-Path $root 'bin' } else { Join-Path $root 'out\build\v1-release' }
$web = if ($packaged) { Join-Path $root 'web' } else { Join-Path $root 'web\dist' }
$server = Join-Path $bin 'medgl_server.exe'
$probe = Join-Path $bin 'medgl_cuda_probe.exe'
foreach ($file in @($server, $probe, (Join-Path $web 'index.html'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw 'Runtime or web assets are incomplete. Build or extract the complete package.' }
}
$webPath = [IO.Path]::GetFullPath($web).TrimEnd('\') + '\'
$dataPath = [IO.Path]::GetFullPath($data).TrimEnd('\') + '\'
if ($dataPath.StartsWith($webPath, [StringComparison]::OrdinalIgnoreCase) -or
    $webPath.StartsWith($dataPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'DICOM and web directories must not contain one another.'
}
$listener = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), ([int]$config.port)
try { $listener.Start() }
catch { throw 'Configured loopback port is unavailable. Select another port.' }
finally { $listener.Stop() }
& $probe
if ($LASTEXITCODE -ne 0) { throw 'CUDA preflight failed. Verify the NVIDIA driver, supported GPU and runtime dependencies.' }
Write-Output ('Preflight passed. URL: http://127.0.0.1:{0}/; PNG: {1}; UID compatibility: {2}' -f
    $config.port, $config.pngCompression, $config.allowInstanceUidMismatch)
if ($CheckOnly) { return }
$arguments = @('--port', [string]$config.port, '--data-root', $data, '--web-root', $web,
    '--png-compression', $config.pngCompression)
if ($config.allowInstanceUidMismatch) {
    Write-Warning 'Read-only instance UID compatibility was explicitly enabled; files will not be repaired or rewritten.'
    $arguments += '--allow-instance-uid-mismatch'
}
& $server @arguments
if ($LASTEXITCODE -ne 0) { throw ('MedGL service exited with code {0}.' -f $LASTEXITCODE) }