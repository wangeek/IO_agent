# Dump the UI Automation tree of a window (read-only).
#
# Used to inspect a specific window on THIS host without pixels: what
# control is it, what does it say, what is its state.
#
# Output is code-point based (PowerShell 5.1 console mangles CJK).
# Pure ASCII source, no BOM.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -NoProfile -File tools\dump_uia.ps1 -Hwnd 0x703CE
#   powershell -ExecutionPolicy Bypass -NoProfile -File tools\dump_uia.ps1 -Class Windows.UI.Core.CoreWindow
#   powershell -ExecutionPolicy Bypass -NoProfile -File tools\dump_uia.ps1 -Pid 15424

param(
    [int64]   $Hwnd = 0,
    [string]  $Class = '',
    [int]     $Pid = 0,
    [int]     $MaxDepth = 4,
    [int]     $MaxNodes = 120
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

function Format-CodePoints {
    param([string] $Text)
    if ($null -eq $Text) { return '(null)' }
    if ($Text.Length -eq 0) { return '(empty)' }
    $ascii = ($Text.ToCharArray() | Where-Object { [int][char]$_ -ge 0x20 -and [int][char]$_ -le 0x7E }) -join ''
    if ($ascii.Length -eq $Text.Length) { return ("ASCII [{0}]" -f $Text) }
    $parts = @()
    foreach ($ch in $Text.ToCharArray()) { $parts += ('U+{0:X4}' -f [int][char]$ch) }
    return ('MIXED/NON-ASCII codepoints: {0}' -f ($parts -join ' '))
}

$root = [System.Windows.Automation.AutomationElement]::RootElement
$children = $root.FindAll([System.Windows.Automation.TreeScope]::Children,
                           [System.Windows.Automation.Condition]::TrueCondition)

$targets = @()
foreach ($el in $children) {
    try {
        $ok = $true
        if ($Hwnd -ne 0 -and $el.Current.NativeWindowHandle -ne $Hwnd) { $ok = $false }
        if ($Class -ne '' -and $el.Current.ClassName -ne $Class) { $ok = $false }
        if ($Pid -ne 0 -and $el.Current.ProcessId -ne $Pid) { $ok = $false }
        if ($ok) { $targets += $el }
    } catch { }
}

if ($targets.Count -eq 0) {
    Write-Output 'NO MATCHING TOP-LEVEL WINDOW'
    exit 0
}

$script:count = 0

function Dump-Node {
    param($El, [int] $Depth)
    if ($script:count -ge $MaxNodes) { return }
    $script:count++
    $pad = ' ' * ($Depth * 2)
    $line = '{0}{1}' -f $pad, $El.Current.ControlType.ProgrammaticName
    try { $line += ' name=' + (Format-CodePoints $El.Current.Name) } catch { }
    try { $line += ' class=[' + $El.Current.ClassName + ']' } catch { }
    try { $line += ' hwnd=0x{0:X}' -f $El.Current.NativeWindowHandle } catch { }
    try {
        $vp = $El.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        $line += ' value=' + (Format-CodePoints $vp.Current.Value)
    } catch { }
    try {
        $tp = $El.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        $line += ' toggle=' + $tp.Current.ToggleState.ToString()
    } catch { }
    try {
        $sp = $El.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        $line += ' selected=' + $sp.Current.IsSelected
    } catch { }
    Write-Output $line

    if ($Depth -ge $MaxDepth) { return }
    $kids = $El.FindAll([System.Windows.Automation.TreeScope]::Children,
                         [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($k in $kids) {
        Dump-Node -El $k -Depth ($Depth + 1)
    }
}

foreach ($t in $targets) {
    Write-Output ('=== window pid={0} hwnd=0x{1:X} ===' -f $t.Current.ProcessId, $t.Current.NativeWindowHandle)
    Dump-Node -El $t -Depth 0
}
exit 0
