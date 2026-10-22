#requires -Version 5.1
<#
  fetch_idf_tools.ps1 - download the ESP-IDF build toolchain with resume.

  WHY
    - idf_tools.py downloads with plain urllib and no resume. The local proxy
      truncates long transfers at a few hundred KB, so every attempt dies with
      "retrieval incomplete" and the toolchain never lands.
    - curl.exe (built into Windows) supports byte-range resume, so a truncated
      transfer continues instead of restarting.
    - idf_tools.py skips the download entirely when dist/<archive> already has
      the expected size and sha256 (see idf_tools.py: check_download_file), so
      the manual download is handed over for extraction.

  USAGE
    powershell -ExecutionPolicy Bypass -NoProfile -File tools\fetch_idf_tools.ps1

  ASCII only: Windows PowerShell 5.1 reads .ps1 without a BOM as ANSI.
#>

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'

$Dist      = 'D:\Espressif\dist'
$Proxy     = 'http://127.0.0.1:7897'

# name, url, size, sha256
$Artifacts = @(
    @{
        Name = 'ninja-win-v1.12.1.zip'
        Url  = 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip'
        Size = 275425
        Sha  = 'f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a'
    },
    @{
        Name = 'cmake-3.30.2-windows-x86_64.zip'
        Url  = 'https://github.com/Kitware/CMake/releases/download/v3.30.2/cmake-3.30.2-windows-x86_64.zip'
        Size = 45404613
        Sha  = '48bf4b3dc2d668c578e0884cac7878e146b036ca6b5ce4f8b5572f861b004c25'
    },
    @{
        Name = 'xtensa-esp-elf-14.2.0_20260121-x86_64-w64-mingw32.zip'
        Url  = 'https://github.com/espressif/crosstool-NG/releases/download/esp-14.2.0_20260121/xtensa-esp-elf-14.2.0_20260121-x86_64-w64-mingw32.zip'
        Size = 413859845
        Sha  = '82cbe0353c2e7c96acc8755c854aaa2d93d346c6a378fc692e60b3912c560108'
    }
)

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok   ($msg) { Write-Host "    OK  $msg" -ForegroundColor Green }
function Write-Warn2($msg) { Write-Host "    !!  $msg" -ForegroundColor Yellow }

function Set-ProxyMode([bool]$useProxy) {
    if ($useProxy) {
        $env:HTTP_PROXY  = $Proxy
        $env:HTTPS_PROXY = $Proxy
        $env:http_proxy  = $Proxy
        $env:https_proxy = $Proxy
    } else {
        Remove-Item Env:HTTP_PROXY, Env:HTTPS_PROXY, Env:http_proxy, Env:https_proxy -ErrorAction SilentlyContinue
    }
}

function Test-Artifact {
    param([string] $Path, [long] $Size, [string] $Sha)
    if (-not (Test-Path $Path)) { return $false }
    $len = (Get-Item $Path).Length
    if ($len -ne $Size) { return $false }
    $h = (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLower()
    return ($h -eq $Sha.ToLower())
}

function Fetch-Artifact {
    param($A)
    $dest = Join-Path $Dist $A.Name
    if (Test-Artifact $dest $A.Size $A.Sha) {
        Write-Ok "$($A.Name) already complete"
        return $true
    }

    for ($attempt = 1; $attempt -le 12; $attempt++) {
        $useProxy = ($attempt % 2 -eq 1)
        Set-ProxyMode $useProxy
        $have = 0
        if (Test-Path $dest) { $have = (Get-Item $dest).Length }
        Write-Host ("    ... {0}: have {1} / {2} bytes (proxy={3})" -f $A.Name, $have, $A.Size, $useProxy)

        & curl.exe -L -C - --retry 8 --retry-all-errors --retry-delay 3 `
            --connect-timeout 20 --speed-time 60 --speed-limit 1024 `
            -s -o $dest $A.Url
        $rc = $LASTEXITCODE

        if (Test-Artifact $dest $A.Size $A.Sha) {
            Write-Ok "$($A.Name) downloaded and verified"
            return $true
        }
        if ($rc -eq 33) {
            # server refused a range request: start over
            Write-Warn2 "$($A.Name): range refused, restarting from scratch"
            if (Test-Path $dest) { Remove-Item -Force $dest -ErrorAction SilentlyContinue }
        }
        Start-Sleep -Seconds 2
    }
    Write-Warn2 "$($A.Name): FAILED after 12 attempts"
    return $false
}

New-Item -ItemType Directory -Force -Path $Dist | Out-Null

$failed = @()
foreach ($a in $Artifacts) {
    Write-Step "fetching $($a.Name)"
    if (-not (Fetch-Artifact $a)) { $failed += $a.Name }
}

Set-ProxyMode $true
if ($failed.Count -eq 0) {
    Write-Ok "all tool archives present in $Dist"
} else {
    Write-Warn2 ("incomplete: " + ($failed -join ', '))
    exit 1
}
