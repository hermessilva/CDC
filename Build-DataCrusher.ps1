<#
.SYNOPSIS
    Build DataCrusher for multiple platforms (Windows/Linux x AMD64/ARM64).

.DESCRIPTION
    Compiles the DataCrusher PostgreSQL CDC Engine using CMake.
    Output files are placed in .\Deploy\ with platform-tagged names:
        DataCrusher.win-x64.exe
        DataCrusher.win-arm64.exe
        DataCrusher.linux-x64
        DataCrusher.linux-arm64

    Windows builds use MSVC (requires Visual Studio Build Tools + PostgreSQL).
    Linux builds use Docker with gcc cross-compilation toolchains + libpq-dev.

.PARAMETER Targets
    Platforms to build. Defaults to all available.
    Valid values: win-x64, win-arm64, linux-x64, linux-arm64

.PARAMETER Clean
    Remove build directories before building.

.PARAMETER PostgreSqlRoot
    Path to the PostgreSQL installation (Windows only).
    Auto-detected from C:\Program Files\PostgreSQL\{18..15} if not specified.

.EXAMPLE
    .\Build-DataCrusher.ps1
    .\Build-DataCrusher.ps1 -Targets win-x64
    .\Build-DataCrusher.ps1 -Targets win-x64,linux-x64 -Clean
    .\Build-DataCrusher.ps1 -Targets win-x64 -PostgreSqlRoot "C:\Program Files\PostgreSQL\17"
#>
[CmdletBinding()]
param(
    [ValidateSet('win-x64','win-arm64','linux-x64','linux-arm64')]
    [string[]] $Targets,

    [switch] $Clean,

    [string] $PostgreSqlRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ProjectDir  = Join-Path $PSScriptRoot 'DataCrusher'
$OutputDir   = Join-Path $PSScriptRoot 'Deploy'
$BuildRoot   = Join-Path $ProjectDir 'build'

# -- Helpers -----------------------------------------------------------------

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "=== $Message ===" -ForegroundColor Cyan
}

function Write-Ok([string]$Message) {
    Write-Host "  [OK]   $Message" -ForegroundColor Green
}

function Write-Fail([string]$Message) {
    Write-Host "  [FAIL] $Message" -ForegroundColor Red
}

function Test-Command([string]$Name) {
    $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Find-VSCMake {
    if (Test-Command 'cmake') { return 'cmake' }
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $installPath = & $vsWhere -latest -property installationPath 2>$null
        if ($installPath) {
            $vsCMake = Join-Path $installPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $vsCMake) { return $vsCMake }
        }
    }
    return $null
}

function Get-CMakeGenerator {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $installVersion = & $vsWhere -latest -property installationVersion 2>$null
        if ($installVersion) {
            $major = [int]($installVersion -split '\.')[0]
            $generatorMap = @{
                18 = 'Visual Studio 18 2026'
                17 = 'Visual Studio 17 2022'
                16 = 'Visual Studio 16 2019'
                15 = 'Visual Studio 15 2017'
            }
            if ($generatorMap.ContainsKey($major)) {
                return $generatorMap[$major]
            }
        }
    }
    if (Test-Command 'ninja') { return 'Ninja' }
    return 'Visual Studio 18 2026'
}

function Get-VSArm64Toolset {
    # Returns 'MSVC'    — VC++ ARM64 cross-compiler (component: MSVC vXXX - ARM64 build tools)
    # Returns 'ClangCL' — LLVM Clang in VS targeting ARM64  (component: C++ Clang tools for Windows)
    # Returns $null     — no ARM64-capable Windows compiler found; Docker fallback will be used
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) { return $null }
    $installPath = & $vsWhere -latest -property installationPath 2>$null
    if (-not $installPath) { return $null }
    $arm64CL = Join-Path $installPath 'VC\Tools\MSVC\*\bin\Hostx64\arm64\cl.exe'
    if (Get-ChildItem $arm64CL -ErrorAction SilentlyContinue | Select-Object -First 1) { return 'MSVC' }
    $clangCL = Join-Path $installPath 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
    if (Test-Path $clangCL) { return 'ClangCL' }
    return $null
}

function Find-PostgreSQL {
    # Use parameter if provided
    if ($PostgreSqlRoot -and (Test-Path $PostgreSqlRoot)) {
        return $PostgreSqlRoot
    }
    # Auto-detect from common locations
    foreach ($ver in @(18, 17, 16, 15)) {
        $path = "C:\Program Files\PostgreSQL\$ver"
        if (Test-Path $path) { return $path }
    }
    # Check PGSQL_HOME env
    $envPath = $env:PGSQL_HOME
    if ($envPath -and (Test-Path $envPath)) { return $envPath }
    return $null
}

# -- Resolve targets ---------------------------------------------------------

if (-not $Targets -or $Targets.Count -eq 0) {
    $Targets = @('win-x64','win-arm64')
    if (Test-Command 'docker') {
        $Targets += @('linux-x64','linux-arm64')
    } else {
        Write-Host 'Docker not found -- skipping Linux targets.' -ForegroundColor Yellow
    }
}

# Ensure output directory exists
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

# -- Clean -------------------------------------------------------------------

if ($Clean -and (Test-Path $BuildRoot)) {
    Write-Step 'Cleaning build directories'
    Remove-Item -Recurse -Force $BuildRoot
    Write-Ok 'Cleaned'
}

# -- Build functions ---------------------------------------------------------

function Ensure-VcpkgLibpq([string]$Arch, [string]$Toolset = 'MSVC') {
    $triplet   = if ($Toolset -eq 'ClangCL') { "$Arch-windows-static-clang" } else { "$Arch-windows-static" }
    $vcpkgRoot = Join-Path $BuildRoot "vcpkg"
    $vcpkgExe  = Join-Path $vcpkgRoot "vcpkg.exe"

    # Clone and bootstrap vcpkg if not present
    if (-not (Test-Path $vcpkgExe)) {
        Write-Step "Setting up vcpkg for static libpq"
        if (Test-Path $vcpkgRoot) { Remove-Item -Recurse -Force $vcpkgRoot }
        $ErrorActionPreference = 'Continue'
        & git clone --depth 1 https://github.com/microsoft/vcpkg.git $vcpkgRoot 2>&1 | ForEach-Object { Write-Host "  $_" }
        $cloneExit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($cloneExit -ne 0) {
            Write-Fail "Failed to clone vcpkg"
            return $null
        }
        Push-Location $vcpkgRoot
        $ErrorActionPreference = 'Continue'
        & .\bootstrap-vcpkg.bat -disableMetrics 2>&1 | ForEach-Object { Write-Host "  $_" }
        $bootstrapExit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        Pop-Location
        if (-not (Test-Path $vcpkgExe)) {
            Write-Fail "Failed to bootstrap vcpkg (exit $bootstrapExit)"
            return $null
        }
        Write-Ok "vcpkg bootstrapped"
    }

    # For ClangCL, create a custom overlay triplet (built-in arm64-windows-static requires MSVC)
    $overlayArgs = @()
    if ($Toolset -eq 'ClangCL') {
        $tripletsDir = Join-Path $BuildRoot 'triplets'
        if (-not (Test-Path $tripletsDir)) { New-Item -ItemType Directory -Path $tripletsDir -Force | Out-Null }
        $tripletFile = Join-Path $tripletsDir "$triplet.cmake"
        if (-not (Test-Path $tripletFile)) {
            Set-Content -Path $tripletFile -Encoding UTF8 -Value (
                "set(VCPKG_TARGET_ARCHITECTURE arm64)`n" +
                "set(VCPKG_CRT_LINKAGE static)`n" +
                "set(VCPKG_LIBRARY_LINKAGE static)`n" +
                "set(VCPKG_PLATFORM_TOOLSET ClangCL)"
            )
            Write-Ok "Created ClangCL triplet: $tripletFile"
        }
        $overlayArgs = @("--overlay-triplets=$tripletsDir")
    }

    # Check if static libpq is already installed
    $installedDir = Join-Path $vcpkgRoot "installed\$triplet"
    $pqHeader     = Join-Path $installedDir "include\libpq-fe.h"
    if (-not (Test-Path $pqHeader)) {
        Write-Step "Building static libpq via vcpkg ($triplet) -- first run may take several minutes"
        $ErrorActionPreference = 'Continue'
        & $vcpkgExe install "libpq[core]:$triplet" @overlayArgs --no-print-usage 2>&1 | ForEach-Object { Write-Host "  $_" }
        $installExit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($installExit -ne 0 -or -not (Test-Path $pqHeader)) {
            Write-Fail "Failed to build static libpq via vcpkg (exit $installExit)"
            return $null
        }
        Write-Ok "Static libpq built ($triplet)"
    } else {
        Write-Ok "Static libpq already available ($triplet)"
    }

    return $installedDir
}

function Build-Windows([string]$Arch) {
    $Tag       = "win-$Arch"
    $BuildDir  = Join-Path $BuildRoot $Tag
    $CMakePath = Find-VSCMake

    if (-not $CMakePath) {
        Write-Fail "$Tag -- CMake not found. Install Visual Studio with C++ workload or add cmake to PATH."
        return $false
    }

    if (-not (Test-Command 'git')) {
        Write-Fail "$Tag -- git is required for vcpkg setup."
        return $false
    }

    # For ARM64: prefer native MSVC/ClangCL; fall back to Docker + llvm-mingw if neither is available
    $arm64Toolset = $null
    if ($Arch -eq 'arm64') {
        $arm64Toolset = Get-VSArm64Toolset
        if (-not $arm64Toolset) {
            if (Test-Command 'docker') {
                return Build-WinArm64ViaDocker
            }
            Write-Fail "$Tag -- No ARM64 compiler found and Docker is unavailable."
            Write-Host "  Fix A (native): VS Installer -> Modify -> Individual Components" -ForegroundColor Yellow
            Write-Host "    search: 'MSVC vXXX - ARM64 build tools'  OR  'C++ Clang tools for Windows'" -ForegroundColor Yellow
            Write-Host "  Fix B (cross):  install Docker Desktop (Linux containers) and retry" -ForegroundColor Yellow
            return $false
        }
        Write-Ok "ARM64 toolset: $arm64Toolset"
    }

    # Build static libpq via vcpkg (no SSL, no compression)
    $pgStaticRoot = Ensure-VcpkgLibpq $Arch $(if ($arm64Toolset) { $arm64Toolset } else { 'MSVC' })
    if (-not $pgStaticRoot) {
        Write-Fail "$Tag -- Static libpq not available. Cannot proceed with static build."
        return $false
    }

    Write-Step "Building $Tag (static)"
    Write-Host "  Static libpq: $pgStaticRoot"

    $Generator = Get-CMakeGenerator
    $cmakeArch = if ($Arch -eq 'arm64') { 'ARM64' } else { 'x64' }

    # Auto-clean stale build dir if the CMake generator changed
    $cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
    if (Test-Path $cacheFile) {
        $cachedGen = (Get-Content $cacheFile | Where-Object { $_ -match '^CMAKE_GENERATOR:' }) -replace '^CMAKE_GENERATOR:[^=]+=', ''
        if ($cachedGen -and $cachedGen.Trim() -ne $Generator) {
            Write-Host "  Generator changed ('$($cachedGen.Trim())' -> '$Generator'), clearing stale cache..." -ForegroundColor Yellow
            Remove-Item -Recurse -Force $BuildDir
        }
    }

    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    }

    $deployDirCmake = $OutputDir -replace '\\', '/'
    $configArgs = @(
        '-S', $ProjectDir,
        '-B', $BuildDir,
        "-DDC_PLATFORM_TAG=$Tag",
        "-DPOSTGRESQL_STATIC_ROOT=$pgStaticRoot",
        "-DDC_DEPLOY_DIR=$deployDirCmake",
        "-DDC_STATIC_LINK=ON",
        "-DDC_STATIC_RUNTIME=ON",
        "-DCMAKE_BUILD_TYPE=Release"
    )

    if ($Generator -like 'Visual Studio*') {
        $configArgs += @('-G', $Generator, '-A', $cmakeArch)
        if ($arm64Toolset -eq 'ClangCL') { $configArgs += @('-T', 'ClangCL') }
    } else {
        $configArgs += @('-G', $Generator)
    }

    Write-Host "  cmake $($configArgs -join ' ')"
    $ErrorActionPreference = 'Continue'
    & $CMakePath @configArgs 2>&1 | ForEach-Object { Write-Host "  $_" }
    $configExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($configExit -ne 0) {
        Write-Fail "$Tag -- CMake configure failed (exit $configExit)"
        return $false
    }

    $buildArgs = @('--build', $BuildDir, '--config', 'Release', '--parallel')
    Write-Host "  cmake $($buildArgs -join ' ')"
    $ErrorActionPreference = 'Continue'
    & $CMakePath @buildArgs 2>&1 | ForEach-Object { Write-Host "  $_" }
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($buildExit -ne 0) {
        Write-Fail "$Tag -- CMake build failed (exit $buildExit)"
        return $false
    }

    # Verify output — static build produces a single .exe with no DLLs
    $expected = Join-Path $OutputDir "DataCrusher.$Tag.exe"
    if (Test-Path $expected) {
        $size = (Get-Item $expected).Length
        Write-Ok "$Tag -- $expected ($([math]::Round($size / 1MB, 2)) MB, static)"
        return $true
    } else {
        $found = Get-ChildItem $OutputDir -Filter "DataCrusher.$Tag.*" -ErrorAction SilentlyContinue
        if ($found) {
            $fileInfo = $found | Select-Object -First 1
            Write-Ok "$Tag -- $($fileInfo.FullName) ($($fileInfo.Length) bytes)"
            return $true
        }
        Write-Fail "$Tag -- Output not found at $expected"
        return $false
    }
}

function Build-Linux([string]$Arch) {
    $Tag      = "linux-$Arch"
    $BuildDir = Join-Path $BuildRoot $Tag

    if (-not (Test-Command 'docker')) {
        Write-Fail "$Tag -- Docker is required for Linux cross-compilation."
        return $false
    }

    Write-Step "Building $Tag (Docker)"

    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    }

    # PostgreSQL version for source build (static linking)
    $pgVersion = '16.8'
    $pgBuildCmd = @(
        "cd /tmp"
        "wget -q https://ftp.postgresql.org/pub/source/v${pgVersion}/postgresql-${pgVersion}.tar.bz2"
        "tar xf postgresql-${pgVersion}.tar.bz2"
        "cd postgresql-${pgVersion}"
    ) -join ' && '

    if ($Arch -eq 'arm64') {
        # Use QEMU emulation via Docker for native ARM64 build (no cross-compilation)
        $dockerPlatform = 'linux/arm64'
        $dockerImage    = 'ubuntu:24.04'
        $setupCmd       = 'apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends cmake make gcc g++ pkg-config wget ca-certificates bzip2 && rm -rf /var/lib/apt/lists/*'

        # Build libpq from source for ARM64 natively (no SSL, no GSSAPI/LDAP)
        $pgConfigureCmd = "./configure --without-readline --without-zlib --without-gssapi --without-ldap --without-icu --without-openssl --prefix=/opt/pgsql > /dev/null 2>&1"

        $cmakeToolchain = $null
    } else {
        $dockerPlatform = 'linux/amd64'
        $dockerImage    = 'ubuntu:24.04'
        $setupCmd       = 'apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends cmake make gcc g++ pkg-config wget ca-certificates bzip2 && rm -rf /var/lib/apt/lists/*'

        # Build libpq from source for x64 (no SSL, no GSSAPI/LDAP)
        $pgConfigureCmd = "./configure --without-readline --without-zlib --without-gssapi --without-ldap --without-icu --without-openssl --prefix=/opt/pgsql > /dev/null 2>&1"

        $cmakeToolchain = $null
    }

    # Build only the libpq client library (pgcommon + pgport + libpq)
    $pgMakeCmd = @(
        'make -j$(nproc) -C src/include install > /dev/null 2>&1'
        'make -j$(nproc) -C src/common install > /dev/null 2>&1'
        'make -j$(nproc) -C src/port install > /dev/null 2>&1'
        'make -j$(nproc) -C src/interfaces/libpq install > /dev/null 2>&1'
        'cd / && rm -rf /tmp/postgresql-*'
    ) -join ' && '

    # Write toolchain file if needed
    $toolchainArg = ''
    if ($cmakeToolchain) {
        $toolchainFile = Join-Path $BuildDir 'toolchain.cmake'
        Set-Content -Path $toolchainFile -Value $cmakeToolchain -Encoding UTF8
        $toolchainArg = "-DCMAKE_TOOLCHAIN_FILE=/build/toolchain.cmake"
    }

    $dockerCmd = @"
$setupCmd && $pgBuildCmd && $pgConfigureCmd && $pgMakeCmd && cd /src && cmake -S . -B /build -DDC_PLATFORM_TAG=$Tag -DCMAKE_BUILD_TYPE=Release -DDC_STATIC_LINK=ON -DDC_DEPLOY_DIR=/deploy $toolchainArg && cmake --build /build --config Release --parallel && ls -la /deploy/
"@

    $projectDirUnix = $ProjectDir -replace '\\','/'
    $outputDirUnix  = $OutputDir -replace '\\','/'
    $buildDirUnix   = $BuildDir -replace '\\','/'

    $dockerArgs = @('run', '--rm')
    if ($dockerPlatform) {
        $dockerArgs += '--platform'
        $dockerArgs += $dockerPlatform
    }
    $dockerArgs += @(
        '-v', "${projectDirUnix}:/src:ro",
        '-v', "${outputDirUnix}:/deploy",
        '-v', "${buildDirUnix}:/build",
        $dockerImage,
        'bash', '-c', $dockerCmd
    )

    Write-Host "  docker $($dockerArgs[0..5] -join ' ') ..."
    $ErrorActionPreference = 'Continue'
    & docker @dockerArgs 2>&1 | ForEach-Object { Write-Host "  $_" }
    $dockerExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($dockerExit -ne 0) {
        Write-Fail "$Tag -- Docker build failed (exit $dockerExit)"
        return $false
    }

    $expected = Join-Path $OutputDir "DataCrusher.$Tag"
    if (Test-Path $expected) {
        $size = (Get-Item $expected).Length
        Write-Ok "$Tag -- $expected ($size bytes)"
        return $true
    } else {
        Write-Fail "$Tag -- Output not found at $expected"
        return $false
    }
}

function Build-WinArm64ViaDocker {
    $Tag      = 'win-arm64'
    $BuildDir = Join-Path $BuildRoot $Tag

    Write-Step "Building $Tag (Docker + llvm-mingw cross-compilation)"

    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    }

    # CMake toolchain file for aarch64-w64-mingw32
    $utf8NoBOM = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText(
        (Join-Path $BuildDir 'toolchain-win-arm64.cmake'),
@"
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(CMAKE_C_COMPILER   aarch64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER aarch64-w64-mingw32-clang++)
set(CMAKE_AR     aarch64-w64-mingw32-ar     CACHE STRING "")
set(CMAKE_RANLIB aarch64-w64-mingw32-ranlib CACHE STRING "")
"@, $utf8NoBOM
    )

    # Bash build script written to disk — avoids all PowerShell string-interpolation issues
    $bashScript = @'
#!/usr/bin/env bash
set -e
PG_VERSION=16.8

# Build static libpq (no SSL, no GSSAPI, no LDAP)
cd /tmp
wget -q --no-check-certificate \
  "https://ftp.postgresql.org/pub/source/v${PG_VERSION}/postgresql-${PG_VERSION}.tar.bz2"
tar xf "postgresql-${PG_VERSION}.tar.bz2"
cd "postgresql-${PG_VERSION}"
./configure \
  --build=x86_64-linux-gnu \
  --host=aarch64-w64-mingw32 \
  --without-readline --without-zlib --without-gssapi \
  --without-ldap --without-icu --without-openssl \
  --prefix=/opt/pgsql \
  CC=aarch64-w64-mingw32-clang \
  AR=aarch64-w64-mingw32-ar \
  RANLIB=aarch64-w64-mingw32-ranlib > /dev/null 2>&1
# Patch sigsetjmp/siglongjmp -> setjmp/longjmp
# (__builtin_setjmp is not supported by Clang for aarch64-w64-mingw32)
for f in src/common/pg_get_line.c src/port/pg_crc32c_armv8_choose.c; do
  sed -i '1s|^|#include <setjmp.h>\n|' "$f"
  sed -i 's/sigjmp_buf/jmp_buf/g; s/siglongjmp(/longjmp(/g' "$f"
  sed -E -i 's/sigsetjmp\(([^,]+), *[0-9]+\)/setjmp(\1)/g' "$f"
done
make -j"$(nproc)" -C src/include          install > /dev/null 2>&1
make -j"$(nproc)" -C src/common           install > /dev/null 2>&1
make -j"$(nproc)" -C src/port             install > /dev/null 2>&1
make -j"$(nproc)" -C src/interfaces/libpq install > /dev/null 2>&1
cd / && rm -rf /tmp/postgresql-*

# Build DataCrusher
cmake -S /src -B /build \
  -DCMAKE_TOOLCHAIN_FILE=/build/toolchain-win-arm64.cmake \
  -DDC_PLATFORM_TAG=win-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DDC_STATIC_LINK=ON \
  -DDC_DEPLOY_DIR=/deploy \
  -DPOSTGRESQL_STATIC_ROOT=/opt/pgsql
cmake --build /build --config Release --parallel
'@
    [System.IO.File]::WriteAllText(
        (Join-Path $BuildDir 'build-win-arm64.sh'),
        $bashScript.Replace("`r`n", "`n"),
        $utf8NoBOM
    )

    $projectDirUnix = $ProjectDir -replace '\\', '/'
    $outputDirUnix  = $OutputDir  -replace '\\', '/'
    $buildDirUnix   = $BuildDir   -replace '\\', '/'

    $dockerArgs = @(
        'run', '--rm',
        '-v', "${projectDirUnix}:/src:ro",
        '-v', "${outputDirUnix}:/deploy",
        '-v', "${buildDirUnix}:/build",
        'mstorsjo/llvm-mingw:latest',
        'bash', '/build/build-win-arm64.sh'
    )

    Write-Host "  docker run --rm -v .../src:ro -v .../deploy -v .../build mstorsjo/llvm-mingw ..."
    $ErrorActionPreference = 'Continue'
    & docker @dockerArgs 2>&1 | ForEach-Object { Write-Host "  $_" }
    $dockerExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($dockerExit -ne 0) {
        Write-Fail "$Tag -- Docker build failed (exit $dockerExit)"
        return $false
    }

    $expected = Join-Path $OutputDir 'DataCrusher.win-arm64.exe'
    if (Test-Path $expected) {
        $size = (Get-Item $expected).Length
        Write-Ok "$Tag -- $expected ($([math]::Round($size / 1MB, 2)) MB, static)"
        return $true
    }
    $found = Get-ChildItem $OutputDir -Filter 'DataCrusher.win-arm64*' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { Write-Ok "$Tag -- $($found.FullName) ($($found.Length) bytes)"; return $true }
    Write-Fail "$Tag -- Output not found at $expected"
    return $false
}

# -- Main build loop --------------------------------------------------------

$Results = @{}

foreach ($target in $Targets) {
    $os, $arch = $target -split '-', 2
    $success = switch ($os) {
        'win'   { Build-Windows $arch }
        'linux' { Build-Linux $arch }
    }
    $Results[$target] = $success
}

# -- Summary -----------------------------------------------------------------

Write-Host ""
Write-Host "=== Build Summary ===" -ForegroundColor Cyan
foreach ($kv in $Results.GetEnumerator() | Sort-Object Name) {
    if ($kv.Value) {
        Write-Ok $kv.Name
    } else {
        Write-Fail $kv.Name
    }
}

$failed = @($Results.Values | Where-Object { -not $_ }).Count
if ($failed -gt 0) {
    Write-Host ""
    Write-Host "$failed target(s) failed." -ForegroundColor Red
    exit 1
} else {
    Write-Host ""
    Write-Host "All targets built successfully." -ForegroundColor Green
    Write-Host "Output: $OutputDir" -ForegroundColor Gray
    exit 0
}
