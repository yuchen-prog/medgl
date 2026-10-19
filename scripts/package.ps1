[CmdletBinding()]
param([switch]$Build, [string]$CudaRoot = $env:CUDA_PATH)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Build) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Preset v1-release -CudaRoot $CudaRoot
    Push-Location (Join-Path $root 'web')
    try {
        npm ci
        if ($LASTEXITCODE -ne 0) { throw 'Frontend locked dependency installation failed.' }
        npm run build
        if ($LASTEXITCODE -ne 0) { throw 'Frontend build failed.' }
    } finally { Pop-Location }
}
$buildRoot = Join-Path $root 'out\build\v1-release'
$cacheText = Get-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Raw
function CacheValue([string]$Name) {
    $match = [regex]::Match($cacheText, '(?m)^' + [regex]::Escape($Name) + ':[^=]+=([^\r\n]+)')
    if (-not $match.Success) { throw 'Required build configuration was not found.' }
    return $match.Groups[1].Value
}
if ((CacheValue 'CMAKE_BUILD_TYPE') -ne 'Release') { throw 'Only the Release application can be packaged.' }
if (-not $CudaRoot) { $CudaRoot = CacheValue 'CUDAToolkit_ROOT' }
$compiler = CacheValue 'CMAKE_CXX_COMPILER'
$dumpbin = Join-Path (Split-Path -Parent $compiler) 'dumpbin.exe'
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmake = $cmakeCommand.Source } else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vsRoot) { throw 'CMake discovery failed.' }
    $cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json
$outputRoot = Join-Path $root 'out\packages'
$name = 'MedGL-{0}-windows-x64' -f $manifest.'version-string'
$package = Join-Path $outputRoot $name
$archive = $package + '.zip'
if ((Test-Path -LiteralPath $package) -or (Test-Path -LiteralPath $archive)) {
    throw 'Versioned package already exists. Preserve it or move it before packaging again.'
}
$stage = Join-Path $outputRoot ('.stage-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    & $cmake "-DINPUT_DIR=$buildRoot" "-DOUTPUT_DIR=$stage" "-DCUDA_BIN=$(Join-Path $CudaRoot 'bin')" "-DDUMPBIN=$dumpbin" -P (Join-Path $root 'deployment\bundle-runtime.cmake')
    if ($LASTEXITCODE -ne 0) { throw 'Runtime dependency closure could not be packaged.' }
    foreach ($folder in @('scripts','deployment','web','licenses','metadata','data')) {
        New-Item -ItemType Directory -Path (Join-Path $stage $folder) | Out-Null
    }
    foreach ($script in @('start-medgl.ps1','verify-package.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination (Join-Path $stage 'scripts')
    }
    Copy-Item -LiteralPath (Join-Path $root 'deployment\medgl.config.json') -Destination (Join-Path $stage 'deployment')
    Copy-Item -LiteralPath (Join-Path $root 'deployment\README.zh-CN.md') -Destination (Join-Path $stage 'README.zh-CN.md')
    Copy-Item -LiteralPath (Join-Path $root 'deployment\data.README.txt') -Destination (Join-Path $stage 'data\README.txt')
    $dist = Join-Path $root 'web\dist'
    if (-not (Test-Path -LiteralPath (Join-Path $dist 'index.html'))) { throw 'Frontend production build is missing.' }
    foreach ($file in Get-ChildItem -LiteralPath $dist -Recurse -File) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint -or $file.Extension -notin @('.html','.js','.css','.woff','.woff2','.png','.svg','.ico')) {
            throw 'Unexpected frontend distribution file.'
        }
        $relative = $file.FullName.Substring($dist.Length + 1)
        $destination = Join-Path (Join-Path $stage 'web') $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination
    }
    $installed = Join-Path $buildRoot 'vcpkg_installed'
    $packages = @()
    foreach ($block in ((Get-Content -LiteralPath (Join-Path $installed 'vcpkg\status') -Raw) -split '\r?\n\r?\n')) {
        if ($block -notmatch '(?m)^Status: install ok installed\r?$') { continue }
        $portName = [regex]::Match($block, '(?m)^Package: ([^\r\n]+)').Groups[1].Value
        $version = [regex]::Match($block, '(?m)^Version: ([^\r\n]+)').Groups[1].Value
        if (-not $portName) { throw 'Invalid installed dependency record.' }
        $copyright = Join-Path $installed ('x64-windows\share\{0}\copyright' -f $portName)
        if (-not (Test-Path -LiteralPath $copyright)) {
            if ($portName -ne 'boost-uninstall') { throw ('Dependency license missing: {0}' -f $portName) }
            $portMetadata = Join-Path $root 'out\tools\vcpkg\ports\boost-uninstall\vcpkg.json'
            if ((Get-Content -LiteralPath $portMetadata -Raw | ConvertFrom-Json).license -ne 'MIT') {
                throw 'Empty build port license declaration changed.'
            }
            Copy-Item -LiteralPath $portMetadata -Destination (Join-Path $stage 'licenses\build-only-boost-uninstall.json')
            Copy-Item -LiteralPath (Join-Path $root 'out\tools\vcpkg\LICENSE.txt') -Destination (Join-Path $stage 'licenses\vcpkg-MIT.txt')
        } else {
            Copy-Item -LiteralPath $copyright -Destination (Join-Path $stage ('licenses\native-{0}.txt' -f $portName))
        }
        $packages += [ordered]@{ name = $portName; version = $version; source = 'vcpkg'; scope = 'installed build dependency; may be header-only or build-only' }
    }
    $lockText = Get-Content -LiteralPath (Join-Path $root 'web\package-lock.json') -Raw
    if ($PSVersionTable.PSVersion.Major -lt 6) {
        Add-Type -AssemblyName System.Web.Extensions
        $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $serializer.MaxJsonLength = [int]::MaxValue
        $lock = $serializer.DeserializeObject($lockText)
    } else { $lock = ConvertFrom-Json -InputObject $lockText -AsHashtable }
    foreach ($entry in $lock['packages'].GetEnumerator()) {
        if (-not $entry.Key -or ($entry.Value.ContainsKey('dev') -and $entry.Value['dev'])) { continue }
        $module = Join-Path (Join-Path $root 'web') $entry.Key
        $license = @(Get-ChildItem -LiteralPath $module -File | Where-Object { $_.Name -match '^LICEN[CS]E(\..*)?$' })
        if ($license.Count -eq 0) { throw 'A frontend runtime dependency license is missing.' }
        $moduleName = $entry.Key -replace '^node_modules/', '' -replace '[/\\]', '-'
        Copy-Item -LiteralPath $license[0].FullName -Destination (Join-Path $stage ('licenses\web-{0}.txt' -f $moduleName))
        $packages += [ordered]@{ name = $moduleName; version = $entry.Value.version; source = 'npm-lock'; scope = 'frontend production dependency' }
    }
    foreach ($font in @('@fontsource-variable/ibm-plex-sans','@fontsource/ibm-plex-mono')) {
        $module = Join-Path (Join-Path $root 'web\node_modules') $font
        $fontPackage = Get-Content -LiteralPath (Join-Path $module 'package.json') -Raw | ConvertFrom-Json
        $licenseName = $font.Replace('/','-') + '.txt'
        Copy-Item -LiteralPath (Join-Path $module 'LICENSE') -Destination (Join-Path $stage ('licenses\font-' + $licenseName))
        $packages += [ordered]@{ name = $font; version = $fontPackage.version; source = 'npm-font'; scope = 'bundled font assets' }
    }
    foreach ($licenseName in @('EULA.txt','LICENSE')) {
        Copy-Item -LiteralPath (Join-Path $CudaRoot $licenseName) -Destination (Join-Path $stage ('licenses\NVIDIA-{0}.txt' -f $licenseName.Replace('.txt','')))
    }
    foreach ($file in @('vcpkg.json','CMakePresets.json','web\package-lock.json')) {
        Copy-Item -LiteralPath (Join-Path $root $file) -Destination (Join-Path $stage 'metadata')
    }
    $sources = @()
    foreach ($folder in @('src','include','scripts','deployment','tests','web\src','web\public','web\tests')) {
        $sourceRoot = Join-Path $root $folder
        if (Test-Path -LiteralPath $sourceRoot) { $sources += Get-ChildItem -LiteralPath $sourceRoot -Recurse -File }
    }
    foreach ($file in @('CMakeLists.txt','CMakePresets.json','vcpkg.json','web\index.html','web\package.json','web\package-lock.json')) {
        $sources += Get-Item -LiteralPath (Join-Path $root $file)
    }
    $sources += Get-ChildItem -LiteralPath (Join-Path $root 'web') -File | Where-Object { $_.Name -match '^(playwright\..*\.ts|tsconfig.*\.json|vite\.config\.ts|eslint\.config\.js)$' }
    $sourceHashes = @($sources | Sort-Object FullName -Unique | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($root.Length + 1).Replace('\','/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
    $sourceHashes | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage 'metadata\source-inputs.json') -Encoding UTF8
    [ordered]@{
        version = $manifest.'version-string'; platform = 'windows-x64'; configuration = 'Release'
        vcpkgBaseline = $manifest.'builtin-baseline'; cudaArchitectures = (CacheValue 'CMAKE_CUDA_ARCHITECTURES')
        compilerVersion = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
        cudaToolkit = (Get-Content -LiteralPath (Join-Path $CudaRoot 'version.json') -Raw | ConvertFrom-Json).cuda.version
        dependencies = $packages; includesPatientData = $false; licenseReviewComplete = $false
        reproducibility = 'Locked inputs and hashes; ZIP timestamps and compiler artifacts are not claimed bit-reproducible.'
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'metadata\build.json') -Encoding UTF8
    $hashes = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($stage.Length + 1).Replace('\','/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
    @{ version = 1; files = $hashes } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'files.sha256.json') -Encoding UTF8
    & (Join-Path $stage 'scripts\verify-package.ps1') -PackageRoot $stage
    Move-Item -LiteralPath $stage -Destination $package
    Compress-Archive -Path (Join-Path $package '*') -DestinationPath $archive -CompressionLevel Optimal
    (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash | Set-Content -LiteralPath ($archive + '.sha256') -Encoding ASCII
    Write-Output ('Package created: {0}' -f $archive)
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}