# 新 dongle(单 Type-C)首次刷机与验收清单

适用对象:**全新的、没刷过任何固件的** ESP32-S3 单 Type-C dongle。
开发用板(双 Type-C DevKitC)走 `tools\idf.ps1 -p COM3 flash`,不走这份。

---

## 0. 先认清一个边界(最重要)

**WiFi OTA 装不了"第一个固件"。**
没有固件 ⇒ 没有 OTA 客户端 ⇒ 没有 WiFi ⇒ 没有 mDNS ⇒ 设备根本找不到。
⇒ **第一个固件必须用 USB 烧**,之后的每次更新才走 WiFi OTA。

这正是单 Type-C dongle 必须有 WiFi OTA 的原因:它插在目标机上就没有第二个
口留给开发机,**产线烧完、装机之后,固件更新只能靠 OTA**。

---

## 1. 硬件前提(必须先确认)

| 项 | 两种可能 | 影响 |
|---|---|---|
| **那个 USB 口接的是谁** | (a) 直接接 ESP32 的 **GPIO19/20**(原生 USB-OTG) | 芯片出厂 ROM 跑 **USB-Serial-JTAG** ⇒ 插上就是 COM 口 ⇒ esptool **自动进下载模式**,直接烧 ✅ |
| | (b) 接了个 **USB-UART 桥**(CH343/CH340) | COM 口是桥的 ⇒ esptool **没法自动进下载模式** ⇒ 产线要**手动按住 BOOT 键**再插上 ⚠ |
| **有没有 LED** | WS2812?几颗?哪个脚? | 决定 `CONFIG_SOPHON_LED_WS2812` / `GPIO` / `COUNT` 三个编译期开关 |
| **有没有引出 UART** | 有的话日志/配网多一条路;没有就只能走 WiFi 日志 + U 盘配网 | 决定验收方式 |

> 默认固件是按**开发板**配的:WS2812 单颗大颗粒 / GPIO48 / 亮度 24。
> 量产板 LED 不同就改 `firmware/main/Kconfig.projbuild` 里那三项(改完要
> `idf.ps1 reconfigure`,**注意 kconfig 会保留旧值,得直接改 sdkconfig**)。

---

## 2. 产线:烧第一个固件

```powershell
# 新板子插到**产线电脑**的 USB 口
python tools\flash_dongle.py --list      # 应该看到一个 vid=303a pid=1001 的 COM 口
python tools\flash_dongle.py             # 自动选口 + 烧 4 个镜像
```

烧完的预期:
- 那个 COM 口**消失**(USB-Serial-JTAG 退场)
- 出现 **HID+MSC 复合设备**:目标机上多出"键盘""鼠标""可移动磁盘(卷标 `SOPHON`)"

产线电脑**不需要装 ESP-IDF**,只要有 `esptool` + `firmware/build/` 里的 4 个 bin。

---

## 3. 产线:配网(必须在装机前做)

设备还没联网 ⇒ 只能走 **U 盘**(宿主写 → 设备自己取,方向天然对):

```powershell
python tools\provision.py --ssid <SSID> --pass <PASS> --name <设备名> --log-ips <开发机IP>
```

- 按**卷标** `SOPHON` 找盘(不写死盘符:拔线即断电,重入后 Windows 可能换盘符)
- 写 `DEV.CFG` 到盘上,设备**每 5 秒**看一眼,变了就应用(首次约 5~10 秒生效)
- `logips=` 是 WiFi 日志通道的**源 IP 授权表**(不配就不开日志,零开销)

## 4. 装机

把 dongle 从产线电脑拔下,插到**目标机**。

---

## 5. 验收(开发机操作,不需要碰目标机,除注入)

| # | 项 | 怎么做 | 判据 |
|---|---|---|---|
| 1 | mDNS 发现 | `python .minimax\skills\sophon-mcp\scripts\discover.py --name <名字> --json` | `found: true` 且拿到 URL |
| 2 | MCP 配置同步 | `python .minimax\skills\sophon-mcp\scripts\sophon_mcp_config.py` | 报"已指向它了"或"已更新" |
| 3 | 设备身份 | `mcp_call.py --status` | `ok:true` / `led` / `free_heap` / `uptime_s` 正常 |
| 4 | 共享盘 | `mcp_call.py --tool ls` | 4 个出厂槽位 + watcher 起来后有 `CAP.TXT`/帧 |
| 5 | 读文件 | `mcp_call.py --tool read --args '{"name":"WATCH.PS1"}'` | 返回内容 |
| 6 | 截图 | `mcp_call.py --tool capture --out frame.jpg` | 拿到真实桌面帧 |
| 7 | 注入自检 | `mcp_call.py --tool input --args '{"action":"macro","macro":"selftest"}'` | 设备报 selftest 通过(IME 态探针) |
| 8 | 拉起 watcher | `mcp_call.py --tool input --args '{"action":"macro","macro":"launch_watcher"}'` | `CAP.TXT` 出现 + 开始出帧 |
| 9 | LED | 看灯 | 就绪=🟢常亮;动作中=🔵常亮;启动中=🔵快闪;网络未就绪=🟡慢闪;故障=🔴常亮 |
| 10 | WiFi 日志 | `python tools\log_listener.py --host <IP>` | 能收开机日志;`status.log.rx` 涨、`dropped` 不涨 |
| 11 | **OTA 二刷** | 开发机 `python tools\ota_server.py --port 8000`,设备侧 `mcp_call.py --tool flash --args '{"url":"http://<开发机IP>:8000/sophon.bin"}'` | 应答 <1s;设备重启;`status.ota.boot` **翻到另一个槽** |
| 12 | 拔盘退出 | 目标机上起 watcher → 拔 dongle → 等 5~10s → 查进程 | `Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" \| Where-Object { $_.CommandLine -like '*WATCH.PS1*' }` **输出为空 = PASS** |

**第 11 项是这次刷机验证的核心**:它证明这块**只有一个口**的 dongle 以后能靠 OTA 活下来。

---

## 6. 已知要盯的地方

- **明文 HTTP + 目前无鉴权** ⇒ 同网段任何主机都能远程调 `input` 往目标机注入按键。
  **量产前这是 P0**(README P3/P5 都还挂着)。验证阶段只在可信网段做。
- **注入是破坏性操作**:主力机上只能"人在场 + 知情 + 手动单次触发",
  设备端**不自动重试**(只有 IME 探针会试两次 —— 探针只写共享盘上自己的
  标记文件,不碰用户环境)。
- **IME**:每次注入都新开一个 Win+R 框 ⇒ 新框 = 该系统默认 IME 态。
  设备端现在**自动探**出该带 Shift 还是不带(探针试两次),
  别再手工猜 `ime` 参数。
