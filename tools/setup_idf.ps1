#requires -Version 5.1
<#
  setup_idf.ps1 - install ESP-IDF v5.5.6 with esp32s3 toolchain on drive D.

  WHY
    - Local proxy 127.0.0.1:7897 resets connections intermittently; large git
      clones get truncated (early EOF).
    - Only the esp32s3-related submodule subset is needed.
    - The toolchain MUST live on drive D (drive C is nearly full).

  STRATEGY
    - Main tree: shallow clone (--depth 1 --branch v5.5.6).
    - Submodules: ALL of them, fetched offline by
      tools\fetch_idf_submodules.py (codeload tarballs, no git clone).
      Do NOT skip any: IDF's early-expansion pass includes the CMakeLists.txt
      of every registered component, so even BT/nimble/openthread/protobuf-c/
      spiffs/unity/cmock/mqtt must be present on disk or the configure step
      aborts with "Missing esp-mqtt submodule".
    - Both git and toolchain downloads retry, alternating proxy / direct.

  USAGE
    powershell -ExecutionPolicy Bypass -NoProfile -File tools\setup_idf.ps1

  NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads .ps1 without a
        BOM using the ANSI code page, so non-ASCII text corrupts parsing.
#>

# NOTE: must stay 'Continue'. Native tools (git, install.bat) write progress to
#       stderr; under 'Stop' PowerShell 5.1 turns that into a terminating
#       NativeCommandError and kills the script. Hard failures are raised with
#       explicit throw instead.
$ErrorActionPreference = 'Continue'

$IdfRoot     = 'D:\Espressif\frameworks\esp-idf'
$ToolsPath   = 'D:\Espressif'
$IdfVersion  = 'v5.5.6'
$Target      = 'esp32s3'
$MaxAttempts = 5
$Proxy       = 'http://127.0.0.1:7897'

$NeededSubmodules = @(
    'components/heap/tlsf',
    'components/heap/heap_caps_allocator',
    'components/lwip/lwip',
    'components/mbedtls/mbedtls',
    'components/esp_phy/lib',
    'components/esp_wifi/lib',
    'components/esp_coex/lib',
    'components/json/cJSON',
    'components/esp_hw_support/lib',
    'components/micro-ecc',
    'components/bootloader_support/esp_secure_boot',
    'components/freertos/FreeRTOS-Kernel'
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

# git transport tuning: bigger buffer, no low-speed abort
git config --global http.postBuffer 524288000
git config --global http.lowSpeedLimit 0
git config --global http.lowSpeedTime 999999

# ------------------------------------------------------------- main tree
if (Test-Path (Join-Path $IdfRoot '.git')) {
    Write-Step "main tree already present: $IdfRoot"
} else {
    $ok = $false
    for ($i = 1; $i -le $MaxAttempts; $i++) {
        $useProxy = ($i % 2 -eq 1)
        Write-Step "cloning main tree (attempt $i/$MaxAttempts, proxy=$useProxy)"
        Set-ProxyMode $useProxy
        New-Item -ItemType Directory -Force -Path (Split-Path $IdfRoot) | Out-Null
        if (Test-Path $IdfRoot) { Remove-Item -Recurse -Force $IdfRoot -ErrorAction SilentlyContinue }
        & git clone --depth 1 --branch $IdfVersion --single-branch https://github.com/espressif/esp-idf.git $IdfRoot 2>&1 | Out-String | Write-Host
        if ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $IdfRoot '.git'))) { $ok = $true; break }
        Write-Warn2 "clone failed (exit=$LASTEXITCODE), retrying"
        Start-Sleep -Seconds 5
    }
    if (-not $ok) { throw "main tree clone failed after $MaxAttempts attempts" }
    Write-Ok "main tree cloned"
}

Set-Location $IdfRoot
Write-Step ("main tree HEAD: " + (& git rev-parse --short HEAD))

# ------------------------------------------------------------- submodules
foreach ($sm in $NeededSubmodules) {
    $parent = Split-Path $sm -Parent
    if (-not (Test-Path (Join-Path $IdfRoot $parent))) {
        Write-Warn2 "skip (path absent): $sm"
        continue
    }
    $done = $false
    for ($i = 1; $i -le 4; $i++) {
        $useProxy = ($i % 2 -eq 1)
        Write-Step "init submodule $sm (attempt $i/4, proxy=$useProxy)"
        Set-ProxyMode $useProxy
        & git submodule update --init --depth 1 --recursive -- $sm 2>&1 | Out-String | Write-Host
        if ($LASTEXITCODE -eq 0) { $done = $true; break }
        Write-Warn2 "submodule $sm failed, retrying"
        Start-Sleep -Seconds 3
    }
    if ($done) { Write-Ok "submodule ready: $sm" } else { Write-Warn2 "submodule NOT ready: $sm" }
}

# ------------------------------------------------------------- toolchain
Set-ProxyMode $true
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PATH       = $IdfRoot

Write-Step "installing $Target toolchain into $ToolsPath (large download, retries)"
for ($i = 1; $i -le 4; $i++) {
    Write-Step "install.bat $Target (attempt $i/4)"
    Push-Location $IdfRoot
    & cmd /c "install.bat $Target" 2>&1 | Out-String | Write-Host
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -eq 0) { break }
    Write-Warn2 "install.bat returned $rc, retrying"
    Start-Sleep -Seconds 10
}

Write-Ok "setup_idf finished. Activate with:"
Write-Host "    . D:\Espressif\frameworks\esp-idf\export.ps1" -ForegroundColor Yellow
