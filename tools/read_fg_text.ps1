# Dump the text content of the foreground window (read-only).
#
# Used to verify, objectively, whether HID keystrokes reached an application:
# open a text editor host-side, let the device type into it, then read the
# content back through UI Automation (TextPattern / ValuePattern).
#
# Output is code-point based (PowerShell 5.1 console mangles CJK).
# Pure ASCII source, no BOM.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -NoProfile -File tools\read_fg_text.ps1

param(
    [int] $MaxChars = 400
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public class FgWin {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
}
"@

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

$h = [FgWin]::GetForegroundWindow()
$tb = New-Object System.Text.StringBuilder 512
[void][FgWin]::GetWindowTextW($h, $tb, 512)
$cb = New-Object System.Text.StringBuilder 256
[void][FgWin]::GetClassNameW($h, $cb, 256)
Write-Output ('foreground hwnd=0x{0:X} class=[{1}] title={2}' -f `
    $h.ToInt64(), $cb.ToString(), (Format-CodePoints $tb.ToString()))

$el = [System.Windows.Automation.AutomationElement]::FromHandle($h)
if ($null -eq $el) { Write-Output '  (no UIA element)'; exit 0 }

$all = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                    [System.Windows.Automation.Condition]::TrueCondition)
$n = 0
foreach ($node in $all) {
    if ($n -ge 20) { break }
    $text = $null
    try {
        $tp = $node.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
        $text = $tp.DocumentRange.GetText($MaxChars)
    } catch { }
    if ($null -eq $text) {
        try {
            $vp = $node.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
            $text = $vp.Current.Value
        } catch { }
    }
    if ($null -eq $text -or $text.Length -eq 0) { continue }
    $n++
    Write-Output ('  {0} name={1} text={2}' -f `
        $node.Current.ControlType.ProgrammaticName,
        (Format-CodePoints $node.Current.Name),
        (Format-CodePoints $text))
}
if ($n -eq 0) { Write-Output '  (no text-bearing element found)' }
exit 0
