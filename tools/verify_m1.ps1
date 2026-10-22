#requires -Version 5.1
<#
  verify_m1.ps1 - acceptance check for the M1 composite USB device.

  Checks, in order:
    1. Enumeration : does Windows see the ESP32-S3 as a USB device at all?
                     Which device classes did it create?
    2. Volume      : is there a FAT volume labelled IOAGENT? (found by label,
                     never by a hard-coded drive letter)
    3. Read/write  : write a file, read it back, compare bytes, delete it.
    4. Endpoints   : report the interfaces Windows bound, so they can be
                     compared against the firmware's frozen endpoint map
                     (HID_IN=0x81, MSC_OUT=0x02, MSC_IN=0x82).

  USAGE
    powershell -ExecutionPolicy Bypass -NoProfile -File tools\verify_m1.ps1
    powershell -ExecutionPolicy Bypass -NoProfile -File tools\verify_m1.ps1 -WaitSeconds 20

  NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads .ps1 files
        without a BOM using the ANSI code page, so non-ASCII text corrupts
        parsing.
#>

param(
    [int]$WaitSeconds = 0
)

$ErrorActionPreference = 'Continue'

$VID       = 'VID_303A'
$PID_HEX   = 'PID_8001'
$VolumeLbl = 'IOAGENT'
$TestFile  = 'M1TEST.TXT'   # 8.3 short name: the FAT16 image has no LFN
$Timeout   = 60

function Write-Section([string]$title) {
    Write-Output ""
    Write-Output ("=== " + $title + " ===")
}

function Get-UsbDevice {
    param([switch]$PresentOnly)
    if ($PresentOnly) {
        return Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue
    }
    return Get-PnpDevice -ErrorAction SilentlyContinue
}

Write-Output "IO_Agent M1 acceptance check"
Write-Output ("host: " + $env:COMPUTERNAME)

if ($WaitSeconds -gt 0) {
    Write-Section ("waiting " + $WaitSeconds + "s for enumeration")
    Start-Sleep -Seconds $WaitSeconds
}

# ---------------------------------------------------------------------------
# 1. enumeration
# ---------------------------------------------------------------------------
Write-Section "1. USB enumeration"

$all = Get-UsbDevice
# Only our composite device (PID_8001) is under test. The ESP32-S3 SoC also
# exposes its own native USB peripherals under the same VID with PID_1001
# (USB Serial/JTAG + JTAG debug unit); those are ROM features, not our
# firmware, so they are reported separately instead of as unknown devices.
$ours     = $all | Where-Object { $_.InstanceId -match $PID_HEX }
$otherVid = $all | Where-Object { ($_.InstanceId -match $VID) -and ($_.InstanceId -notmatch $PID_HEX) }

if (-not $ours) {
    Write-Output "FAIL: no device with $VID and $PID_HEX found."
    Write-Output "      The USB-OTG port (the Type-C port marked 'USB') must be"
    Write-Output "      plugged into THIS machine. The port marked 'COM' is the"
    Write-Output "      CH343 UART and only carries the serial log."
    exit 1
}

Write-Output ("found " + @($ours).Count + " device node(s) for $PID_HEX (our composite device)")
foreach ($d in $ours) {
    Write-Output ("  [" + $d.Status + "] " + $d.Class + " | " + $d.FriendlyName + " | " + $d.InstanceId)
}

if ($otherVid) {
    Write-Output ("note: " + @($otherVid).Count + " node(s) with the same VID but a different PID (SoC native USB, not ours):")
    foreach ($d in $otherVid) {
        Write-Output ("  [" + $d.Status + "] " + $d.Class + " | " + $d.FriendlyName)
    }
}

$classes = @($ours | ForEach-Object { $_.Class } | Sort-Object -Unique)
Write-Output ("classes: " + ($classes -join ', '))

$hasHid  = @($ours | Where-Object { $_.Class -eq 'HIDClass' }).Count -gt 0
$hasMass = @($ours | Where-Object { $_.Class -eq 'DiskDrive' -or $_.Class -eq 'USB' -and $_.FriendlyName -match 'Mass Storage' }).Count -gt 0
$hasComposite = @($ours | Where-Object { $_.FriendlyName -match 'Composite' }).Count -gt 0

Write-Output ("  HID interface      : " + $(if ($hasHid)  { 'present' } else { 'MISSING' }))
Write-Output ("  Mass storage iface : " + $(if ($hasMass) { 'present' } else { 'MISSING' }))
Write-Output ("  Composite device   : " + $(if ($hasComposite) { 'present' } else { 'not seen as composite' }))

# Windows splits the single HID interface into TWO collections by parsing the
# composite report descriptor: COL01 = keyboard, COL02 = mouse. That is the
# proof that the composite report descriptor (Report ID 1 = keyboard,
# 2 = mouse) was parsed correctly by the host, and it is why the frozen
# "single HID interface" design needs no extra endpoint.
$hidNames = @($ours | Where-Object { $_.Class -eq 'HIDClass' } | ForEach-Object { $_.FriendlyName })
Write-Output ("  HID node names     : " + ($hidNames -join ' | '))

# ---------------------------------------------------------------------------
# 2. volume
# ---------------------------------------------------------------------------
Write-Section "2. IOAGENT volume"

$vol = Get-CimInstance Win32_LogicalDisk -ErrorAction SilentlyContinue |
       Where-Object { $_.VolumeName -eq $VolumeLbl }

if (-not $vol) {
    Write-Output "FAIL: no logical disk with volume label '$VolumeLbl'."
    Write-Output "      The disk may not be formatted/assigned yet, or the host"
    Write-Output "      may still be writing the FAT structures."
    $vol = Get-CimInstance Win32_LogicalDisk -ErrorAction SilentlyContinue |
           Where-Object { $_.DriveType -eq 2 } |
           ForEach-Object { Write-Output ("  removable candidate: " + $_.DeviceID + " label='" + $_.VolumeName + "'") }
    exit 2
}

$root = $vol.DeviceID
Write-Output ("volume: " + $root + " label='" + $vol.VolumeName + "' size=" + $vol.Size + " free=" + $vol.FreeSpace)

# ---------------------------------------------------------------------------
# 3. read / write round trip
# ---------------------------------------------------------------------------
Write-Section "3. read/write round trip"

$target = $root + '\' + $TestFile
$payload = "IO_AGENT_M1_WRITE_TEST_2026"
$payloadBytes = [System.Text.Encoding]::ASCII.GetBytes($payload)

try {
    [System.IO.File]::WriteAllBytes($target, $payloadBytes)
    Write-Output ("wrote " + $payloadBytes.Length + " bytes to " + $target)
} catch {
    Write-Output ("FAIL: write failed: " + $_.Exception.Message)
    exit 3
}

try {
    $back = [System.IO.File]::ReadAllBytes($target)
    Write-Output ("read back " + $back.Length + " bytes")
    if ($back.Length -eq $payloadBytes.Length) {
        $same = $true
        for ($i = 0; $i -lt $back.Length; $i++) {
            if ($back[$i] -ne $payloadBytes[$i]) { $same = $false; break }
        }
        Write-Output ("content match: " + $same)
        if (-not $same) { exit 4 }
    } else {
        Write-Output "FAIL: length mismatch"
        exit 4
    }
} catch {
    Write-Output ("FAIL: read failed: " + $_.Exception.Message)
    exit 4
}

try {
    [System.IO.File]::Delete($target)
    Write-Output ("deleted " + $target)
} catch {
    Write-Output ("WARN: delete failed: " + $_.Exception.Message)
}

Write-Section "4. removable / bus type"
# The volume is located by LABEL, so we only know its drive letter here.
# Get-Disk wants a disk NUMBER, not a drive letter: map letter -> partition -> disk.
$driveLetter = $root.TrimEnd('\').TrimEnd(':')
$part = Get-Partition -DriveLetter $driveLetter -ErrorAction SilentlyContinue
$disk = $null
if ($part -and $part.DiskNumber -ne $null) {
    $disk = Get-Disk -Number $part.DiskNumber -ErrorAction SilentlyContinue
}
if ($disk) {
    Write-Output ("disk bus type : " + $disk.BusType)
    Write-Output ("disk size     : " + [math]::Round($disk.Size / 1MB, 2) + " MB")
    Write-Output ("disk friendly : " + $disk.FriendlyName)
    Write-Output ("disk number   : " + $disk.Number)
    Write-Output ("is read-only  : " + $disk.IsReadOnly)
    Write-Output ("disk location : " + $disk.Location)
    # Get-Disk.IsRemovable can be empty for USB MSD, so report what Windows
    # actually decided at volume level: DriveType 2 = Removable.
    $volType = (Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$root'").DriveType
    $volTypeName = switch ($volType) { 2 { 'Removable' } 3 { 'Fixed' } 4 { 'Network' } default { "type$volType" } }
    Write-Output ("volume type   : " + $volTypeName + " (Win32_LogicalDisk.DriveType=" + $volType + ")")
} else {
    Write-Output ("WARN: could not map drive " + $driveLetter + " to a disk node (informational only)")
}

Write-Section "5. USB interface / endpoint evidence"
# Windows splits a single HID interface into collections by the report
# descriptor: COL01 = keyboard, COL02 = mouse. That is the proof that the
# composite report descriptor (Report ID 1 = keyboard, 2 = mouse) was parsed
# correctly by the host.
$hid = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
       Where-Object { $_.InstanceId -like "*$VID&$PID_HEX&MI_00*" }
if ($hid) {
    foreach ($h in $hid) {
        Write-Output ("  HID node : " + $h.Class + " | " + $h.FriendlyName + " | " + $h.InstanceId)
    }
} else {
    Write-Output ("WARN: no HID collection nodes for $VID&$PID_HEX&MI_00")
}
$msc = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
       Where-Object { $_.InstanceId -like "*$VID&$PID_HEX&MI_01*" }
if ($msc) {
    foreach ($m in $msc) {
        Write-Output ("  MSC node : " + $m.Class + " | " + $m.FriendlyName + " | " + $m.InstanceId)
    }
} else {
    Write-Output ("WARN: no MSC node for $VID&$PID_HEX&MI_01")
}

Write-Output ""
Write-Output "RESULT: M1 host-side checks completed"
exit 0
