<#
.SYNOPSIS
    Build DataCrusher for multiple platforms (Windows/Linux x AMD64/ARM64).

.DESCRIPTION
    Compiles the DataCrusher PostgreSQL CDC Engine using CMake.
    Output files are placed in Tools\x64\ with platform-tagged names:
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

$ProjectDir  = $PSScriptRoot
$OutputDir   = Join-Path $ProjectDir '..\x64'
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
        $version = & $vsWhere -latest -property catalog_productLineVersion 2>$null
        if ($version -eq '2022') { return 'Visual Studio 17 2022' }
        if ($version -eq '2019') { return 'Visual Studio 16 2019' }
    }
    if (Test-Command 'ninja') { return 'Ninja' }
    return 'Visual Studio 17 2022'
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

function Ensure-VcpkgLibpq([string]$Arch) {
    $triplet   = "$Arch-windows-static"
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

    # Check if static libpq is already installed
    $installedDir = Join-Path $vcpkgRoot "installed\$triplet"
    $pqHeader     = Join-Path $installedDir "include\libpq-fe.h"
    if (-not (Test-Path $pqHeader)) {
        Write-Step "Building static libpq via vcpkg ($triplet) -- first run may take several minutes"
        $ErrorActionPreference = 'Continue'
        & $vcpkgExe install "libpq[core]:$triplet" --no-print-usage 2>&1 | ForEach-Object { Write-Host "  $_" }
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

    # Build static libpq via vcpkg (no SSL, no compression)
    $pgStaticRoot = Ensure-VcpkgLibpq $Arch
    if (-not $pgStaticRoot) {
        Write-Fail "$Tag -- Static libpq not available. Cannot proceed with static build."
        return $false
    }

    Write-Step "Building $Tag (static)"
    Write-Host "  Static libpq: $pgStaticRoot"

    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    }

    $Generator = Get-CMakeGenerator
    $cmakeArch = if ($Arch -eq 'arm64') { 'ARM64' } else { 'x64' }

    $configArgs = @(
        '-S', $ProjectDir,
        '-B', $BuildDir,
        "-DDC_PLATFORM_TAG=$Tag",
        "-DPOSTGRESQL_STATIC_ROOT=$pgStaticRoot",
        "-DDC_STATIC_LINK=ON",
        "-DDC_STATIC_RUNTIME=ON",
        "-DCMAKE_BUILD_TYPE=Release"
    )

    if ($Generator -like 'Visual Studio*') {
        $configArgs += @('-G', $Generator, '-A', $cmakeArch)
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
        $dockerPlatform = $null
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
$setupCmd && $pgBuildCmd && $pgConfigureCmd && $pgMakeCmd && cd /src && cmake -S . -B /build -DDC_PLATFORM_TAG=$Tag -DCMAKE_BUILD_TYPE=Release -DDC_STATIC_LINK=ON $toolchainArg && cmake --build /build --config Release --parallel && ls -la /x64/
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
        '-v', "${outputDirUnix}:/x64",
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
