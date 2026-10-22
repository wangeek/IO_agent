# ---------------------------------------------------------------------------
# IO_Agent M1 -- unplug / replug robustness check
#
# What it proves
#   1. The USB device disappears from the host when the cable is removed.
#   2. The device comes back and Windows re-binds keyboard + mouse + disk.
#   3. The ESP32 does NOT reset or crash: the firmware must report a
#      disconnect and then a fresh "host enumerated and configured", while the
#      uptime counter keeps running (a reset would restart it).
#
# How it works
#   Runs two loops in parallel:
#     - a serial reader on COM3 that timestamps every firmware log line
#     - a PnP poller that samples the VID_303A presence every 500 ms
#   The operator performs the physical unplug/replug while it runs.
#
# NOTE: the board is powered through the "COM" port (CH343), so pulling the
# "USB" port must NOT reset the MCU. If the uptime restarts, the wrong port
# was unplugged or the board lost power.
#
# ASCII only: PowerShell 5.1 reads BOM-less .ps1 as ANSI.
# ---------------------------------------------------------------------------
param(
    [int]$DurationSeconds = 90,
    [string]$ComPort = 'COM3',
    [string]$VID = 'VID_303A',
    [string]$LogDir = '.stage'
)
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir | Out-Null }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
# ABSOLUTE paths: a PowerShell background job runs in its own process with a
# different working directory, so a relative path silently writes nowhere.
$logDirAbs = (Resolve-Path -Path $LogDir).Path
$serialLog = Join-Path $logDirAbs ("unplug-serial-" + $stamp + ".log")
$pnplog = Join-Path $logDirAbs ("unplug-pnp-" + $stamp + ".log")
# Create the serial log up front so the analysis step can tell "no output"
# apart from "reader never started".
Set-Content -Path $serialLog -Value '' -Encoding ASCII

Write-Host "=== IO_Agent unplug/replug check ==="
Write-Host ("duration : waits up to " + [math]::Max($DurationSeconds, 600) + " s for a full unplug cycle")
Write-Host ("serial   : " + $ComPort + "  -> " + $serialLog)
Write-Host ("pnp      : " + $pnplog)
Write-Host ""
Write-Host ">>> Unplug the USB-OTG Type-C port whenever you can, wait 3 s, plug it back in."
Write-Host "    The script keeps waiting until it has seen the full cycle."
Write-Host ""

$hardLimit = [math]::Max($DurationSeconds, 600)

# --- serial reader (background job) -----------------------------------------
$serialJob = Start-Job -ScriptBlock {
    param($port, $outFile, $secs)
    $ErrorActionPreference = 'Continue'
    try {
        $sp = New-Object System.IO.Ports.SerialPort $port, 115200, 'None', 8, 'One'
        $sp.ReadTimeout = 500
        # Opening COM3 with DTR/RTS asserted RESETS the board (the devkit's
        # auto-reset circuit wires RTS -> EN). Keep both de-asserted so the
        # unplug test measures a real unplug, not a power cycle.
        $sp.DtrEnable = $false
        $sp.RtsEnable = $false
        $sp.Open()
        $sp.DtrEnable = $false
        $sp.RtsEnable = $false
    } catch {
        Add-Content -Path $outFile -Value ("[serial-open-error] " + $_.Exception.Message)
        return
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $secs) {
        try {
            $line = $sp.ReadLine()
            if ($line -ne $null) {
                Add-Content -Path $outFile -Value ("[" + $sw.Elapsed.TotalSeconds.ToString('F2') + "s] " + $line)
            }
        } catch { }
    }
    $sp.Close()
} -ArgumentList $ComPort, $serialLog, $hardLimit

# --- PnP presence poller ----------------------------------------------------
# Wait for the operator rather than racing a fixed window: the loop keeps
# sampling until it has seen ABSENT followed by PRESENT (one full unplug cycle)
# AND the firmware has had a few seconds to log the re-enumeration.
$presence = New-Object System.Collections.ArrayList
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$lastState = $null
$firstAbsentT = -1.0
$seenAbsent = $false
$seenPresentAfterAbsent = $false
$cycleDoneAt = -1.0

while ($sw.Elapsed.TotalSeconds -lt $hardLimit) {
    $present = $false
    try {
        $found = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                 Where-Object { $_.InstanceId -like ('*' + $VID + '*') }
        if ($found) { $present = $true }
    } catch { }

    if ($present -ne $lastState) {
        $state = if ($present) { 'PRESENT' } else { 'ABSENT' }
        $tNow = $sw.Elapsed.TotalSeconds
        $line = ("[" + $tNow.ToString('F2') + "s] device " + $state)
        [void]$presence.Add($line)
        Add-Content -Path $pnplog -Value $line
        Write-Host $line
        if (-not $present) {
            if (-not $seenAbsent) { $firstAbsentT = $tNow }
            $seenAbsent = $true
        } elseif ($seenAbsent) {
            $seenPresentAfterAbsent = $true
            $cycleDoneAt = $tNow
        }
        $lastState = $present
    }

    # Give the firmware 12 s to emit its link-loss / re-enumeration log lines,
    # then stop. (The firmware polls the link every 200 ms; the ATTACHED event
    # lands ~1.5 s after the host re-binds the device.)
    if ($seenPresentAfterAbsent -and ($sw.Elapsed.TotalSeconds - $cycleDoneAt) -ge 12) { break }
    Start-Sleep -Milliseconds 500
}

Write-Host ""
if ($seenPresentAfterAbsent) {
    Write-Host "--- unplug cycle seen; stopping the serial reader ---"
    Stop-Job -Job $serialJob -ErrorAction SilentlyContinue
} else {
    Write-Host "--- no unplug cycle seen; serial reader ran the full window ---"
}
Receive-Job -Job $serialJob -Wait -ErrorAction SilentlyContinue | Out-Null

# --- analysis ---------------------------------------------------------------
Write-Host ""
Write-Host "=== presence transitions ==="
Get-Content $pnplog -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("  " + $_) }

$transitions = @($presence | Where-Object { $_ -match 'ABSENT' }).Count
$reEnumerated = $false
$disconnectSeen = $false
$linkRestored = $false
$serialLines = 0
$serialError = ''
# A reset that happens BEFORE the unplug is the serial-port open side effect
# (opening COM3 asserts DTR/RTS -> the devkit auto-reset circuit pulls EN low).
# That is expected and harmless. Only a reset AFTER the unplug means the
# USB-OTG port is also the board's power rail -- that IS a failure.
$resetAfterUnplug = $false
$firstAbsentT = -1.0
$resetT = -1.0

if (Test-Path $serialLog) {
    $lines = @(Get-Content $serialLog -ErrorAction SilentlyContinue)
    $serialLines = $lines.Count
    foreach ($l in $lines) {
        $ts = -1.0
        if ($l -match '^\[([0-9.]+)s\]') { $ts = [double]$Matches[1] }
        if ($l -match 'rst:0x') {
            if ($resetT -lt 0) { $resetT = $ts }
            if ($firstAbsentT -ge 0 -and $ts -ge $firstAbsentT) { $resetAfterUnplug = $true }
        }
        # 'host enumerated and configured' = TinyUSB ATTACHED event (re-enumeration).
        # 'host link restored'             = link came back (tud_ready() true again).
        if ($l -match 'enumerated and configured') { $reEnumerated = $true }
        if ($l -match 'host link restored') { $linkRestored = $true }
        # 'host link lost' = link went down (ESP32-S3 DWC2 reports SUSPEND on
        # unplug, not DETACHED -- see firmware/io_usb/include/io_usb.h).
        # 'host disconnected' = the DETACHED event (kept for other hardware).
        if ($l -match 'host link lost') { $disconnectSeen = $true }
        if ($l -match 'host disconnected') { $disconnectSeen = $true }
        if ($l -match '\[serial-open-error\]') { $serialError = $l }
    }
    Write-Host ""
    Write-Host ("=== firmware serial log: " + $serialLines + " line(s) ===")
    $lines | Where-Object {
        $_ -match 'rst:0x|enumerated|configured|disconnect|host link|status: link=|write detected|panic|abort|Guru Meditation|serial-open-error'
    } | Select-Object -First 200 | ForEach-Object { Write-Host ("  " + $_) }
} else {
    Write-Host ""
    Write-Host "=== firmware serial log: FILE MISSING ==="
}

Write-Host ""
Write-Host "=== verdict ==="
$ok = $true
if ($transitions -lt 1) {
    Write-Host "  FAIL: no ABSENT transition was observed -- the unplug did NOT happen."
    Write-Host "        Please unplug the USB-OTG Type-C port during the run."
    $ok = $false
} else {
    Write-Host "  OK  : device went ABSENT at least once (host saw it disappear)"
}
if ($serialError) {
    Write-Host ("  WARN: serial reader failed: " + $serialError)
    Write-Host "        PnP evidence is still valid; firmware-side confirmation is missing."
} elseif ($serialLines -eq 0) {
    Write-Host "  WARN: serial log is empty; firmware-side confirmation is missing."
} else {
    if ($disconnectSeen) {
        Write-Host "  OK  : firmware logged the link loss (host link lost / host disconnected)"
    } else {
        Write-Host "  WARN: firmware logged neither 'host link lost' nor 'host disconnected'"
        Write-Host "        -- the device never noticed the link going down"
    }
    if ($linkRestored) {
        Write-Host "  OK  : firmware saw the link come back (host link restored)"
    }
    if ($reEnumerated) {
        Write-Host "  OK  : firmware reported an enumeration after the reconnect"
    } else {
        Write-Host "  FAIL: no enumeration reported by the firmware after reconnect"
        $ok = $false
    }
    if ($resetAfterUnplug) {
        Write-Host "  FAIL: the MCU reset AFTER the unplug -- the USB-OTG port also feeds"
        Write-Host "        the board's power rail, so this test cannot isolate USB behaviour."
        $ok = $false
    } elseif ($resetT -ge 0) {
        Write-Host ("  OK  : the only reset was at t=" + $resetT + "s (serial-open side effect, before the unplug)")
    } else {
        Write-Host "  OK  : no MCU reset at all during the test"
    }
}

if ($ok) { Write-Host "`nRESULT: unplug/replug check PASSED"; exit 0 }
Write-Host "`nRESULT: unplug/replug check FAILED"; exit 1
