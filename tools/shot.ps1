# Capture the screen to a PNG (read-only; for host-side verification).
#
# Pure ASCII source, no BOM. Uses GDI (same capture path as the watcher).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -NoProfile -File tools\shot.ps1 out.png [maxWidth]

param(
    [string] $Out = 'D:\Lab\IO_agent\.stage\screen.png',
    [int]    $MaxWidth = 1280
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap(
    [int]$b.Width, [int]$b.Height,
    [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
try {
    $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size,
        [System.Drawing.CopyPixelOperation]::SourceCopy)
} finally {
    $g.Dispose()
}

try {
    if ($bmp.Width -gt $MaxWidth) {
        $h = [int]([double]$bmp.Height * $MaxWidth / $bmp.Width)
        $small = New-Object System.Drawing.Bitmap($MaxWidth, $h)
        $g2 = [System.Drawing.Graphics]::FromImage($small)
        try {
            $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g2.DrawImage($bmp, (New-Object System.Drawing.Rectangle(0, 0, $MaxWidth, $h)))
        } finally {
            $g2.Dispose()
        }
        $bmp.Dispose()
        $bmp = $small
    }
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output ("saved {0}x{1} -> {2}" -f $bmp.Width, $bmp.Height, $Out)
} finally {
    $bmp.Dispose()
}
exit 0
