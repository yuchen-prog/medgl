[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$launcher = Join-Path $PSScriptRoot 'start-medgl.ps1'
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('medgl-launcher-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $configPath = Join-Path $temporary 'config.json'
    $valid = Get-Content -LiteralPath (Join-Path $root 'deployment\medgl.config.json') -Raw | ConvertFrom-Json
    $checks = @(
        @{ field = 'port'; value = 0 },
        @{ field = 'port'; value = 8081.5 },
        @{ field = 'pngCompression'; value = 'lossy' },
        @{ field = 'allowInstanceUidMismatch'; value = 'false' },
        @{ field = 'dataRoot'; value = '' },
        @{ field = 'dataRoot'; value = (Join-Path $temporary 'missing') }
    )
    foreach ($check in $checks) {
        $config = $valid | ConvertTo-Json | ConvertFrom-Json
        $config.($check.field) = $check.value
        $config | ConvertTo-Json | Set-Content -LiteralPath $configPath -Encoding UTF8
        $rejected = $false
        try { & $launcher -ConfigPath $configPath -CheckOnly } catch { $rejected = $true }
        if (-not $rejected) { throw 'Invalid launcher configuration was accepted.' }
    }
    $valid | Add-Member -NotePropertyName 'unknown' -NotePropertyValue $true
    $valid | ConvertTo-Json | Set-Content -LiteralPath $configPath -Encoding UTF8
    $rejected = $false
    try { & $launcher -ConfigPath $configPath -CheckOnly } catch { $rejected = $true }
    if (-not $rejected) { throw 'Unknown launcher field was accepted.' }
    $listener = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), 0
    try {
        $listener.Start()
        $busyPort = $listener.LocalEndpoint.Port
        $rejected = $false
        try { & $launcher -DataRoot $temporary -Port $busyPort -CheckOnly }
        catch { $rejected = $_.Exception.Message -eq 'Configured loopback port is unavailable. Select another port.' }
        if (-not $rejected) { throw 'Busy launcher port was accepted.' }
    } finally { $listener.Stop() }
    Write-Output 'Launcher invalid-config and occupied-port regressions passed (8 cases).'
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}