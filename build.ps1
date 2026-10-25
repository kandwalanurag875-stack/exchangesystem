# build.ps1 — Windows build driver using the MSVC toolchain (VS 2022).
#
# Usage:
#   .\build.ps1                 # build everything into .\build
#   .\build.ps1 tests           # build a single target
#   .\build.ps1 -Run tests      # build then run it
#
# The Linux/g++ path (the spec's deploy target) uses the Makefile instead.
param(
    [string]$Target = "all",
    [switch]$Run
)
$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$build = Join-Path $root "build"
New-Item -ItemType Directory -Force $build | Out-Null

# ---- import the MSVC environment once (cheap; cached per session) ------------
if (-not $env:VSCMD_ARG_HOST_ARCH) {
    $vcvars = $null
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -property installationPath
        if ($vsPath) {
            $cand = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $cand) { $vcvars = $cand }
        }
    }
    if (-not $vcvars) {
        # fall back to well-known install locations
        foreach ($ed in @("Community","Professional","Enterprise","BuildTools")) {
            $cand = "C:\Program Files\Microsoft Visual Studio\2022\$ed\VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $cand) { $vcvars = $cand; break }
        }
    }
    if (-not $vcvars) { throw "No MSVC vcvars64.bat found." }
    Write-Host "Importing MSVC env from $vcvars ..." -ForegroundColor DarkGray
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match "^(.*?)=(.*)$") {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
        }
    }
}

# Hot-path release flags. /O2 = optimise for speed, /GR- = no RTTI (the spec's
# -fno-rtti equivalent). We keep C++ exceptions (/EHsc) because the STL relies on
# them; the matching hot path never throws. /arch is left at the x64 default
# (SSE2) for portability — flip to /arch:AVX2 on a known machine for more.
$CXXFLAGS = @("/nologo", "/std:c++17", "/O2", "/GR-", "/EHsc", "/DNDEBUG",
              "/D_CRT_SECURE_NO_WARNINGS", "/I", "$root\include", "/W3")

# target name -> source file(s)
$targets = @{
    "replay"         = @("$root\apps\replay.cpp")
    "bench"          = @("$root\apps\bench.cpp")
    "bench_ds"       = @("$root\apps\bench_ds.cpp")
    "pipeline_bench" = @("$root\apps\pipeline_bench.cpp")
    "recover_demo"   = @("$root\apps\recover_demo.cpp")
    "feed_server"    = @("$root\apps\feed_server.cpp")
    "gen_itch"       = @("$root\apps\gen_itch.cpp")
    "itch_replay"    = @("$root\apps\itch_replay.cpp")
    "lobster_replay" = @("$root\apps\lobster_replay.cpp")
    "tests"          = @("$root\tests\test_order_book.cpp")
    "tests_durability" = @("$root\tests\test_durability.cpp")
    "tests_feed"     = @("$root\tests\test_feed.cpp")
    "tests_raft"     = @("$root\tests\test_raft.cpp")
    "raft_node"      = @("$root\apps\raft_node.cpp")
}

function Build-One($name) {
    $srcs = $targets[$name]
    if (-not $srcs) { throw "Unknown target '$name'. Known: $($targets.Keys -join ', ')" }
    $exist = $srcs | Where-Object { Test-Path $_ }
    if (-not $exist) { Write-Host "skip $name (no source yet)" -ForegroundColor DarkGray; return $null }
    $out = Join-Path $build "$name.exe"
    Write-Host "==> $name" -ForegroundColor Cyan
    # NOTE: trailing forward slash for the object dir — a trailing backslash
    # would escape the closing quote on the native command line.
    & cl @CXXFLAGS $exist "/Fe:$out" "/Fo:$build/" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "build failed: $name" }
    return $out
}

$built = @()
if ($Target -eq "all") {
    foreach ($t in $targets.Keys) { $r = Build-One $t; if ($r) { $built += $r } }
} else {
    $r = Build-One $Target; if ($r) { $built += $r }
}

Write-Host "`nBuilt:" -ForegroundColor Green
$built | ForEach-Object { Write-Host "  $_" }

if ($Run -and $Target -ne "all") {
    $exe = Join-Path $build "$Target.exe"
    Write-Host "`n--- running $Target ---`n" -ForegroundColor Yellow
    & $exe
}
