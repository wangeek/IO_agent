#requires -Version 5.1
<#
  test_watcher_diff.ps1 - deterministic test of the watcher's change detection.

  Synthetic frames, no mouse involved, so the result cannot be disturbed by a
  human moving the pointer.

  Cases:
    A. marker appears   - a filled disc drawn in frame B only must be detected
                           in the block that contains it.
    B. vertical bar     - a bar spanning 3 block rows must merge into ONE
                           rectangle 3 blocks tall. Before the merge fix every
                           rectangle came out exactly one block tall.
    C. nothing changed  - identical frames must produce no rectangles.

  ASCII only.
#>

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$Block = 16
$Step  = 3
$W = 960
$H = 540

function Get-BlockHashes {
    param([System.Drawing.Bitmap] $Bmp, [int] $Block, [int] $Step)
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
    for ($by = 0; $by -lt $rows; $by++) {
        $y0 = $by * $Block
        $yMax = [Math]::Min($y0 + $Block, $Bmp.Height) - 1
        for ($bx = 0; $bx -lt $cols; $bx++) {
            $x0 = $bx * $Block
            $xMax = [Math]::Min($x0 + $Block, $Bmp.Width) - 1
            $acc = 0
            for ($y = $y0; $y -le $yMax; $y += $Step) {
                $rowOff = $y * $stride
                for ($x = $x0; $x -le $xMax; $x += $Step) {
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
            if ($null -ne $open -and $open[3] -eq ($by - 1) -and
                $runX0 -le $open[2] -and $runX1 -ge $open[1]) {
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

function New-Frame {
    param([int] $W, [int] $H)
    return (New-Object System.Drawing.Bitmap($W, $H,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb))
}

function Fill-Disc {
    param([System.Drawing.Bitmap] $Bmp, [int] $X, [int] $Y, [int] $R)
    $g = [System.Drawing.Graphics]::FromImage($Bmp)
    try {
        $b = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Red)
        try { $g.FillEllipse($b, ($X - $R), ($Y - $R), ($R * 2), ($R * 2)) }
        finally { $b.Dispose() }
    } finally { $g.Dispose() }
}

function Fill-Bar {
    param([System.Drawing.Bitmap] $Bmp, [int] $X, [int] $Y, [int] $Wpx, [int] $Hpx)
    $g = [System.Drawing.Graphics]::FromImage($Bmp)
    try {
        $b = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Blue)
        try { $g.FillRectangle($b, $X, $Y, $Wpx, $Hpx) }
        finally { $b.Dispose() }
    } finally { $g.Dispose() }
}

$failures = 0
function Check {
    param([string] $Name, [bool] $Ok, [string] $Detail)
    if ($Ok) {
        Write-Output ("PASS  " + $Name + "  " + $Detail)
    } else {
        Write-Output ("FAIL  " + $Name + "  " + $Detail)
        $script:failures++
    }
}

# --- case C: identical frames ---------------------------------------------
$a = New-Frame $W $H
$b = New-Frame $W $H
$gA = Get-BlockHashes $a $Block $Step
$gB = Get-BlockHashes $b $Block $Step
$r = Get-ChangedRects $gB.Hashes $gA.Hashes $gB.Cols $gB.Rows $Block
Check "C identical frames" (@($r).Count -eq 0) ("rects=" + @($r).Count)

# --- case A: cursor disc appears ------------------------------------------
$cx = 437
$cy = 125
Fill-Disc $b $cx $cy 6
$gB = Get-BlockHashes $b $Block $Step
$r = @(Get-ChangedRects $gB.Hashes $gA.Hashes $gB.Cols $gB.Rows $Block)
$bx = [int] [Math]::Floor($cx / $Block)
$by = [int] [Math]::Floor($cy / $Block)
$hit = $false
foreach ($q in $r) {
    $rx0 = $q[0]; $ry0 = $q[1]; $rx1 = $q[0] + $q[2]; $ry1 = $q[1] + $q[3]
    if ($bx * $Block -ge $rx0 -and $bx * $Block -lt $rx1 -and
        $by * $Block -ge $ry0 -and $by * $Block -lt $ry1) { $hit = $true }
}
Check "A cursor disc detected" $hit ("rects=" + (($r | ForEach-Object { '[' + ($_ -join ',') + ']' }) -join ' '))

# --- case B: vertical bar spanning 3 block rows ---------------------------
$a2 = New-Frame $W $H
$b2 = New-Frame $W $H
$barX = 320
$barY = 200
$barW = 10
$barH = 60      # spans block rows 12, 13, 14, 15
Fill-Bar $b2 $barX $barY $barW $barH
$ga2 = Get-BlockHashes $a2 $Block $Step
$gb2 = Get-BlockHashes $b2 $Block $Step
$r2 = @(Get-ChangedRects $gb2.Hashes $ga2.Hashes $gb2.Cols $gb2.Rows $Block)
$maxH = 0
foreach ($q in $r2) { if ($q[3] -gt $maxH) { $maxH = $q[3] } }
Check "B vertical bar merged into one tall rect" ($r2.Count -eq 1 -and $maxH -ge 48) `
    ("rects=" + $r2.Count + " maxH=" + $maxH + " (expect 1 rect, height >= 48)")

$a.Dispose(); $b.Dispose(); $a2.Dispose(); $b2.Dispose()

Write-Output ""
if ($failures -eq 0) {
    Write-Output "RESULT: all watcher diff checks passed"
    exit 0
}
Write-Output ("RESULT: " + $failures + " check(s) failed")
exit 1
