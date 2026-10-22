#requires -Version 5.1
<#
  idf.ps1 - run ESP-IDF's idf.py inside the project's pinned toolchain.

  WHY
    - The toolchain lives on drive D (drive C is nearly full) and is not on
      the default PATH, so every idf.py invocation needs the same environment.
    - IDF_PYTHON_ENV_PATH is pinned so idf.py always uses the prepared
      virtualenv instead of searching for a system Python.
    - IDF_SKIP_CHECK_SUBMODULES=1 disables IDF's automatic
      "git submodule update --init --recursive". That auto-fetch walks GitHub
      through a flaky local proxy and hangs; submodules are provisioned
      offline by tools\fetch_idf_submodules.py instead (codeload tarballs,
      each file verified against GitHub's git-tree API).

  USAGE
    powershell -ExecutionPolicy Bypass -NoProfile -File tools\idf.ps1 build
    powershell -ExecutionPolicy Bypass -NoProfile -File tools\idf.ps1 -p COM3 flash
    powershell -ExecutionPolicy Bypass -NoProfile -File tools\idf.ps1 monitor

  NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads .ps1 without a
        BOM using the ANSI code page, so non-ASCII text corrupts parsing.
#>

$ErrorActionPreference = 'Continue'

$IdfRoot     = 'D:\Espressif\frameworks\esp-idf'
$ToolsPath   = 'D:\Espressif'
$PythonEnv   = 'D:\Espressif\python_env\idf5.5_py3.11_env'

$env:IDF_PATH                   = $IdfRoot
$env:IDF_TOOLS_PATH             = $ToolsPath
$env:IDF_PYTHON_ENV_PATH        = $PythonEnv
$env:IDF_SKIP_CHECK_SUBMODULES  = '1'

# esp_rom's gdbinit generator needs the ROM ELF archives. IDF's export.ps1
# normally exports this path; we run idf.py directly, so set it ourselves.
# Downloaded and sha256-verified against tools.json (esp-rom-elfs 20260528).
$RomElfDir = Join-Path $ToolsPath 'tools\esp-rom-elfs\20260528'
if (Test-Path $RomElfDir) { $env:ESP_ROM_ELF_DIR = $RomElfDir }

# Windows PowerShell 5.1 defaults to the ANSI code page (GBK on this machine).
# CMake prints non-ASCII project strings; idf.py then re-encodes that console
# output, and the GBK codec raises UnicodeEncodeError on the replacement
# character, killing the build after a successful configure step. Force UTF-8
# on both the Python side and the console side.
$env:PYTHONIOENCODING           = 'utf-8'
$env:PYTHONUTF8                = '1'
try {
    [Console]::OutputEncoding    = [System.Text.Encoding]::UTF8
    $OutputEncoding             = [System.Text.Encoding]::UTF8
} catch {
    # A non-console host (redirected output) may refuse this; harmless.
}

$ToolBins = @(
    'D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin',
    'D:\Espressif\tools\cmake\3.30.2\bin',
    'D:\Espressif\tools\ninja\1.12.1'
)
foreach ($b in $ToolBins) {
    if (Test-Path $b) { $env:PATH = $b + ';' + $env:PATH }
}

$Python = Join-Path $PythonEnv 'Scripts\python.exe'
if (-not (Test-Path $Python)) { throw "IDF python env not found: $Python" }
$IdfPy  = Join-Path $IdfRoot 'tools\idf.py'
if (-not (Test-Path $IdfPy))  { throw "idf.py not found: $IdfPy" }

$ProjectDir = Join-Path $PSScriptRoot '..\firmware'
$ProjectDir = (Resolve-Path $ProjectDir).Path

Push-Location $ProjectDir
try {
    & $Python $IdfPy @args
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($rc -ne 0) { exit $rc }
exit 0
