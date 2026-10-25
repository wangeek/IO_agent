<#
    check_watcher_gone.ps1 -- 拔盘 5s 退出回归:在**目标机**上跑。

用法(拔掉 dongle 的 USB 之后等 5~10 秒,再跑这条):
    powershell -ExecutionPolicy Bypass -File check_watcher_gone.ps1

预期:输出 "PASS: 没有残留的 watcher 进程"。

为什么要在目标机上查:
  dongle 拔线 = 整机断电,设备自己就消失了,读不到盘上的任何东西 ——
  拔盘退出这件事**只有目标机上的进程表能作证**。
  watch.ps1 的判据是"载体自检":写盘失败且卷目录消失 ⇒ 主动 exit 4
  (watcher/watch.ps1:每轮开头查一次,2 fps ⇒ 500 ms 一查;
   写失败重试最多 5 轮 × 400 ms ⇒ 最坏 2~3 s 内退出)。

关于"不往用户磁盘写日志":
  watch.ps1 默认 $LogPath 为空 —— 它**不在目标机磁盘上留任何痕迹**
  (无痕性验收项:拔插 10 次,目标机磁盘无新增持久化文件)。
  所以退出证据只能查进程,这是刻意的取舍,不是漏了。
#>

$ErrorActionPreference = 'Continue'

# 1) watcher 进程还在吗
$procs = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe' OR Name='pwsh.exe'" |
           Where-Object { $_.CommandLine -like '*WATCH.PS1*' })

if ($procs.Count -eq 0) {
    Write-Host "PASS: 没有残留的 watcher 进程" -ForegroundColor Green
    exit 0
}

Write-Host "FAIL: 还有 $($procs.Count) 个 watcher 进程没退:" -ForegroundColor Red
foreach ($p in $procs) {
    Write-Host ("  PID {0}  {1}" -f $p.ProcessId, $p.CommandLine)
}
Write-Host ""
Write-Host "如果刚拔盘不到 5 秒,等一会儿再跑一次 —— 判据是 5 秒内退出。" -ForegroundColor Yellow
exit 1
