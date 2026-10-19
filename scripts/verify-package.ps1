[CmdletBinding()]
param([string]$PackageRoot = (Split-Path -Parent $PSScriptRoot))

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $PackageRoot).ProviderPath.TrimEnd('\')
if ((Get-Item -LiteralPath $root).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Package root must not be linked.' }
$directories = New-Object 'Collections.Generic.Stack[IO.DirectoryInfo]'
$directories.Push((Get-Item -LiteralPath $root))
$allFiles = @()
while ($directories.Count -gt 0) {
    foreach ($item in Get-ChildItem -LiteralPath $directories.Pop().FullName -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Package tree must not contain links.' }
        if ($item.PSIsContainer) { $directories.Push($item) } else { $allFiles += $item }
    }
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'files.sha256.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.version -ne 1 -or @($manifest.files).Count -eq 0) { throw 'Invalid package hash manifest.' }
$seen = @{}
foreach ($entry in $manifest.files) {
    if ($entry.path -isnot [string] -or [IO.Path]::IsPathRooted($entry.path) -or
        $entry.path -match '(^|[/\\])\.\.([/\\]|$)|:' -or $entry.sha256 -notmatch '^[A-Fa-f0-9]{64}$') {
        throw 'Unsafe package hash entry.'
    }
    $file = [IO.Path]::GetFullPath((Join-Path $root $entry.path))
    if (-not $file.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) -or $seen.ContainsKey($file)) {
        throw 'Duplicate or escaping package hash entry.'
    }
    $seen[$file] = $true
    if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
        (Get-Item -LiteralPath $file).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Package file missing or linked.' }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) { throw 'Package file hash mismatch.' }
}
$manifestPath = Join-Path $root 'files.sha256.json'
$actual = @($allFiles | Where-Object { $_.FullName -ne $manifestPath })
if ($actual.Count -ne $seen.Count) { throw 'Unexpected package files found.' }
foreach ($file in $actual) { if (-not $seen.ContainsKey($file.FullName)) { throw 'Unlisted package file found.' } }
Write-Output ('Package SHA256 validation passed: {0} files. This is integrity, not publisher authentication.' -f $seen.Count)