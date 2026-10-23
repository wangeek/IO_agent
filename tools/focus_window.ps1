# Bring a window to the foreground by process name (read-only w.r.t. the app).
#
# Why: Start-Process does not steal foreground (Windows forbids background
# processes from doing that), so a freshly opened Notepad stays behind the
# current window and a HID typing test would land in the wrong app.
# UI Automation SetFocus does move the foreground.
#
# Pure ASCII source, no BOM.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -NoProfile -File tools\focus_window.ps1 -ProcName notepad

param(
    [string] $ProcName = 'notepad',
    [int]    $WaitMs = 800
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public class Fg {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
"@

function Get-ForegroundInfo {
    $h = [Fg]::GetForegroundWindow()
    $tb = New-Object System.Text.StringBuilder 512
    [void][Fg]::GetWindowTextW($h, $tb, 512)
    $cb = New-Object System.Text.StringBuilder 256
    [void][Fg]::GetClassNameW($h, $cb, 256)
    return ('hwnd=0x{0:X} class=[{1}] title=[{2}]' -f $h.ToInt64(), $cb.ToString(), $tb.ToString())
}

$procs = Get-Process -Name $ProcName -ErrorAction SilentlyContinue
if (-not $procs) {
    Write-Output ("no process named '{0}'" -f $ProcName)
    exit 1
}
$pid0 = $procs[0].Id

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $pid0)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
Write-Output ("process '{0}' pid={1} top-level windows={2}" -f $ProcName, $pid0, $wins.Count)

foreach ($w in $wins) {
    try {
        $w.SetFocus()
        Write-Output ('  SetFocus on hwnd=0x{0:X} class=[{1}]' -f $w.Current.NativeWindowHandle, $w.Current.ClassName)
    } catch {
        Write-Output ('  SetFocus failed: ' + $_.Exception.Message)
    }
}
Start-Sleep -Milliseconds $WaitMs

# Fallback: classic SetForegroundWindow on the native handle.
foreach ($w in $wins) {
    try {
        $h = [IntPtr]$w.Current.NativeWindowHandle
        [void][Fg]::SetForegroundWindow($h)
        [void][Fg]::ShowWindow($h, 9)   # SW_RESTORE
    } catch { }
}
Start-Sleep -Milliseconds $WaitMs

Write-Output ('foreground now: ' + (Get-ForegroundInfo))
exit 0
