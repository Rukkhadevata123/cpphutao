#requires -Version 5.1
<#
.SYNOPSIS
Build the native x64 launcher and injected DLL with portable LLVM-MinGW.
.DESCRIPTION
Requires Windows 10/11 x64 and PowerShell 5.1 or newer. No MSYS2 or Git needed.
Downloads a pinned UCRT toolchain and MinHook source ZIP into a cache under TEMP.
Reuses the cache on later runs. Builds in a unique TEMP directory, removes the
working directory on exit, and publishes the release package into artifacts.
.EXAMPLE
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or
    -not [Environment]::Is64BitOperatingSystem) {
    throw 'This script requires 64-bit Windows.'
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory)][string]$Program,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Program $($Arguments -join ' ')"
    }
}

function Get-CachedArchive {
    param(
        [Parameter(Mandatory)][string]$Uri,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$Sha256,
        [Parameter(Mandatory)][string]$WorkDirectory
    )

    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -eq $Sha256) {
            Write-Host "Reusing archive: $Destination"
            return
        }
        Write-Warning "Cached archive checksum mismatch; downloading again: $Destination"
    }

    # An interrupted download never becomes a reusable cache entry.
    $downloadPath = Join-Path $WorkDirectory ([IO.Path]::GetFileName($Destination))
    Write-Host "Downloading: $Uri"
    Invoke-WebRequest -Uri $Uri -OutFile $downloadPath -UseBasicParsing
    $actualHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash
    if ($actualHash -ne $Sha256) {
        throw "SHA256 mismatch for $Uri. Expected $Sha256, got $actualHash."
    }
    Move-Item -LiteralPath $downloadPath -Destination $Destination -Force
}

$projectRoot = $PSScriptRoot
foreach ($file in @('hutao-minhook-ng.cpp', 'gi-toolkit-ng.cpp', 'config.ini', 'LICENSE')) {
    $path = Join-Path $projectRoot $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required project file is missing: $path"
    }
}

$toolchainVersion = '20261104'
$toolchainName = "llvm-mingw-$toolchainVersion-ucrt-x86_64"
$toolchainSha256 = '552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f'
$toolchainUri = "https://github.com/mstorsjo/llvm-mingw/releases/download/$toolchainVersion/$toolchainName.zip"
$minHookTag = 'v1.3.4'
$minHookCommit = 'c3fcafdc10146beb5919319d0683e44e3c30d537'
$minHookSha256 = 'cdcb160f734d81bd4d235dfea79e3f5a661c8ef0ab74fa814272aa5449069034'
$minHookUri = "https://github.com/TsudaKageyu/minhook/archive/$minHookCommit.zip"

$tempDirectory = [IO.Path]::GetTempPath()
$cacheDirectory = Join-Path $tempDirectory 'hutao-minhook-ng-cpp-cache'
$workDirectory = Join-Path $tempDirectory ('hutao-minhook-ng-cpp-' + [Guid]::NewGuid().ToString('N'))
$toolchainDirectory = Join-Path $cacheDirectory $toolchainName
$toolchainBin = Join-Path $toolchainDirectory 'bin'
$clang = Join-Path $toolchainBin 'x86_64-w64-mingw32-clang.exe'
$clangxx = Join-Path $toolchainBin 'x86_64-w64-mingw32-clang++.exe'
$archiver = Join-Path $toolchainBin 'llvm-ar.exe'
$oldPath = $env:Path
$oldSecurityProtocol = [Net.ServicePointManager]::SecurityProtocol
$locationPushed = $false

New-Item -ItemType Directory -Path $workDirectory | Out-Null
try {
    New-Item -ItemType Directory -Path $cacheDirectory -Force | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = $oldSecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Push-Location -LiteralPath $workDirectory
    $locationPushed = $true
    Write-Host "Build directory: $workDirectory"
    Write-Host "Download cache: $cacheDirectory"

    if (-not (Test-Path -LiteralPath $toolchainDirectory -PathType Container)) {
        $toolchainArchive = Join-Path $cacheDirectory "$toolchainName.zip"
        Get-CachedArchive -Uri $toolchainUri -Destination $toolchainArchive `
            -Sha256 $toolchainSha256 -WorkDirectory $workDirectory

        Write-Host "Extracting: $toolchainName"
        $unpackDirectory = Join-Path $workDirectory 'toolchain'
        Expand-Archive -LiteralPath $toolchainArchive -DestinationPath $unpackDirectory
        $unpackedToolchain = Join-Path $unpackDirectory $toolchainName
        foreach ($relativeTool in @(
            'bin\x86_64-w64-mingw32-clang.exe',
            'bin\x86_64-w64-mingw32-clang++.exe',
            'bin\llvm-ar.exe',
            'share\libc++\v1\std.cppm'
        )) {
            $tool = Join-Path $unpackedToolchain $relativeTool
            if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
                throw "Required tool was not found in the archive: $tool"
            }
        }
        # Publish the toolchain only after extraction has completed successfully.
        Move-Item -LiteralPath $unpackedToolchain -Destination $toolchainDirectory
    }
    else {
        Write-Host "Reusing toolchain: $toolchainDirectory"
    }

    foreach ($tool in @($clang, $clangxx, $archiver)) {
        if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
            throw "Cached toolchain is incomplete: $tool. Remove '$toolchainDirectory' and rerun."
        }
    }
    $env:Path = "$toolchainBin;$oldPath"
    Invoke-Checked $clangxx @('--version')

    $minHookArchive = Join-Path $cacheDirectory "minhook-$minHookCommit.zip"
    Get-CachedArchive -Uri $minHookUri -Destination $minHookArchive `
        -Sha256 $minHookSha256 -WorkDirectory $workDirectory
    Expand-Archive -LiteralPath $minHookArchive -DestinationPath $workDirectory
    $minHookSource = Join-Path $workDirectory "minhook-$minHookCommit"
    Write-Host "Using MinHook: $minHookTag ($minHookCommit)"

    $includePaths = @(
        (Join-Path $minHookSource 'include'),
        (Join-Path $minHookSource 'src'),
        (Join-Path $minHookSource 'src\hde')
    )
    $cSources = @(
        'src\buffer.c',
        'src\hook.c',
        'src\trampoline.c',
        'src\hde\hde64.c'
    )
    $objects = @()

    foreach ($relativeSource in $cSources) {
        $source = Join-Path $minHookSource $relativeSource
        $objectName = [IO.Path]::GetFileNameWithoutExtension($relativeSource) + '.o'
        $object = Join-Path $workDirectory $objectName
        $arguments = @('-Os', '-DNDEBUG', '-ffunction-sections', '-fdata-sections')
        foreach ($includePath in $includePaths) {
            $arguments += "-I$includePath"
        }
        $arguments += @('-c', $source, '-o', $object)
        Invoke-Checked $clang $arguments
        $objects += $object
    }

    $minHookLibrary = Join-Path $workDirectory 'libMinHook.a'
    Invoke-Checked $archiver (@('rcs', $minHookLibrary) + $objects)

    $stagingDirectory = Join-Path $workDirectory 'artifacts'
    New-Item -ItemType Directory -Path $stagingDirectory | Out-Null
    $cppFlags = @(
        '-std=c++23',
        '-Os',
        '-DNDEBUG',
        '-ffunction-sections',
        '-fdata-sections',
        '-fms-extensions',
        '-DUNICODE',
        '-D_UNICODE',
        '-DWIN32_LEAN_AND_MEAN',
        '-DNOMINMAX',
        '-D_WIN32_WINNT=0x0A00',
        '-DWINVER=0x0A00'
    )
    $stdSource = Join-Path $toolchainDirectory 'share\libc++\v1\std.cppm'
    Invoke-Checked $clangxx ($cppFlags + @(
        '-Wno-reserved-module-identifier', '-c', $stdSource, '-fmodule-output=std.pcm', '-o', 'std.o'
    ))

    $targetFlags = $cppFlags + @('-fmodule-file=std=std.pcm', '-static', '-Wl,--gc-sections,--no-insert-timestamp', '-s')

    $dllPath = Join-Path $stagingDirectory 'hutao-minhook-ng.dll'
    $dllArguments = $targetFlags + @(
        '-shared',
        (Join-Path $projectRoot 'hutao-minhook-ng.cpp'),
        'std.o',
        $minHookLibrary,
        '-ladvapi32',
        '-luser32',
        '-o', $dllPath
    )
    Invoke-Checked $clangxx $dllArguments

    $launcherPath = Join-Path $stagingDirectory 'gi-toolkit-ng.exe'
    $launcherArguments = $targetFlags + @(
        '-municode',
        '-mwindows',
        (Join-Path $projectRoot 'gi-toolkit-ng.cpp'),
        'std.o',
        '-luser32',
        '-o', $launcherPath
    )
    Invoke-Checked $clangxx $launcherArguments

    foreach ($file in @('config.ini', 'LICENSE')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $stagingDirectory
    }
    Copy-Item -LiteralPath (Join-Path $minHookSource 'LICENSE.txt') `
        -Destination (Join-Path $stagingDirectory 'MINHOOK-LICENSE.txt')

    # Publish only after both programs and all package files have been produced.
    $artifactsDirectory = Join-Path $projectRoot 'artifacts'
    New-Item -ItemType Directory -Path $artifactsDirectory -Force | Out-Null
    foreach ($file in @('gi-toolkit-ng.exe', 'hutao-minhook-ng.dll', 'config.ini', 'LICENSE', 'MINHOOK-LICENSE.txt')) {
        Copy-Item -LiteralPath (Join-Path $stagingDirectory $file) -Destination $artifactsDirectory -Force
    }
    Write-Host "Release package ready: $artifactsDirectory"
}
finally {
    $env:Path = $oldPath
    [Net.ServicePointManager]::SecurityProtocol = $oldSecurityProtocol
    if ($locationPushed) {
        Pop-Location
    }
    try {
        Remove-Item -LiteralPath $workDirectory -Recurse -Force
    }
    catch {
        Write-Warning "Could not remove build directory '$workDirectory': $_"
    }
}
