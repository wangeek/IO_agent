# ---------------------------------------------------------------------------
# Sophon -- cold boot / power cycle re-entry check
#
# Why this exists
#   The dongle form factor has a SINGLE USB-A port: pulling the cable cuts
#   power to the whole device. Re-plugging it is therefore a COLD BOOT, not
#   just a USB re-enumeration. The question this script answers is:
#
#     "After a real power loss and re-plug, does the device come back and
#      re-enter the host cleanly?"
#
#   It is NOT the same test as verify_unplug.ps1 (that one keeps the board
#   powered through the COM port and only cycles the USB data link).
#   Here the board loses power, so the firmware restarts from scratch and the
#   RAM disk is rebuilt from the factory image.
#
# What it proves (host side -- works for the dongle too, no serial needed)
#   1. The device disappears from the host when power is removed.
#   2. It comes back and Windows re-binds keyboard + mouse + disk.
#   3. The virtual volume reappears (located BY VOLUME LABEL, because the
#      drive letter may change across a power cycle).
#   4. The RAM disk was reset to the factory image: a marker file written
#      before the cycle is GONE afterwards, and the pre-placed tool file is
#      still there with the same content.
#   5. Mount latency after the re-plug, so we know how long the agent has to
#      wait before the volume is usable.
#
# Optional firmware side (dev board only; pass -SkipSerial to skip)
#   6. The device's own view: a "status:" line with enumerated=1 and
#      sectors=8192 proves the firmware restarted, rebuilt the RAM disk and
#      re-enumerated USB -- independent of the host's PnP view.
#
# NOTE on the dev board
#   Both Type-C ports feed the board's power rail, so a cold boot requires
#   BOTH cables to be removed. The UART port is the only serial link, and
#   the CH343 disappears with its cable -- so the boot banner cannot be
#   captured (the port does not exist while the board is off). The firmware
#   leg therefore reads the periodic status lines after the operator plugs
#   the UART cable back in, instead of the boot banner.
#
# ASCII only: PowerShell 5.1 reads BOM-less .ps1 as ANSI.
# ---------------------------------------------------------------------------
param(
    [string]$ComPort = 'COM3',
    [string]$DeviceId = 'VID_303A&PID_8001',
    [string]$Label = 'SOPHON',
    [string]$LogDir = '.stage',
    [string]$MarkerName = 'CYCLE.MRK',
    [string]$ToolFile = 'WATCH.PS1',
    [int]$MaxOffSeconds = 600,
    [int]$MaxPresentSeconds = 180,
    [int]$MaxVolumeSeconds = 90,
    [switch]$SkipSerial
)
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir | Out-Null }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logDirAbs = (Resolve-Path -Path $LogDir).Path
$serialLog = Join-Path $logDirAbs ("powercycle-serial-" + $stamp + ".log")
$pnplog = Join-Path $logDirAbs ("powercycle-pnp-" + $stamp + ".log")
Set-Content -Path $serialLog -Value '' -Encoding ASCII

$script:results = New-Object System.Collections.ArrayList
function Add-Result {
    param([string]$Name, [bool]$Ok, [string]$Detail)
    $tag = if ($Ok) { 'OK  ' } else { 'FAIL' }
    [void]$script:results.Add((New-Object psobject -Property @{
            Name = $Name; Ok = $Ok; Detail = $Detail; Tag = $tag
        }))
    Write-Host ("  " + $tag + ": " + $Name + " -- " + $Detail)
}

function Get-DevicePresent {
    try {
        $found = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                 Where-Object { $_.InstanceId -like ('*' + $DeviceId + '*') }
        if ($found) { return $true }
    } catch { }
    return $false
}

function Get-VolumeRootByLabel {
    param([string]$WantLabel)
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        try {
            if ($d.IsReady -and $d.VolumeLabel -eq $WantLabel -and $d.DriveType -eq 'Removable') {
                return $d.RootDirectory.FullName
            }
        } catch { }
    }
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        try {
            if ($d.IsReady -and $d.VolumeLabel -eq $WantLabel) {
                return $d.RootDirectory.FullName
            }
        } catch { }
    }
    return $null
}

function Get-FileHashHex {
    param([string]$Path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        try {
            $h = $sha.ComputeHash($fs)
        } finally {
            $fs.Close()
            $sha.Dispose()
        }
        $sb = New-Object System.Text.StringBuilder
        foreach ($b in $h) { [void]$sb.Append($b.ToString('x2')) }
        return $sb.ToString()
    } catch {
        return ''
    }
}

Write-Host "=== Sophon cold boot / power cycle check ==="
Write-Host ("device id        : " + $DeviceId)
Write-Host ("serial (optional): " + $ComPort + "  -> " + $serialLog)
Write-Host ("pnp              : " + $pnplog)
Write-Host ""

# ===========================================================================
# PHASE 1 -- pre-cycle state
# ===========================================================================
Write-Host "--- phase 1: pre-cycle state ---"
$preRoot = Get-VolumeRootByLabel $Label
if ($null -eq $preRoot) {
    Write-Host ("  FAIL: volume '" + $Label + "' not found before the cycle.")
    Write-Host "        Plug the device in and make sure the volume is mounted, then re-run."
    Write-Host ""
    Write-Host "RESULT: power cycle check FAILED"
    exit 1
}
$preRoot = $preRoot.TrimEnd('\') + '\'
Write-Host ("  volume root : " + $preRoot)

$preToolPath = Join-Path $preRoot $ToolFile
$preToolHash = ''
$preToolSize = -1
if (Test-Path $preToolPath) {
    $preToolHash = Get-FileHashHex $preToolPath
    $preToolSize = (Get-Item $preToolPath).Length
    Write-Host ("  tool file   : " + $ToolFile + " size=" + $preToolSize + " sha256=" + $preToolHash)
} else {
    Write-Host ("  WARN: " + $ToolFile + " not found on the volume (pre-check)")
}

# Marker file: proves the RAM disk really was reset by the power cycle.
$markerPath = Join-Path $preRoot $MarkerName
try {
    $stampBytes = [System.Text.Encoding]::ASCII.GetBytes(
        ('powercycle-marker ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + "`r`n"))
    [System.IO.File]::WriteAllBytes($markerPath, $stampBytes)
    Write-Host ("  marker      : wrote " + $MarkerName + " (" + $stampBytes.Length + " bytes)")
} catch {
    Write-Host ("  WARN: could not write marker file: " + $_.Exception.Message)
}

$preListing = @()
try {
    $preListing = @(Get-ChildItem -Path $preRoot -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
} catch { }
Write-Host ("  files before: " + ($preListing -join ', '))

# ===========================================================================
# PHASE 2 -- power off
# ===========================================================================
Write-Host ""
Write-Host "--- phase 2: power off ---"
Write-Host "  Remove BOTH cables from the board (UART and USB)."
Write-Host "  On the dongle this is just pulling the single USB-A plug."
Write-Host "  The script will notice the device going ABSENT."
Write-Host ""

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$seenAbsent = $false
$lastState = $null
$absentAt = -1.0
while ($sw.Elapsed.TotalSeconds -lt $MaxOffSeconds) {
    $present = Get-DevicePresent
    if ($present -ne $lastState) {
        $state = if ($present) { 'PRESENT' } else { 'ABSENT' }
        $line = ("[" + $sw.Elapsed.TotalSeconds.ToString('F2') + "s] device " + $state)
        Add-Content -Path $pnplog -Value $line
        Write-Host ("  " + $line)
        if (-not $present) {
            $seenAbsent = $true
            $absentAt = $sw.Elapsed.TotalSeconds
        }
        $lastState = $present
    }
    if ($seenAbsent) { break }
    Start-Sleep -Milliseconds 300
}

if (-not $seenAbsent) {
    Write-Host ""
    Write-Host "  FAIL: device never went ABSENT. Power was probably never removed."
    Write-Host ""
    Write-Host "RESULT: power cycle check FAILED"
    exit 1
}
Add-Result 'host sees the device disappear on power loss' $true ("ABSENT at t=" + $absentAt.ToString('F2') + "s")

# ===========================================================================
# PHASE 3 -- power on
# ===========================================================================
Write-Host ""
Write-Host "--- phase 3: power on ---"
Write-Host "  Plug the device back in (dongle: one USB-A plug)."
Write-Host "  On the dev board: plug the UART cable first (power), then the USB cable (data)."
Write-Host "  The script will wait for the device to enumerate again."
Write-Host ""

$sw2 = [System.Diagnostics.Stopwatch]::StartNew()
$seenPresent = $false
$presentAt = -1.0
while ($sw2.Elapsed.TotalSeconds -lt $MaxPresentSeconds) {
    $present = Get-DevicePresent
    if ($present -ne $lastState) {
        $state = if ($present) { 'PRESENT' } else { 'ABSENT' }
        $line = ("[" + $sw2.Elapsed.TotalSeconds.ToString('F2') + "s] device " + $state)
        Add-Content -Path $pnplog -Value $line
        Write-Host ("  " + $line)
        if ($present) {
            $seenPresent = $true
            $presentAt = $sw2.Elapsed.TotalSeconds
        }
        $lastState = $present
    }
    if ($seenPresent) { break }
    Start-Sleep -Milliseconds 300
}

if (-not $seenPresent) {
    Write-Host ""
    Write-Host "  FAIL: device did not enumerate within " + $MaxPresentSeconds + " s."
    Write-Host "        (Check USB supply: ESP32-S3 + WiFi bursts need >= 500 mA.)"
    Write-Host ""
    Write-Host "RESULT: power cycle check FAILED"
    exit 1
}
Add-Result 'device enumerates again after power on' $true ("PRESENT at t=" + $presentAt.ToString('F2') + "s")

# ===========================================================================
# PHASE 4 -- volume comes back
# ===========================================================================
Write-Host ""
Write-Host "--- phase 4: waiting for the virtual volume ---"
$sw3 = [System.Diagnostics.Stopwatch]::StartNew()
$postRoot = $null
while ($sw3.Elapsed.TotalSeconds -lt $MaxVolumeSeconds) {
    $postRoot = Get-VolumeRootByLabel $Label
    if ($null -ne $postRoot) { break }
    Start-Sleep -Milliseconds 500
}

if ($null -eq $postRoot) {
    Add-Result 'virtual volume reappears (by volume label)' $false ("not found within " + $MaxVolumeSeconds + " s")
} else {
    $postRoot = $postRoot.TrimEnd('\') + '\'
    $mountLatency = $sw3.Elapsed.TotalSeconds
    Add-Result 'virtual volume reappears (by volume label)' $true ("root=" + $postRoot + " after " + $mountLatency.ToString('F2') + " s")
    Write-Host ("  NOTE: the drive letter may change across a power cycle.")
    Write-Host ("        before = " + $preRoot + "   after = " + $postRoot)
}

# ===========================================================================
# PHASE 5 -- RAM disk was reset to the factory image
# ===========================================================================
Write-Host ""
Write-Host "--- phase 5: RAM disk state after the cycle ---"
if ($null -ne $postRoot) {
    $markerGone = -not (Test-Path (Join-Path $postRoot $MarkerName))
    $markerDetail = 'marker still present -- the volume was NOT reset'
    if ($markerGone) { $markerDetail = 'marker is gone' }
    Add-Result 'RAM disk was reset (marker file gone)' $markerGone $markerDetail

    $postToolPath = Join-Path $postRoot $ToolFile
    if (-not (Test-Path $postToolPath)) {
        Add-Result ('factory tool file present (' + $ToolFile + ')') $false 'file missing after the cycle'
    } else {
        $postToolHash = Get-FileHashHex $postToolPath
        $postToolSize = (Get-Item $postToolPath).Length
        if ($preToolHash -and $postToolHash -eq $preToolHash) {
            Add-Result ('factory tool file present (' + $ToolFile + ')') $true `
                ('size=' + $postToolSize + ', sha256 matches the pre-cycle value')
        } elseif ($preToolSize -ge 0 -and $postToolSize -eq $preToolSize) {
            Add-Result ('factory tool file present (' + $ToolFile + ')') $true `
                ('size=' + $postToolSize + ', sha256 differs (the file was edited before the cycle)')
        } else {
            Add-Result ('factory tool file present (' + $ToolFile + ')') $false `
                ('pre size=' + $preToolSize + ', post size=' + $postToolSize)
        }
    }

    $postListing = @()
    try {
        $postListing = @(Get-ChildItem -Path $postRoot -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    } catch { }
    Write-Host ("  files after : " + ($postListing -join ', '))
}

# ===========================================================================
# PHASE 6 -- optional firmware leg
# ===========================================================================
if (-not $SkipSerial) {
    Write-Host ""
    Write-Host "--- phase 6: firmware leg (optional) ---"
    Write-Host "  The UART port only exists while its cable is plugged."
    Write-Host "  Plug the UART cable back in now; the script reads the status lines."
    Write-Host ""

    $deadline = (Get-Date).AddSeconds(90)
    $opened = $false
    while ((Get-Date) -lt $deadline -and -not $opened) {
        try {
            $sp = New-Object System.IO.Ports.SerialPort $ComPort, 115200, 'None', 8, 'One'
            $sp.ReadTimeout = 500
            # DTR/RTS de-asserted BEFORE Open: the devkit wires RTS -> EN, so
            # an asserted line would reset the board and destroy the evidence.
            $sp.DtrEnable = $false
            $sp.RtsEnable = $false
            $sp.Open()
            $sp.DtrEnable = $false
            $sp.RtsEnable = $false
            $opened = $true
        } catch {
            Start-Sleep -Milliseconds 1000
        }
    }

    if (-not $opened) {
        Write-Host ("  WARN: could not open " + $ComPort + " -- firmware leg skipped.")
        Write-Host "        Host-side evidence above is still valid."
    } else {
        $sw4 = [System.Diagnostics.Stopwatch]::StartNew()
        while ($sw4.Elapsed.TotalSeconds -lt 20) {
            try {
                $line = $sp.ReadLine()
                if ($line -ne $null) {
                    Add-Content -Path $serialLog -Value ("[" + $sw4.Elapsed.TotalSeconds.ToString('F2') + "s] " + $line)
                }
            } catch { }
        }
        $sp.Close()
        $sp.Dispose()

        $serialLines = @()
        if (Test-Path $serialLog) { $serialLines = @(Get-Content $serialLog -ErrorAction SilentlyContinue) }
        Write-Host ("  serial lines: " + $serialLines.Count)
        $serialLines |
            Where-Object { $_ -match 'rst:0x|status: link=|RAM disk|enumerated|panic|abort|Guru' } |
            Select-Object -First 60 | ForEach-Object { Write-Host ("    " + $_) }

        $statusSeen = @($serialLines | Where-Object { $_ -match 'status: link=' })
        $panicSeen = @($serialLines | Where-Object { $_ -match 'panic|abort|Guru Meditation|assert failed' })

        if ($statusSeen.Count -gt 0) {
            Add-Result 'firmware restarted and is running' $true ($statusSeen.Count.ToString() + ' status line(s) seen')
        } else {
            Add-Result 'firmware restarted and is running' $false 'no status line in the serial log'
        }

        if (($serialLines | Where-Object { $_ -match 'enumerated=1' }).Count -gt 0) {
            Add-Result 'device side reports USB enumerated' $true 'status line shows enumerated=1'
        } else {
            Add-Result 'device side reports USB enumerated' $false 'no status line with enumerated=1'
        }

        if (($serialLines | Where-Object { $_ -match 'sectors=8192' }).Count -gt 0) {
            Add-Result 'RAM disk rebuilt by the restarted firmware' $true 'status line shows sectors=8192'
        } else {
            Add-Result 'RAM disk rebuilt by the restarted firmware' $false 'no status line with sectors=8192'
        }

        if ($panicSeen.Count -gt 0) {
            Add-Result 'no panic / abort in the log' $false ($panicSeen.Count.ToString() + ' panic/abort line(s)')
        } else {
            Add-Result 'no panic / abort in the log' $true 'clean'
        }
    }
}

# ===========================================================================
# verdict
# ===========================================================================
Write-Host ""
Write-Host "=== verdict ==="
$failed = 0
foreach ($r in $script:results) {
    Write-Host ("  " + $r.Tag + ": " + $r.Name + " -- " + $r.Detail)
    if (-not $r.Ok) { $failed++ }
}

Write-Host ""
if ($failed -eq 0) {
    Write-Host "RESULT: power cycle check PASSED"
    exit 0
}
Write-Host ("RESULT: power cycle check FAILED (" + $failed + " check(s) failed)")
exit 1
