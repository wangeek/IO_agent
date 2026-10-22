# IO_Agent — ESP32 双向 I/O Agent

> 术语不熟先看 **[`docs/glossary.md`](docs/glossary.md)**(NVS / CDC / L0-L2 / SoftAP 等一律在那儿解释)。

目标:一块 ESP32-S3,插在"不能装软件的机器"上,形成 输入注入 + 回读验证 的闭环 agent。

定位(2026-10-20):ESP32 外设给 AI agent 提供 **物理硬件级输入**(HID 键鼠注入)
与**多形态回读输出**(驻守程序或硬件接口的截图、网络与文件读取),最大程度模拟
人工与机器交互,支撑自动化测试/运维;并以 **MCP 服务端** 形式直接接入 agent 工作流
(设备自带 MCP server,agent 主动连设备)。

## 当前进度(2026-10-24)

**M0 / M1 已完成并实板验证通过**(详见 `docs/implementation-plan.md` 的 M0 / M1 实测结果):

| 里程碑 | 状态 | 关键结论 |
|---|---|---|
| M0 环境与骨架 | ✅ | ESP-IDF v5.5.6 + 23/23 子模块双通道校验 + 固件编译/烧录通过(稀疏镜像 + 槽位化后 `io_agent.bin` 884,176 B) |
| M1 固件最小可枚举 | ✅ | 复合设备在 Windows 上同时枚举出 **键盘 + 鼠标 + 可移动磁盘**;E: 盘读写字节级一致;端点占用 3 个(IN 侧 2) |
| M1 拔插不崩 | ✅ | 实测 ABSENT→PRESENT 完整循环:主机侧消失/重现,**固件侧无复位**(uptime 连续)、链路掉线/恢复均被观测到、重新枚举成功 |
| T2 运行时下发(2026-10-22) | ✅ | 出厂镜像**不再内嵌** `watch.ps1`,只留 3 个设备管理槽 + 1 个宿主信箱(**槽位容量在固件里占 0 字节**);内容落 flash `script0`/`script1` A/B 快照(断电不丢),经 `PAYLOAD.BIN` 信箱下发(M5 后 MCP `deploy_payload` 直接调同一 API)。**真源/副本契约:flash 是唯一真源(宿主够不到),RAM disk 槽位只在开机时铺开;运行期下发只写 flash** → 与宿主零竞争。实板双槽 A/B 验收 **17 项全绿** |
| M2 注入链路(2026-10-22) | ✅ | 设备端 HID 注入打通:**远程 `act` 单次触发,Win+R 实测在主机上打开了运行框**(客观判据=枚举 Windows 顶层窗口标题)。US 布局表(95 字符)带自检 + 仓库内独立交叉校验脚本 `tools/check_us_layout.py`。**注入默认关**,只走远程显式触发(见决策表) |
| M5 远程链路(2026-10-22) | ✅ | 设备侧 MCP server 落地(HTTP + JSON-RPC 2.0):`/mcp` + `/ping` + `/blob/b0..3`,5 个工具(`device/info`/`act`/`observe`/`watcher/status`/`deploy_payload`)。实测闭环:远程下发 → flash → 开机 overlay → 设备只读解析 → HTTP 回传 **sha256 逐字节一致**;`act(hotkey win+r)` 目标机窗口列表新增「运行」。凭据走 NVS + UART0 控制台配网(**不进仓库/不进固件**)。mDNS 与 TLS-PSK 待做 |

- **实板**:DevKitC-1-N16R8(Flash 16MB + Octal PSRAM 8MB,esptool 实测确认)。
- **主机侧枚举**:`HID\VID_303A&PID_8001&MI_00&COL01`(HID Keyboard Device)、
  `&MI_00&COL02`(HID-compliant mouse)、`&MI_01`(USB Mass Storage Device)、
  卷 `IOAGENT`(FAT / Removable / 4MB)。
- **下一步**:M2 注入链路(键鼠动作序列本地定时执行)。

## 架构决策记录(已定,不重开)

| 决策 | 结论 |
|---|---|
| 芯片 | ESP32-S3(唯一同时有 USB Device HID + USB Host/OTG 的主流型号;C3/C6 无 USB Device HID) |
| USB 组合 | **HID + MSC 复合设备**,4/6 端点,剩 2 富余 |
| HID 接口数 | **单 HID 接口 + 复合报告描述符**(Report ID 1=键盘 / 2=鼠标),不实现 HID OUT。**M1 实测已确认(2026-10-24)**:Windows **会**按报告描述符把单 HID 接口拆成两个集合 —— `MI_00&COL01`=HID Keyboard Device、`MI_00&COL02`=HID-compliant mouse,两个都独立可用。**原顾虑(以为只会枚举成一个 HID 设备)已推翻**,描述符方案成立 |
| ESP32 → 机器(执行) | HID IN(键盘/鼠标注入) |
| 机器 → ESP32(数据) | **v2:主机写虚拟 U 盘(标准 MSC 主方向)+ ESP32 本地读 RAM disk**;ESP32 主动向主机塞数据/主机反向读盘已被证伪(MSC 缓存 + 设备无法发起传输),见 `docs/output-channel-design.md` |
| 回传干道 | **方向反转(2026-10-20 定案)**:agent **主动连设备**,设备端跑 MCP server(WiFi + mDNS 自动发现)。设备不外推数据,"帧回给 agent" = 设备读 RAM disk 后应请求直传。**带外的确切含义**:设备连的是你的网络,与目标机网络无关 → 目标机断网/隔离网照样可控 |
| 确认/ACK | U 盘文件即回执(RESP);**LED 通道退出主路径**,降为可选调试/心跳信号(2026-10-20 定案,见 `docs/design-review.md` 结论 4) |
| Output 分层 | L1 U 盘驻守脚本(默认,免安装)/ L0 硬件直采=P4 二代(远期,产品内唯一硬件路线)/ ~~L2 目标机有网直连~~(**已作废**:拓扑反转为 agent 主动连设备)。**一体化形态约束:外置采集棒等配件堆方案不入实施计划** |
| MCP 宿主 | **设备端(ESP32)自跑 MCP server**:Streamable HTTP + JSON-RPC 2.0,工具集 `act`/`observe`/`watcher`/`device`/**`deploy_payload`**(T2 运行时下发,2026-10-22 增);帧用 blob 返回不 base64。兜底:极简 HTTP API + host 侧薄适配器 |
| 指标 | 稳定形态全屏截图 ≤1s(端到端:等新帧 ≤500ms + 读盘 ~10ms + WiFi 传输 50-100ms ≈ 0.6-0.7s);bootstrap ≤10s 无人工;稳态 USB 写入 <300KB/s |
| 采集方式 | **GDI `CopyFromScreen`**(一代保传统桌面用途,Windows 自带、零安装、最快);DXGI Desktop Duplication 后续可插拔(采集接口缝已留)。真独占全屏可能黑帧 = 已知边界 |
| 配网/绑定 | 设备只需 **WiFi 凭据 + 一把 PSK**:预置(默认)/ SoftAP + 手机(换网)/ 人工投送 `dev.cfg`。**TLS-PSK 双向认证**(设备验证 agent),mDNS 自动发现,不用配 IP |
| bootstrap | Stage 0:上电自检 → 连 WiFi → mDNS 广播(agent 可发现)→ Stage 1:按策略用 HID 拉起目标机驻守脚本 → Stage 2:agent 连上并配对 + 能力自陈 → 之后按目标机器能力降级选通道 |
| 固件组织 | 每级 = 编译期开关 + 能力注册表;状态机只查"我现在有什么",不假设某级存在 |
| USB 链路活性判据 | **`tud_ready()` = `mounted && !suspended`**,不靠 `TINYUSB_EVENT_DETACHED`。**M1 实测确认(2026-10-24)**:ESP32-S3 的 USB 是 Synopsys DWC2 内核,TinyUSB 的 DWC2 device 模式**不实现设备断开检测**(`dcd_dwc2.c` 里 `GINTSTS_DISCINT` 仍是 TODO,唯一的 UNPLUGGED 来源 `GOTGINT_SEDET` 需要 OTG+VBUS 感知,devkit 没接)→ 物理拔线实际只上报 **SUSPEND**,`TINYUSB_EVENT_DETACHED` **永不触发**。实测拔线时 `mounted=1 suspended=1`,`tud_mounted()` 不会翻转,所以也不能用它。重新插上后主机发 SOF,SOF 处理器清掉 `suspended` → 链路自动恢复(实测 140 ms) |
| 描述符 | 自写(TinyUSB 层),**不用 Arduino 便利库** — 后者常不实现 HID OUT 端点、且复合设备端点分配不受控(LED 已退出主路径,端点分配仍是硬理由) |
| 构建/依赖 | **离线可复现**:esp_tinyusb 2.4.0 + tinyusb 0.21.0~2 **vendored** 到 `firmware/components/`,禁用 IDF 组件管理器(`set(ENV{IDF_COMPONENT_MANAGER} 0)`)。IDF 子模块用 `tools/fetch_idf_submodules.py` 走 **GitHub codeload tarball** 离线抓取并做逐文件 SHA-1 校验 —— 本机代理 `127.0.0.1:7897` 会随机截断 git smart-HTTP 与大文件,`git clone` 不可靠(vendored 组件的 3 处补丁见 `firmware/components/README-vendor-patches.md`) |
| RAM disk 体积 | **出厂镜像用稀疏格式 + 槽位化(2026-10-22)**：卷容量由 BPB 声明(4MB),镜像文件只写到最后一个已用扇区(**97 扇区 / 49,664 B**),固件启动时在 PSRAM 开满 4MB 卷并把尾部补零 → 内嵌镜像 **4MB → 48KB**,`io_agent.bin` **约 4.42MB → 884,176 B**。宿主看到的仍是完整 4MB 可移动卷(实测 `size=4,144,640`),设备端依然零 FAT 逻辑。**"AI 现场就能生成的启动文件"不占固件位置** —— 出厂镜像只留薄引导槽:3 个设备管理槽(`WATCH.PS1` 32K / `AGENT.SH` 32K / `AGENT.EXE` 64K)+ 1 个宿主信箱(`PAYLOAD.BIN` 128K),**槽位簇从卷末尾往前分配**,簇链与目录项进镜像、**簇内容不进镜像** → 槽位容量在固件里占 **0 字节** |
| Flash 分区 | `nvs`24K / `phy_init`4K / `otadata`8K / `ota_0` **2MB** / `ota_1` **2MB** / `script0`512K / `script1`512K / 余 10.9MB。OTA 双槽从 6MB 缩到 2MB(app 实占 **884,176 B**,余 1,212,976 B(57.8%);`script0/1` 是 T2 的运行时 payload 仓(A/B),断电不丢 |
| watcher 启动 | **按卷标解析,不写死盘符(2026-10-22)**:dongle 拔线即断电,重入后盘符可能变。设备 HID 敲 `Win+R` 注入 `cmd /c for %d in (I O A G E N T) do @if exist %d:\WATCH.PS1 start "" powershell -ExecutionPolicy Bypass -WindowStyle Hidden %d:\WATCH.PS1`,注入后轮询 RAM disk 里的 `CAP.TXT` 判定拉起成功,未出现则 2s 间隔重试(最多 5 次) |
| 注入触发方式 | **默认关,只走远程显式触发(2026-10-22 事故后定案)**。往用户机器注入按键是**破坏性操作**:目标机若在锁屏,按键会被当成 PIN 输入 → 持续失败 → 触发账户防暴力锁定(实测事故)。因此 `IO_LAUNCHER_AUTOSTART` 默认 **0**(开机不自动拉起),注入只能由远程 `act` **单次触发、零重试**(失败不自动重发 —— 对"打系统快捷键"这类动作,重试=持续破坏)。`act` 默认回传注入**前后两张截图 URL**,由 agent 自己判定生效。验收放虚拟机/靶机;主力机上只能"人在场 + 知情 + 手动单次触发" |

## 已知硬约束

- S3 的 USB-OTG 与 USB-Serial-JTAG 共享一个 PHY:启用 HID 后原生串口调试口消失,日志走 UART0。
- **USB 拔线检测不可用**:ESP32-S3 用的是 Synopsys DWC2 内核,TinyUSB 的 DWC2 device 模式
  **没有实现设备断开检测**,`TINYUSB_EVENT_DETACHED` 永远不触发;拔线只表现为总线 SUSPEND。
  → "主机还在不在"只能看 `tud_ready()`(mounted && !suspended),详见决策表「USB 链路活性判据」。
  连带影响:M2 注入链路的中止条件不能挂在 DETACHED 事件上。
- **端点预算**:S2/S3 控制器共 7 个端点(含 EP0)、最多 5 个 IN(含 EP0)→ 6 个可配置。
  **实测占用 3 个**(EP0 + `0x81` HID IN + `0x02` MSC OUT + `0x82` MSC IN,IN 侧 2 个),
  富余 IN 2 / OUT 3;**USB CDC 塞不下**(需 3 个)→ "USB 串口当数据回传"没有退路。
- **flash encryption / secure boot 会禁用 ROM 里的 USB-OTG 栈** → 本项目 USB 口就是产品主接口,
  量产固件**必须走 WiFi OTA**;开发期不开加密(否则每次更新都要全量擦除)。
- LED 通道约 3–9 bit/s,只做 ACK/心跳,不做数据。
- macOS 多数版本不同步下发 LED 报告 → LED 通道探测失败时必须有降级分支。
- FAT 无 FIFO 语义,`echo > file` 是 fire-and-forget → 回执走 RESP 文件(LED 已退出主路径)。
- ESP32 在 FAT 上主动建文件易损坏 → ESP32 只读文件,写回交给主机。
- ESP32 不能向主机发起任何数据(USB 主从 + FAT 缓存)→ 配网/绑定只能"主机推送到盘上"。
- **RAM disk 每次上电恢复出厂镜像**:内容随断电消失。dongle 形态拔线即断电 →
  任何运行时下发到 RAM 的内容都会丢,**需要留存的东西必须落 flash**(`script0`/`script1` 仓)。
- **重入后盘符不保证复用**:dongle 重入是冷启动,Windows 可能分配到别的盘符 →
  宿主侧按**卷标**定位(`IOAGENT`),设备侧注入的启动命令也按卷标解析(见决策表「watcher 启动」)。

## 目录

- `docs/glossary.md` — **术语表**(读其它文档前先扫一眼)
- `docs/agent-interface-mcp.md` — **Agent 接口层:手眼模型**(act/observe 原语、N1-N7 裁定、UAC 补充、采集方式定案)
- `docs/output-channel-design.md` — Output 通道设计 v2(三层架构、延迟预算、屏幕即接口模式)
- `docs/connectivity-and-binding.md` — **连接与绑定 v3:设备即 MCP 服务端**(拓扑、鉴权方向、配网、MCP on-device 可行性与风险、连带作废清单)
- `docs/hardware-selection.md` — 硬件选型与形态(N16R8 锁定、三阶段板形、供电/天线/日志策略)
- `docs/implementation-plan.md` — **实施计划**(仓库骨架、里程碑、契约冻结清单)
- `docs/design-review.md` — 2026-10-20 设计评审:8 条发现与定案结论(含当日拓扑反转)
- `docs/msc-watcher-protocol.md` — v1 文件协议,回读部分已被 v2 取代;CMD/RESP 约定平移至 v2,**LED 段落已作废**
