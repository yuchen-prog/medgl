[CmdletBinding()]
param(
    [string]$Archive,
    [ValidateRange(1, 65535)][int]$Port = 8091,
    [ValidateRange(1, 100)][int]$Cycles = 100
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Archive) { $Archive = Join-Path $root 'out\packages\MedGL-0.1.0-windows-x64.zip' }
$expected = (Get-Content -LiteralPath ($Archive + '.sha256') -Raw).Trim()
if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $expected) { throw 'Archive SHA256 mismatch.' }
$directory = Join-Path $root ('out\validation\package-' + [guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $Archive -DestinationPath $directory
& (Join-Path $directory 'scripts\verify-package.ps1') -PackageRoot $directory
function Expect-IntegrityFailure([string]$ExpectedMessage) {
    $rejected = $false
    try { & (Join-Path $directory 'scripts\verify-package.ps1') -PackageRoot $directory }
    catch { $rejected = $_.Exception.Message -eq $ExpectedMessage }
    if (-not $rejected) { throw 'Corrupted package integrity regression failed.' }
}
$fixture = Join-Path $directory 'data\README.txt'
$original = [IO.File]::ReadAllBytes($fixture)
try {
    [IO.File]::AppendAllText($fixture, 'modified')
    Expect-IntegrityFailure 'Package file hash mismatch.'
} finally { [IO.File]::WriteAllBytes($fixture, $original) }
$extra = Join-Path $directory 'data\files.sha256.json'
try {
    [IO.File]::WriteAllText($extra, '{}')
    Expect-IntegrityFailure 'Unexpected package files found.'
} finally { Remove-Item -LiteralPath $extra }
$junction = Join-Path $directory 'data\unexpected-link'
try {
    New-Item -ItemType Junction -Path $junction -Target (Join-Path $directory 'licenses') | Out-Null
    Expect-IntegrityFailure 'Package tree must not contain links.'
} finally { if (Test-Path -LiteralPath $junction) { [IO.Directory]::Delete($junction) } }
& (Join-Path $directory 'scripts\verify-package.ps1') -PackageRoot $directory
Write-Output 'Package corruption, unlisted-manifest and directory-link regressions passed.'
$bin = Join-Path $directory 'bin'
$data = Join-Path $root 'out\validation\synthetic-ct'
$launcher = Join-Path $directory 'scripts\start-medgl.ps1'
$originalPath = $env:PATH
$server = $null
$variables = @('MEDGL_REAL_URL','MEDGL_PNG_COMPRESSION','MEDGL_PACKAGE_CYCLES')
$saved = @{}
foreach ($name in $variables) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
    Push-Location ([IO.Path]::GetTempPath())
    try {
        & $launcher -Port $Port -CheckOnly
        & $launcher -DataRoot $data -Port $Port -PngCompression fast -CheckOnly
        $arguments = @('--port', [string]$Port, '--data-root', ('"{0}"' -f $data), '--web-root',
            ('"{0}"' -f (Join-Path $directory 'web')), '--png-compression', 'fast')
        $server = Start-Process -FilePath (Join-Path $bin 'medgl_server.exe') -ArgumentList $arguments -WorkingDirectory $directory -PassThru -NoNewWindow
    } finally { Pop-Location }
    $env:PATH = $originalPath
    $env:MEDGL_REAL_URL = 'http://127.0.0.1:{0}' -f $Port
    $env:MEDGL_PNG_COMPRESSION = 'fast'
    $env:MEDGL_PACKAGE_CYCLES = [string]$Cycles
    Push-Location (Join-Path $root 'web')
    try {
        npm run test:real -- real-backend.spec.ts
        if ($LASTEXITCODE -ne 0) { throw 'Extracted package browser acceptance failed.' }
        $server.Refresh()
        $before = [ordered]@{ privateBytes = $server.PrivateMemorySize64; workingSetBytes = $server.WorkingSet64; handles = $server.HandleCount }
        npm run test:package
        if ($LASTEXITCODE -ne 0) { throw 'Extracted package lifecycle acceptance failed.' }
        $server.Refresh()
        $after = [ordered]@{ privateBytes = $server.PrivateMemorySize64; workingSetBytes = $server.WorkingSet64; handles = $server.HandleCount }
        [ordered]@{
            passed = $true; cycles = $Cycles; packageVersion = '0.1.0'; childPathOnlyWindows = $true
            separateMachine = $false; data = 'synthetic-ct-64-cubed-and-phantom'; before = $before; after = $after
            memoryScope = 'Two process snapshots around lifecycle loop, not continuous peak tracking or leak proof'
        } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'out\validation\package-process.json') -Encoding UTF8
    } finally { Pop-Location }
    Write-Output ('Extracted package browser and {0} lifecycle cycles passed; reports in out/validation.' -f $Cycles)
} finally {
    $env:PATH = $originalPath
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    if ($server) { if (-not $server.HasExited) { Stop-Process -Id $server.Id; $server.WaitForExit() }; $server.Dispose() }
}