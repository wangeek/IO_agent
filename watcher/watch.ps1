<#
.SYNOPSIS
    IO_agent target-side watcher (v1). Zero-install, hidden, runs from the
    virtual USB volume served by the ESP32-S3.

.DESCRIPTION
    Responsibilities (contract: docs/implementation-plan.md 4.1):
      CAP.TXT    - capability self-report (os/arch/admin/net/screen/version)
      SNAP_P.JPG- rolling preview frame, 960x540, JPEG quality 70, 2 fps
      SNAP_A.JPG- full-resolution frame, captured on demand
      SNAP.IDX   - current frame name + length + sequence (double-read safe)
      DIFF.IDX   - changed-region list (16x16 block hash), JSON single line
      SNAP_R.JPG - JPEG cropped to the changed region (N1: crop happens here)

    Rules that are not negotiable:
      - Never write to the target machine's own disk. Everything goes to the
        virtual volume.
      - If a write fails, the carrier is gone: exit immediately. No lingering
        process, no half-written state left behind.
      - The drive is located by VOLUME LABEL, never by a hardcoded letter.
      - GDI CopyFromScreen does NOT include the mouse pointer, so the cursor is
        synthesized into the frame and its position is reported in SNAP.IDX.

    ASCII only: Windows PowerShell 5.1 reads .ps1 without a BOM as ANSI.

.PARAMETER Root
    Target volume root. Default: locate by volume label IOAGENT.

.PARAMETER Iterations
    Number of capture cycles. 0 = run forever (production).

.PARAMETER Fps
    Preview frame rate. Default 2.

.PARAMETER Label
    Volume label to search for. Default IOAGENT.

.PARAMETER LogPath
    Optional log file for diagnostics. Empty = no logging.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File watch.ps1 -Root X:\ -Iterations 1
#>
[CmdletBinding()]
param(
    [string] $Root = '',
    [int]    $Iterations = 0,
    [double] $Fps = 2.0,
    [string] $Label = 'IOAGENT',
    [string] $LogPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# ------------------------------------------------------------------ constants
$PREVIEW_W   = 960
$PREVIEW_H   = 540
$PREVIEW_Q   = 70
$FULL_Q      = 90
$BLOCK       = 16
$SEQ_MOD     = 1000000007

$FILE_PREVIEW = 'SNAP_P.JPG'
$FILE_FULL    = 'SNAP_A.JPG'
$FILE_ROI     = 'SNAP_R.JPG'
$FILE_IDX     = 'SNAP.IDX'
$FILE_DIFF    = 'DIFF.IDX'
$FILE_CAP     = 'CAP.TXT'
$DIR_CMD      = 'CMD'
$DIR_RESP     = 'RESP'

# ------------------------------------------------------------------ helpers
function Write-Log {
    param([string] $Message)
    if ([string]::IsNullOrEmpty($LogPath)) { return }
    try {
        $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Message
        [System.IO.File]::AppendAllText($LogPath, $line + "`r`n")
    } catch { }
}

function Get-VolumeRoot {
    param([string] $WantLabel)
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        try {
            if ($d.IsReady -and $d.VolumeLabel -eq $WantLabel -and $d.DriveType -eq 'Removable') {
                return $d.RootDirectory.FullName
            }
        } catch { }
    }
    # Fall back to any ready volume with the label (some hosts report the
    # virtual MSC volume as Fixed rather than Removable).
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        try {
            if ($d.IsReady -and $d.VolumeLabel -eq $WantLabel) {
                return $d.RootDirectory.FullName
            }
        } catch { }
    }
    return $null
}

$script:tmpSeq = 0
function Get-TempName {
    <# The volume is FAT16 WITHOUT long-name support, so every name the
       watcher creates must be a legal 8.3 name. "SNAP_P.JPG.TMP" is not,
       hence a fixed pool of short temp names. #>
    $script:tmpSeq = ($script:tmpSeq + 1) % 8
    return '~T' + $script:tmpSeq + '.TMP'
}

function Write-Atomic {
    <# Write bytes to $Path via a temp file + rename so the device never
       observes a half-written file. #>
    param([string] $Path, [byte[]] $Bytes)
    $dir = [System.IO.Path]::GetDirectoryName($Path)
    $tmp = [System.IO.Path]::Combine($dir, (Get-TempName))
    [System.IO.File]::WriteAllBytes($tmp, $Bytes)
    if ([System.IO.File]::Exists($Path)) { [System.IO.File]::Delete($Path) }
    [System.IO.File]::Move($tmp, $Path)
}

function Get-JpegBytes {
    param([System.Drawing.Image] $Image, [int] $Quality)
    $codec = $null
    foreach ($c in [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders()) {
        if ($c.MimeType -eq 'image/jpeg') { $codec = $c; break }
    }
    if ($null -eq $codec) { throw 'JPEG encoder not found' }

    $enc = New-Object System.Drawing.Imaging.EncoderParameters(1)
    $enc.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
        [System.Drawing.Imaging.Encoder]::Quality, [long] $Quality)
    $ms = New-Object System.IO.MemoryStream
    try {
        $Image.Save($ms, $codec, $enc)
        return $ms.ToArray()
    } finally {
        $ms.Dispose()
    }
}

# ------------------------------------------------------------------ P/Invoke
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class IoAgentNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }
    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT lpPoint);
    [DllImport("user32.dll")]
    public static extern bool GetForegroundWindow();
}
"@

# ------------------------------------------------------------------ capture
function Get-ScreenBounds {
    return [System.Windows.Forms.SystemInformation]::VirtualScreen
}

function Capture-Screen {
    <# Returns a 24bpp bitmap of the whole virtual desktop. #>
    $b = Get-ScreenBounds
    $bmp = New-Object System.Drawing.Bitmap(
        [int] $b.Width, [int] $b.Height,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size,
            [System.Drawing.CopyPixelOperation]::SourceCopy)
    } finally {
        $g.Dispose()
    }
    return $bmp
}

function Draw-CursorMarker {
    <# GDI does not capture the pointer. Synthesize a marker so the agent can
       see where the pointer is.

       The marker must be a FILLED disc, not an outline. The diff grid samples
       every 3rd pixel, and an outline collapses to a ~1px arc once the capture
       is scaled down to the preview size - the sparse samples then land
       between the strokes, the block hash stays identical and the diff
       reports "nothing changed" while the pointer clearly moved. A filled disc
       is several pixels wide in preview space, so at least one sample always
       falls inside it. #>
    param([System.Drawing.Bitmap] $Bmp)
    $pt = New-Object IoAgentNative+POINT
    if (-not [IoAgentNative]::GetCursorPos([ref] $pt)) { return }
    $bounds = Get-ScreenBounds
    # X and Y scale independently: the preview is 960x540 while the captured
    # desktop can be any aspect ratio (this host is 1536x960), so one ratio for
    # both axes would place the marker off its true position.
    $scaleX = [double] $Bmp.Width / [double] $bounds.Width
    $scaleY = [double] $Bmp.Height / [double] $bounds.Height
    $x = [int] (($pt.X - $bounds.X) * $scaleX)
    $y = [int] (($pt.Y - $bounds.Y) * $scaleY)
    $r = 6
    $g = [System.Drawing.Graphics]::FromImage($Bmp)
    try {
        $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Red)
        try { $g.FillEllipse($brush, ($x - $r), ($y - $r), ($r * 2), ($r * 2)) }
        finally { $brush.Dispose() }
    } finally {
        $g.Dispose()
    }
}

function Scale-Bitmap {
    param([System.Drawing.Image] $Src, [int] $W, [int] $H)
    $dst = New-Object System.Drawing.Bitmap($W, $H,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($dst)
    try {
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.DrawImage($Src, (New-Object System.Drawing.Rectangle(0, 0, $W, $H)))
    } finally {
        $g.Dispose()
    }
    return $dst
}

# ------------------------------------------------------------------ diff
function Get-BlockHashes {
    <# Sparse-sampled 16x16 block checksums over a 24bpp bitmap.
       Sparse sampling keeps this cheap enough for 2 fps in PowerShell. #>
    param([System.Drawing.Bitmap] $Bmp, [int] $Block)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $Bmp.Width, $Bmp.Height)
    $locked = $Bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $stride = 0
    $bytes = $null
    try {
        $stride = $locked.Stride
        $bytes = New-Object byte[] ([int] ($stride * $Bmp.Height))
        [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $bytes, 0, $bytes.Length)
    } finally {
        $Bmp.UnlockBits($locked)
    }

    $cols = [int] [Math]::Ceiling($Bmp.Width / $Block)
    $rows = [int] [Math]::Ceiling($Bmp.Height / $Block)
    $hashes = New-Object 'int[]' ($cols * $rows)
    # Sampling step inside a 16x16 block. 3 keeps the cursor marker and thin
    # UI strokes visible to the diff; 4 was fast enough to miss both.
    $step = 3
    for ($by = 0; $by -lt $rows; $by++) {
        $y0 = $by * $Block
        $yMax = [Math]::Min($y0 + $Block, $Bmp.Height) - 1
        for ($bx = 0; $bx -lt $cols; $bx++) {
            $x0 = $bx * $Block
            $xMax = [Math]::Min($x0 + $Block, $Bmp.Width) - 1
            $acc = 0
            for ($y = $y0; $y -le $yMax; $y += $step) {
                $rowOff = $y * $stride
                for ($x = $x0; $x -le $xMax; $x += $step) {
                    $i = $rowOff + ($x * 3)
                    $acc = ($acc + ($bytes[$i] * 3) + ($bytes[$i + 1] * 5) + ($bytes[$i + 2] * 7)) % 2147483647
                }
            }
            $hashes[($by * $cols) + $bx] = $acc
        }
    }
    return @{ Hashes = $hashes; Cols = $cols; Rows = $rows }
}

function Get-ChangedRects {
    <# Compare two block-hash grids, then merge changed blocks into rectangles.
       Returns an array of int[4] = x,y,w,h in bitmap space. #>
    param([int[]] $Cur, [int[]] $Prev, [int] $Cols, [int] $Rows, [int] $Block)

    $grid = New-Object 'bool[]' ($Cols * $Rows)
    $changed = 0
    for ($i = 0; $i -lt $Cur.Length; $i++) {
        if ($null -eq $Prev -or $Cur[$i] -ne $Prev[$i]) { $grid[$i] = $true; $changed++ }
    }
    if ($changed -eq 0) { return @() }

    $rects = New-Object System.Collections.ArrayList
    $open = $null
    for ($by = 0; $by -lt $Rows; $by++) {
        $x = 0
        while ($x -lt $Cols) {
            if (-not $grid[($by * $Cols) + $x]) { $x++; continue }
            $start = $x
            while ($x -lt $Cols -and $grid[($by * $Cols) + $x]) { $x++ }
            $runX0 = $start
            $runX1 = $x - 1
            # Merge with the open rectangle only when it ends on the previous
            # row. Comparing against the START row ($open[0]) instead of the
            # END row ($open[3]) can never extend a rectangle across more than
            # two rows, and the end row was never updated - so every rectangle
            # came out exactly one block tall and tall changes were shredded
            # into a pile of 1-row strips.
            if ($null -ne $open -and $open[3] -eq ($by - 1) -and
                $runX0 -le $open[2] -and $runX1 -ge $open[1]) {
                # extend the open rectangle horizontally
                if ($runX0 -lt $open[1]) { $open[1] = $runX0 }
                if ($runX1 -gt $open[2]) { $open[2] = $runX1 }
                $open[3] = $by
            } else {
                if ($null -ne $open) {
                    [void] $rects.Add($open)
                    $open = $null
                }
                $open = @($by, $runX0, $runX1, $by)
            }
        }
    }
    if ($null -ne $open) { [void] $rects.Add($open) }

    # convert grid coords to pixels
    $out = New-Object System.Collections.ArrayList
    foreach ($r in $rects) {
        $px = $r[1] * $Block
        $py = $r[0] * $Block
        $pw = ($r[2] - $r[1] + 1) * $Block
        $ph = ($r[3] - $r[0] + 1) * $Block
        [void] $out.Add(@($px, $py, $pw, $ph))
    }
    return $out.ToArray()
}

function Get-RoiBytes {
    <# Crop the changed region out of a full-resolution bitmap and encode it.
       Rects is an array of int[4] (x,y,w,h), so the parameter must stay
       object[] - a typed [int[]] would flatten the inner arrays. #>
    param([System.Drawing.Bitmap] $Bmp, [object[]] $Rects, [int] $Quality)
    if ($null -eq $Rects -or $Rects.Length -eq 0) { return $null }

    $minX = $Rects[0][0]; $minY = $Rects[0][1]
    $maxX = $Rects[0][0] + $Rects[0][2]; $maxY = $Rects[0][1] + $Rects[0][3]
    foreach ($r in $Rects) {
        if ($r[0] -lt $minX) { $minX = $r[0] }
        if ($r[1] -lt $minY) { $minY = $r[1] }
        if (($r[0] + $r[2]) -gt $maxX) { $maxX = $r[0] + $r[2] }
        if (($r[1] + $r[3]) -gt $maxY) { $maxY = $r[1] + $r[3] }
    }
    $w = [Math]::Min($maxX - $minX, $Bmp.Width - $minX)
    $h = [Math]::Min($maxY - $minY, $Bmp.Height - $minY)
    if ($w -le 0 -or $h -le 0) { return $null }

    $rect = New-Object System.Drawing.Rectangle($minX, $minY, $w, $h)
    $crop = $Bmp.Clone($rect, $Bmp.PixelFormat)
    try {
        return (Get-JpegBytes $crop $Quality)
    } finally {
        $crop.Dispose()
    }
}

# ------------------------------------------------------------------ capability
function Get-Capability {
    $admin = $false
    try {
        $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
        $pr = New-Object System.Security.Principal.WindowsPrincipal($id)
        $admin = $pr.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { }

    $net = $false
    try {
        $net = [System.Net.NetworkInformation.NetworkInterface]::GetIsNetworkAvailable()
    } catch { }

    $b = Get-ScreenBounds
    $ver = '0.1.0'
    $json = '{{"os":"{0}","arch":"{1}","admin":{2},"net":{3},"screen":"{4}x{5}","watcher_ver":"{6}"}}' -f `
        [System.Environment]::OSVersion.Platform, `
        $env:PROCESSOR_ARCHITECTURE, `
        $(if ($admin) { 'true' } else { 'false' }), `
        $(if ($net) { 'true' } else { 'false' }), `
        $b.Width, $b.Height, $ver
    return $json
}

# ------------------------------------------------------------------ main
$driveRoot = $Root
if ([string]::IsNullOrEmpty($driveRoot)) {
    $driveRoot = Get-VolumeRoot $Label
    if ($null -eq $driveRoot) {
        Write-Log "volume '$Label' not found"
        exit 2
    }
}
$driveRoot = $driveRoot.TrimEnd('\') + '\'
Write-Log "watcher root = $driveRoot"

$seq = 0
$prevHashes = $null
$interval = [int] [Math]::Round(1000.0 / $Fps)
$cmdSeen = @{}

try {
    Write-Atomic (Join-Path $driveRoot $FILE_CAP) `
        ([System.Text.Encoding]::ASCII.GetBytes((Get-Capability)))
} catch {
    Write-Log "CAP write failed: $($_.Exception.Message)"
    exit 3
}

$iter = 0
while ($true) {
    $iter++
    $t0 = Get-Date

    # --- carrier liveness: the volume must still be there.
    if (-not [System.IO.Directory]::Exists($driveRoot)) {
        Write-Log 'carrier gone: volume directory vanished'
        exit 4
    }

    # --- command inbox (device -> watcher). Files are consumed by rewriting
    #     them into RESP with a terminal state.
    try {
        $cmdDir = Join-Path $driveRoot $DIR_CMD
        if ([System.IO.Directory]::Exists($cmdDir)) {
            foreach ($f in [System.IO.Directory]::GetFiles($cmdDir, '*.cmd')) {
                $name = [System.IO.Path]::GetFileName($f)
                if ($cmdSeen.ContainsKey($name)) { continue }
                $cmdSeen[$name] = $true
                $body = ''
                try { $body = [System.IO.File]::ReadAllText($f) } catch { }
                $resp = '{""id"":""{0}"",""st"":""OK"",""echo"":""{1}""}' -f $name, `
                    ($body -replace '"', '""')
                $respDir = Join-Path $driveRoot $DIR_RESP
                if (-not [System.IO.Directory]::Exists($respDir)) {
                    [void] [System.IO.Directory]::CreateDirectory($respDir)
                }
                # 8.3 only: strip the extension before adding .RSP
                $respName = [System.IO.Path]::GetFileNameWithoutExtension($name) + '.RSP'
                Write-Atomic (Join-Path $respDir $respName) `
                    ([System.Text.Encoding]::ASCII.GetBytes($resp))
            }
        }
    } catch {
        Write-Log "cmd inbox failed: $($_.Exception.Message)"
    }

    # --- capture
    $full = $null
    try {
        $full = Capture-Screen
        $prevCursor = New-Object IoAgentNative+POINT
        [void] [IoAgentNative]::GetCursorPos([ref] $prevCursor)

        $preview = Scale-Bitmap $full $PREVIEW_W $PREVIEW_H
        try {
            Draw-CursorMarker $preview

            $bytes = Get-JpegBytes $preview $PREVIEW_Q
            Write-Atomic (Join-Path $driveRoot $FILE_PREVIEW) $bytes

            # diff over the preview grid. The very first frame has no
            # baseline, so it is not a diff - it only seeds the baseline.
            $grid = Get-BlockHashes $preview $BLOCK
            if ($null -eq $prevHashes) {
                $rects = @()
            } else {
                $rects = Get-ChangedRects $grid.Hashes $prevHashes $grid.Cols $grid.Rows $BLOCK
            }
            $prevHashes = $grid.Hashes

            $seq = ($seq + 1) % $SEQ_MOD

            # ROI crop lives in FULL resolution space: map preview blocks back.
            $scaleX = $full.Width / [double] $PREVIEW_W
            $scaleY = $full.Height / [double] $PREVIEW_H
            $fullRects = @()
            foreach ($r in $rects) {
                $fullRects += , @(
                    [int] [Math]::Floor($r[0] * $scaleX),
                    [int] [Math]::Floor($r[1] * $scaleY),
                    [int] [Math]::Ceiling($r[2] * $scaleX),
                    [int] [Math]::Ceiling($r[3] * $scaleY))
            }

            $roiBytes = Get-RoiBytes $full $fullRects $FULL_Q
            $roiPath = Join-Path $driveRoot $FILE_ROI
            if ($null -ne $roiBytes) {
                Write-Atomic $roiPath $roiBytes
                $roiName = $FILE_ROI
                $roiLen = $roiBytes.Length
            } else {
                # No change: drop the stale ROI crop so the device can never
                # pick up a frame that does not match SNAP.IDX.
                try { if ([System.IO.File]::Exists($roiPath)) { [System.IO.File]::Delete($roiPath) } } catch { }
                $roiName = ''
                $roiLen = 0
            }

            # SNAP.IDX: name + length + seq + cursor. Written last so that a
            # reader that sees this index sees a complete frame behind it.
            $idx = '{{"name":"{0}","len":{1},"seq":{2},"cursor":[{3},{4}],"roi":{{"file":"{5}","len":{6},"rects":{7}}}}}' -f `
                $FILE_PREVIEW, $bytes.Length, $seq, $prevCursor.X, $prevCursor.Y, `
                $roiName, $roiLen, `
                (($fullRects | ForEach-Object { '[' + ($_ -join ',') + ']' }) -join ',')
            Write-Atomic (Join-Path $driveRoot $FILE_IDX) `
                ([System.Text.Encoding]::ASCII.GetBytes($idx))

            # DIFF.IDX: changed region list only (single line JSON)
            $diff = '{{"seq":{0},"grid":[{1},{2}],"rects":[{3}]}}' -f `
                $seq, $grid.Cols, $grid.Rows, `
                (($fullRects | ForEach-Object { '[' + ($_ -join ',') + ']' }) -join ',')
            Write-Atomic (Join-Path $driveRoot $FILE_DIFF) `
                ([System.Text.Encoding]::ASCII.GetBytes($diff))
        } finally {
            $preview.Dispose()
        }
    } catch {
        # 写失败**不等于**载波丢了:设备正在读盘(observe 会读盘)时,宿主
        # 这一侧的文件改名/写入可能撞车。载波真丢了(卷目录不见了)才退出;
        # 否则重试 —— "watcher 跑两帧就没了"就是撞车导致的。
        Write-Log "capture/write failed (iter $iter): $($_.Exception.Message)"
        if (-not [System.IO.Directory]::Exists($driveRoot)) {
            Write-Log 'carrier gone: volume directory vanished'
            exit 4
        }
        if ($iter -ge 5) {
            Write-Log 'giving up after 5 failed iterations'
            exit 5
        }
        Start-Sleep -Milliseconds 400
        continue
    } finally {
        if ($null -ne $full) { $full.Dispose() }
    }

    if ($Iterations -gt 0 -and $iter -ge $Iterations) { break }

    $elapsed = ((Get-Date) - $t0).TotalMilliseconds
    $sleep = $interval - $elapsed
    if ($sleep -gt 0) { Start-Sleep -Milliseconds ([int] $sleep) }
}

Write-Log "watcher finished after $iter iterations"
exit 0
