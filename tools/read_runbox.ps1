# Read (and optionally close) the Windows "Run" dialog on THIS host.
#
# Why: the injection path types an ASCII command into the Run box. If the
# machine's IME is in Chinese mode, those keystrokes land in the pinyin
# buffer instead of the edit control, and the command gets mangled into
# Chinese. This script is the objective read-back that tells us which mode
# the IME was in when the device typed.
#
# Two lessons are baked in here:
#   * Win11's Win+R dialog is a XAML host (class "Windows.UI.Core.CoreWindow"),
#     NOT the classic "#32770" - matching on "#32770" alone finds nothing.
#   * Output is code-point based on purpose: PowerShell 5.1 console encoding
#     mangles CJK, and "was it ASCII or Chinese" must be unambiguous.
#
# Pure ASCII source, no BOM (repo convention: Windows PowerShell 5.1 reads
# .ps1 without a BOM as ANSI).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -NoProfile -File tools\read_runbox.ps1
#   powershell -ExecutionPolicy Bypass -NoProfile -File tools\read_runbox.ps1 -Close
#   powershell -ExecutionPolicy Bypass -NoProfile -File tools\read_runbox.ps1 -ListOnly

param(
    [switch] $Close,
    [switch] $Clear,
    [switch] $ListOnly,
    [int]    $WaitMs = 0
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class RunBoxWin {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool PostMessageW(IntPtr h, uint Msg, IntPtr wParam, IntPtr lParam);

    public class WinInfo {
        public IntPtr Hwnd;
        public string Cls;
        public string Title;
        public uint Pid;
    }

    public static List<WinInfo> TopWindows() {
        var list = new List<WinInfo>();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(512);
            GetWindowTextW(h, sb, 512);
            string t = sb.ToString();
            var cb = new StringBuilder(256);
            GetClassNameW(h, cb, 256);
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            list.Add(new WinInfo { Hwnd = h, Cls = cb.ToString(), Title = t, Pid = pid });
            return true;
        }, IntPtr.Zero);
        return list;
    }

    /* Child windows of a dialog (class + hwnd). */
    public static List<WinInfo> Children(IntPtr parent) {
        var list = new List<WinInfo>();
        EnumChildWindows(parent, delegate(IntPtr h, IntPtr p) {
            var sb = new StringBuilder(512);
            GetWindowTextW(h, sb, 512);
            var cb = new StringBuilder(256);
            GetClassNameW(h, cb, 256);
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            list.Add(new WinInfo { Hwnd = h, Cls = cb.ToString(), Title = sb.ToString(), Pid = pid });
            return true;
        }, IntPtr.Zero);
        return list;
    }

    /*
     * WM_GETTEXT across process boundaries. The system marshals WM_GETTEXT
     * for standard controls (Edit), so this works on another process's
     * edit box - GetWindowText would not.
     */
    public static string GetControlText(IntPtr hwnd) {
        var sb = new StringBuilder(4096);
        SendMessageW(hwnd, 0x000D, (IntPtr)sb.Capacity, sb);
        return sb.ToString();
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, StringBuilder lp);

    /* WM_SETTEXT with an empty string clears an Edit control. */
    public static void ClearControl(IntPtr hwnd) {
        var empty = new StringBuilder("");
        SendMessageW(hwnd, 0x000C, (IntPtr)0, empty);
    }
}
"@

function Format-CodePoints {
    param([string] $Text)
    if ($null -eq $Text) { return '(null)' }
    if ($Text.Length -eq 0) { return '(empty)' }
    $ascii = ($Text.ToCharArray() | Where-Object { [int][char]$_ -ge 0x20 -and [int][char]$_ -le 0x7E }) -join ''
    if ($ascii.Length -eq $Text.Length) {
        return ("ASCII [{0}]" -f $Text)
    }
    $parts = @()
    foreach ($ch in $Text.ToCharArray()) { $parts += ('U+{0:X4}' -f [int][char]$ch) }
    return ('MIXED/NON-ASCII codepoints: {0}' -f ($parts -join ' '))
}

if ($WaitMs -gt 0) { Start-Sleep -Milliseconds $WaitMs }

$wins = [RunBoxWin]::TopWindows()

Write-Output '=== top-level visible windows ==='
foreach ($w in $wins) {
    if ($w.Title.Length -eq 0) { continue }
    Write-Output ('  hwnd=0x{0:X} pid={1} class=[{2}] title={3}' -f `
        $w.Hwnd.ToInt64(), $w.Pid, $w.Cls, (Format-CodePoints $w.Title))
}

if ($ListOnly) { exit 0 }

# --- Win32 path: find the Run dialog and read its Edit control directly. ---
# UIA does not always expose the Run dialog's edit box, but WM_GETTEXT on a
# standard Edit control crosses the process boundary just fine.
Write-Output '=== run dialog edit (WM_GETTEXT) ==='
$runFound = 0
foreach ($w in $wins) {
    if ($w.Cls -ne '#32770') { continue }
    $kids = [RunBoxWin]::Children($w.Hwnd)
    $edits = $kids | Where-Object { $_.Cls -eq 'Edit' }
    if (-not $edits) { continue }
    $runFound++
    Write-Output ('  DIALOG hwnd=0x{0:X} pid={1} title={2}' -f `
        $w.Hwnd.ToInt64(), $w.Pid, (Format-CodePoints $w.Title))
    foreach ($e in $edits) {
        $txt = [RunBoxWin]::GetControlText($e.Hwnd)
        Write-Output ('    EDIT hwnd=0x{0:X} value={1}' -f $e.Hwnd.ToInt64(), (Format-CodePoints $txt))
        if ($Clear) {
            [RunBoxWin]::ClearControl($e.Hwnd)
            Write-Output '    -> cleared (WM_SETTEXT "")'
        }
    }
    if ($Close) {
        [void][RunBoxWin]::PostMessageW($w.Hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        Write-Output '    -> WM_CLOSE posted'
    }
}
if ($runFound -eq 0) { Write-Output '  (no #32770 dialog with an Edit child)' }

# Candidate dialogs: classic dialog class, or the Win11 XAML run-dialog host.
$candidateClasses = @('#32770', 'Windows.UI.Core.CoreWindow')

Write-Output '=== edit controls of candidate dialogs ==='
$hit = 0
foreach ($w in $wins) {
    if ($candidateClasses -notcontains $w.Cls) { continue }
    if ($w.Title.Length -eq 0) { continue }
    try {
        $el = [System.Windows.Automation.AutomationElement]::FromHandle($w.Hwnd)
        if ($null -eq $el) { continue }
        $editCond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Edit)
        $edits = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants, $editCond)
        if ($edits.Count -eq 0) { continue }
        $hit++
        Write-Output ('  DIALOG hwnd=0x{0:X} class=[{1}] title={2}' -f `
            $w.Hwnd.ToInt64(), $w.Cls, (Format-CodePoints $w.Title))
        $i = 0
        foreach ($ed in $edits) {
            $i++
            $value = $null
            try {
                $vp = $ed.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
                $value = $vp.Current.Value
            } catch {
                try { $value = $ed.Current.Name } catch { $value = '(unreadable)' }
            }
            Write-Output ('    EDIT#{0} value={1}' -f $i, (Format-CodePoints $value))
        }
        if ($Close) {
            [void][RunBoxWin]::PostMessageW($w.Hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            Write-Output '    -> WM_CLOSE posted'
        }
    } catch { }
}
if ($hit -eq 0) { Write-Output '  (no candidate dialog with an Edit control)' }
exit 0
